#include "app/AppPaths.h"
#include "features/ProviderClient.h"
#include "indexer/Dsp.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <QUuid>
#include <QVariant>
#include <QtEndian>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include <chromaprint.h>

#ifdef Q_OS_LINUX
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {

constexpr int kSchemaVersion = 5;
constexpr int kProcessTimeoutMs = 10 * 60 * 1000;
constexpr int kProviderStatusTimeoutMs = 30'000;
constexpr double kChromaprintBerThreshold = 0.15;
constexpr int kChromaprintOffsetFrames = 3;
constexpr qint64 kDurationBucketMs = 2'000;
constexpr const char *kExtractor = "muzaiten-dsp";
constexpr const char *kSemanticModel = "laion-clap-music-audioset";
constexpr const char *kSemanticCheckpoint = "music_audioset_epoch_15_esc_90.14.pt";
constexpr const char *kSemanticCheckpointSha256 = "fae3e9c087f2909c28a09dc31c8dfcdacbc42ba44c70e972b58c1bd1caf6dedd";
constexpr const char *kSemanticFeatureRevision = "clap-htsat-base-audio-window-v1";
std::atomic_bool g_stopRequested = false;
bool g_progressJsonl = false;
QString g_progressRequestId;

enum class Stage {
    Identity,
    Features,
    All,
};

enum class Power {
    Unspecified,
    Background,
    Balanced,
    Turbo,
};

struct RefreshOptions {
    QString libraryPath;
    QString featuresPath;
    QString statePath;
    QString providerPath;
    Stage stage = Stage::All;
    Power power = Power::Unspecified;
    int limit = -1;
    int jobs = 0;
    bool json = false;
    bool progress = false;
    bool verbose = false;
    std::optional<bool> semantic;
};

struct EffectivePower {
    Power power = Power::Turbo;
    int jobs = 1;
};

struct StatusOptions {
    QString featuresPath;
    QString statePath;
    QString providerPath;
    bool json = false;
};

struct ProviderOptions {
    QString featuresPath;
    QString statePath;
    QString providerPath;
    QString device = QStringLiteral("auto");
    QString text;
    bool json = false;
    bool progress = false;
    bool force = false;
};

struct Candidate {
    QString path;
    qint64 mtime = 0;
    qint64 size = 0;
    // Group membership before this path was selected for re-analysis. The
    // file upsert clears content_group_id; retaining the old id in memory
    // lets the normal scan recompute only the component whose changed member
    // may have split or merged. -1 means a genuinely new/previously
    // ungrouped path.
    qint64 previousGroupId = -1;
};

struct DecodedAudio {
    QByteArray pcm;
    std::vector<float> samples;
    qint64 durationMs = 0;
};

struct StageTimings {
    qint64 decodeMs = 0;
    qint64 hashMs = 0;
    qint64 dspMs = 0;
    qint64 fpMs = 0;
};

struct TimingAccumulator {
    std::vector<qint64> decode;
    std::vector<qint64> hash;
    std::vector<qint64> dsp;
    std::vector<qint64> fp;

    void reserve(std::size_t count)
    {
        decode.reserve(count);
        hash.reserve(count);
        dsp.reserve(count);
        fp.reserve(count);
    }

    void add(const StageTimings &timings)
    {
        decode.push_back(timings.decodeMs);
        hash.push_back(timings.hashMs);
        dsp.push_back(timings.dspMs);
        fp.push_back(timings.fpMs);
    }
};

struct FileAnalysis {
    Candidate candidate;
    qint64 durationMs = -1;
    QString decodeHash;
    QByteArray chromaprint;
    std::optional<Dsp::ScalarFeatures> scalars;
    QString status = QStringLiteral("decode_failed");
    StageTimings timings;
};

struct GroupRow {
    QString path;
    qint64 durationMs = 0;
    QString decodeHash;
    std::vector<qint32> chromaprint;
    qint64 oldGroupId = -1; // pre-regroup content_group_id; -1 = none
};

struct ScalarExtraction {
    bool known = false;
    Dsp::ScalarFeatures features;
};

class SqlConnection final {
public:
    SqlConnection()
        : m_name(QStringLiteral("muzaiten-features-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
        , m_database(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name))
    {
    }

    ~SqlConnection()
    {
        if (m_database.isValid()) {
            m_database.close();
        }
        m_database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_name);
    }

    SqlConnection(const SqlConnection &) = delete;
    SqlConnection &operator=(const SqlConnection &) = delete;

    QSqlDatabase &database() { return m_database; }

private:
    QString m_name;
    QSqlDatabase m_database;
};

QString readStateSetting(const QString &statePath, const QString &key, const QString &fallback = {})
{
    if (!QFileInfo::exists(statePath)) {
        return fallback;
    }
    SqlConnection connection;
    QSqlDatabase &database = connection.database();
    database.setDatabaseName(statePath);
    database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000"));
    if (!database.open()) {
        return fallback;
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    query.addBindValue(key);
    return query.exec() && query.next() ? query.value(0).toString() : fallback;
}

bool settingEnabled(const QString &value)
{
    const QString normalized = value.trimmed().toLower();
    return normalized == QLatin1String("1") || normalized == QLatin1String("true")
        || normalized == QLatin1String("yes") || normalized == QLatin1String("on");
}

[[noreturn]] void fail(const QString &message)
{
    throw std::runtime_error(message.toStdString());
}

class CommandError final : public std::runtime_error {
public:
    CommandError(int code, const QString &message)
        : std::runtime_error(message.toStdString())
        , m_code(code)
    {
    }

    int code() const { return m_code; }

private:
    int m_code;
};

[[noreturn]] void failWithCode(int code, const QString &message)
{
    throw CommandError(code, message);
}

void requestStop(int)
{
    g_stopRequested.store(true, std::memory_order_relaxed);
}

bool stopRequested()
{
    return g_stopRequested.load(std::memory_order_relaxed);
}

std::size_t hardwareThreads()
{
    return std::max<std::size_t>(1, std::thread::hardware_concurrency());
}

QString powerName(Power power)
{
    switch (power) {
    case Power::Background:
        return QStringLiteral("background");
    case Power::Balanced:
        return QStringLiteral("balanced");
    case Power::Turbo:
    case Power::Unspecified:
        return QStringLiteral("turbo");
    }
    return QStringLiteral("turbo");
}

EffectivePower resolvePower(const RefreshOptions &options)
{
    const std::size_t cores = hardwareThreads();
    const Power effectivePower = options.power == Power::Unspecified ? Power::Turbo : options.power;
    int mappedJobs = static_cast<int>(cores);
    if (effectivePower == Power::Background) {
        mappedJobs = static_cast<int>(std::max<std::size_t>(1, cores / 4));
    } else if (effectivePower == Power::Balanced) {
        mappedJobs = static_cast<int>(std::max<std::size_t>(2, cores / 2));
    }
    if (options.jobs > 0) {
        mappedJobs = options.jobs;
    }
    return {effectivePower, std::max(1, mappedJobs)};
}

void applyPowerPriority(Power power)
{
#ifdef Q_OS_LINUX
    if (power == Power::Background) {
        setpriority(PRIO_PROCESS, 0, 19);
        constexpr int ioprioWhoProcess = 1;
        constexpr int ioprioClassIdle = 3;
        constexpr int ioprioClassShift = 13;
        syscall(SYS_ioprio_set, ioprioWhoProcess, 0, ioprioClassIdle << ioprioClassShift);
    } else if (power == Power::Balanced) {
        setpriority(PRIO_PROCESS, 0, 10);
    }
#else
    Q_UNUSED(power);
#endif
}

qint64 elapsedMs(std::chrono::steady_clock::time_point started)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - started)
        .count();
}

void execSql(QSqlDatabase &database, const QString &sql)
{
    QSqlQuery query(database);
    if (!query.exec(sql)) {
        fail(query.lastError().text() + QStringLiteral(": ") + sql);
    }
}

void prepareOrFail(QSqlQuery &query, const QString &sql)
{
    if (!query.prepare(sql)) {
        fail(query.lastError().text() + QStringLiteral(": ") + sql);
    }
}

void execPrepared(QSqlQuery &query)
{
    if (!query.exec()) {
        fail(query.lastError().text());
    }
}

class SqlTransaction final {
public:
    explicit SqlTransaction(QSqlDatabase &database, const QString &context)
        : m_database(database)
        , m_context(context)
    {
        if (!m_database.transaction()) {
            fail(QStringLiteral("starting %1: %2").arg(m_context, m_database.lastError().text()));
        }
    }

    ~SqlTransaction()
    {
        if (!m_committed) {
            m_database.rollback();
        }
    }

    void commit()
    {
        if (!m_database.commit()) {
            fail(QStringLiteral("committing %1: %2").arg(m_context, m_database.lastError().text()));
        }
        m_committed = true;
    }

private:
    QSqlDatabase &m_database;
    QString m_context;
    bool m_committed = false;
};

QVariant optionalVariant(const std::optional<double> &value)
{
    return value ? QVariant(*value) : QVariant();
}

QVariant scalarVariant(const std::optional<Dsp::ScalarFeatures> &features, double Dsp::ScalarFeatures::*member)
{
    return features ? QVariant((*features).*member) : QVariant();
}

// Shared by the group-features and file_features upserts — both tables carry
// the same ten scalar columns after their key.
void bindScalarRow(QSqlQuery &query, const QVariant &key, const std::optional<Dsp::ScalarFeatures> &features)
{
    int bindIndex = 0;
    query.bindValue(bindIndex++, key);
    query.bindValue(bindIndex++, features ? optionalVariant(features->tempoBpm) : QVariant());
    query.bindValue(bindIndex++, features ? optionalVariant(features->loudnessLufs) : QVariant());
    query.bindValue(bindIndex++, features ? optionalVariant(features->loudnessStdDb) : QVariant());
    query.bindValue(bindIndex++, scalarVariant(features, &Dsp::ScalarFeatures::spectralCentroidMeanHz));
    query.bindValue(bindIndex++, scalarVariant(features, &Dsp::ScalarFeatures::spectralCentroidStdHz));
    query.bindValue(bindIndex++, scalarVariant(features, &Dsp::ScalarFeatures::spectralFlatnessMean));
    query.bindValue(bindIndex++, scalarVariant(features, &Dsp::ScalarFeatures::zeroCrossingRate));
    query.bindValue(bindIndex++, scalarVariant(features, &Dsp::ScalarFeatures::onsetRateHz));
    query.bindValue(bindIndex++, features ? optionalVariant(features->energy) : QVariant());
    query.bindValue(bindIndex++, QString::fromLatin1(kExtractor));
    query.bindValue(bindIndex, QString::fromLatin1(Dsp::kDspVersion));
}

QSqlDatabase openFeaturesDatabase(const QString &path, SqlConnection &connection)
{
    QFileInfo info(path);
    if (!info.dir().exists() && !QDir().mkpath(info.dir().absolutePath())) {
        fail(QStringLiteral("could not create %1").arg(info.dir().absolutePath()));
    }

    QSqlDatabase &database = connection.database();
    database.setDatabaseName(path);
    if (!database.open()) {
        fail(QStringLiteral("opening %1: %2").arg(path, database.lastError().text()));
    }
    execSql(database, QStringLiteral("PRAGMA journal_mode=WAL"));
    execSql(database, QStringLiteral("PRAGMA synchronous=NORMAL"));
    execSql(database, QStringLiteral("PRAGMA busy_timeout=5000"));
    return database;
}

QStringList tableColumns(QSqlDatabase &database, const QString &table)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("PRAGMA table_info(%1)").arg(table));
    execPrepared(query);
    QStringList columns;
    while (query.next()) {
        columns.push_back(query.value(1).toString());
    }
    return columns;
}

bool featuresTableHasCurrentShape(QSqlDatabase &database)
{
    const QStringList columns = tableColumns(database, QStringLiteral("features"));
    const QStringList expected{
        QStringLiteral("content_group_id"),
        QStringLiteral("tempo_bpm"),
        QStringLiteral("loudness_lufs"),
        QStringLiteral("loudness_std_db"),
        QStringLiteral("spectral_centroid_mean_hz"),
        QStringLiteral("spectral_centroid_std_hz"),
        QStringLiteral("spectral_flatness_mean"),
        QStringLiteral("zero_crossing_rate"),
        QStringLiteral("onset_rate_hz"),
        QStringLiteral("energy"),
        QStringLiteral("extractor"),
        QStringLiteral("version"),
    };
    return columns == expected;
}

void initSemanticSchema(QSqlDatabase &database)
{
    SqlTransaction transaction(database, QStringLiteral("semantic schema migration"));
    execSql(database, QStringLiteral(
                          "CREATE TABLE IF NOT EXISTS semantic_generations("
                          " id INTEGER PRIMARY KEY AUTOINCREMENT,"
                          " capability TEXT NOT NULL,"
                          " model TEXT NOT NULL,"
                          " checkpoint_sha256 TEXT NOT NULL,"
                          " feature_revision TEXT NOT NULL,"
                          " vector_dim INTEGER NOT NULL,"
                          " provider_path TEXT,"
                          " provider_version TEXT,"
                          " created_at INTEGER NOT NULL,"
                          " completed_at INTEGER,"
                          " active INTEGER NOT NULL DEFAULT 0,"
                          " UNIQUE(capability, model, checkpoint_sha256, feature_revision, vector_dim))"));

    const bool legacyEmbeddings = database.tables().contains(QStringLiteral("embeddings"))
        && !tableColumns(database, QStringLiteral("embeddings")).contains(QStringLiteral("generation_id"));
    const bool legacyNeighbors = database.tables().contains(QStringLiteral("track_neighbors"))
        && !tableColumns(database, QStringLiteral("track_neighbors")).contains(QStringLiteral("generation_id"));
    if (legacyEmbeddings) {
        execSql(database, QStringLiteral("ALTER TABLE embeddings RENAME TO embeddings_v4"));
    }
    if (legacyNeighbors) {
        execSql(database, QStringLiteral("ALTER TABLE track_neighbors RENAME TO track_neighbors_v4"));
    }
    execSql(database, QStringLiteral(
                          "CREATE TABLE IF NOT EXISTS embeddings("
                          " content_group_id INTEGER NOT NULL,"
                          " generation_id INTEGER NOT NULL,"
                          " dim INTEGER NOT NULL,"
                          " vector BLOB NOT NULL,"
                          " PRIMARY KEY(content_group_id, generation_id),"
                          " FOREIGN KEY(generation_id) REFERENCES semantic_generations(id))"));
    execSql(database, QStringLiteral(
                          "CREATE TABLE IF NOT EXISTS track_neighbors("
                          " content_group_id INTEGER NOT NULL,"
                          " neighbor_group_id INTEGER NOT NULL,"
                          " rank INTEGER NOT NULL,"
                          " cosine REAL NOT NULL,"
                          " generation_id INTEGER NOT NULL,"
                          " algorithm_revision TEXT NOT NULL,"
                          " top_k INTEGER NOT NULL,"
                          " PRIMARY KEY(content_group_id, generation_id, rank),"
                          " FOREIGN KEY(generation_id) REFERENCES semantic_generations(id))"));

    const bool hasLegacyEmbeddings = database.tables().contains(QStringLiteral("embeddings_v4"));
    const bool hasLegacyNeighbors = database.tables().contains(QStringLiteral("track_neighbors_v4"));
    if (!hasLegacyEmbeddings) {
        if (hasLegacyNeighbors) {
            execSql(database, QStringLiteral("DROP TABLE track_neighbors_v4"));
        }
        transaction.commit();
        return;
    }

    bool known = false;
    int dimension = 0;
    int rowCount = 0;
    {
        QSqlQuery provenance(database);
        if (!provenance.exec(QStringLiteral(
                "SELECT COUNT(DISTINCT model || char(0) || version || char(0) || dim),"
                " MIN(model), MIN(version), MIN(dim), COUNT(*) FROM embeddings_v4"))
            || !provenance.next()) {
            fail(provenance.lastError().text());
        }
        dimension = provenance.value(3).toInt();
        rowCount = provenance.value(4).toInt();
        known = provenance.value(0).toInt() == 1
            && provenance.value(1).toString() == QLatin1String(kSemanticModel)
            && provenance.value(2).toString() == QLatin1String(kSemanticCheckpoint)
            && dimension == 512;
    }
    if (rowCount > 0) {
        const QString model = known ? QLatin1String(kSemanticModel) : QStringLiteral("legacy-unknown");
        const QString checkpoint = known ? QLatin1String(kSemanticCheckpointSha256) : QStringLiteral("unknown");
        const QString revision = known ? QLatin1String(kSemanticFeatureRevision) : QStringLiteral("unknown");
        qint64 generationId = -1;
        {
            QSqlQuery existing(database);
            prepareOrFail(existing, QStringLiteral(
                "SELECT id FROM semantic_generations WHERE capability = 'clap' AND model = ?"
                " AND checkpoint_sha256 = ? AND feature_revision = ? AND vector_dim = ?"));
            existing.addBindValue(model);
            existing.addBindValue(checkpoint);
            existing.addBindValue(revision);
            existing.addBindValue(dimension);
            execPrepared(existing);
            if (existing.next()) {
                generationId = existing.value(0).toLongLong();
            }
        }
        if (generationId < 0) {
            QSqlQuery generation(database);
            prepareOrFail(generation, QStringLiteral(
                "INSERT INTO semantic_generations(capability, model, checkpoint_sha256, feature_revision,"
                " vector_dim, provider_path, provider_version, created_at, completed_at, active)"
                " VALUES('clap', ?, ?, ?, ?, 'legacy-v4-migration', NULL, ?, ?, ?)"));
            generation.addBindValue(model);
            generation.addBindValue(checkpoint);
            generation.addBindValue(revision);
            generation.addBindValue(dimension);
            generation.addBindValue(QDateTime::currentSecsSinceEpoch());
            generation.addBindValue(known ? QDateTime::currentSecsSinceEpoch() : QVariant());
            generation.addBindValue(known ? 1 : 0);
            execPrepared(generation);
            generationId = generation.lastInsertId().toLongLong();
        }

        execSql(database, QStringLiteral("UPDATE semantic_generations SET active = CASE WHEN id = %1 THEN %2 ELSE 0 END")
                              .arg(generationId)
                              .arg(known ? 1 : 0));
        const auto countRows = [&](const QString &sql) {
            QSqlQuery query(database);
            if (!query.exec(sql) || !query.next()) {
                fail(query.lastError().text());
            }
            return query.value(0).toLongLong();
        };
        if (countRows(QStringLiteral("SELECT COUNT(*) FROM embeddings WHERE generation_id = %1").arg(generationId))
            != rowCount) {
            execSql(database, QStringLiteral("DELETE FROM track_neighbors WHERE generation_id = %1").arg(generationId));
            execSql(database, QStringLiteral("DELETE FROM embeddings WHERE generation_id = %1").arg(generationId));
            execSql(database, QStringLiteral(
                                  "INSERT INTO embeddings(content_group_id, generation_id, dim, vector)"
                                  " SELECT content_group_id, %1, dim, vector FROM embeddings_v4")
                                  .arg(generationId));
        }
        if (known && hasLegacyNeighbors) {
            const qint64 legacyNeighborCount = countRows(QStringLiteral("SELECT COUNT(*) FROM track_neighbors_v4"));
            if (countRows(QStringLiteral("SELECT COUNT(*) FROM track_neighbors WHERE generation_id = %1")
                              .arg(generationId)) != legacyNeighborCount) {
                execSql(database, QStringLiteral("DELETE FROM track_neighbors WHERE generation_id = %1")
                                      .arg(generationId));
                execSql(database, QStringLiteral(
                                      "INSERT INTO track_neighbors(content_group_id, neighbor_group_id, rank, cosine,"
                                      " generation_id, algorithm_revision, top_k)"
                                      " SELECT content_group_id, neighbor_group_id, rank, cosine, %1,"
                                      " 'cosine-blockwise-v1',"
                                      " (SELECT COALESCE(MAX(rank), 0) FROM track_neighbors_v4)"
                                      " FROM track_neighbors_v4")
                                      .arg(generationId));
            }
        }
    }
    execSql(database, QStringLiteral("DROP TABLE embeddings_v4"));
    if (hasLegacyNeighbors) {
        execSql(database, QStringLiteral("DROP TABLE track_neighbors_v4"));
    }
    transaction.commit();
}

void initSchema(QSqlDatabase &database)
{
    execSql(database, QStringLiteral(
                          "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT)"));

    execSql(database, QStringLiteral(
                          "CREATE TABLE IF NOT EXISTS files("
                          " path TEXT PRIMARY KEY,"
                          " mtime INTEGER NOT NULL,"
                          " size INTEGER NOT NULL,"
                          " duration_ms INTEGER,"
                          " decode_hash TEXT,"
                          " chromaprint_fp BLOB,"
                          " content_group_id INTEGER,"
                          " analyzed_at INTEGER NOT NULL,"
                          " status TEXT NOT NULL DEFAULT 'ok')"));
    execSql(database, QStringLiteral(
                          "CREATE TABLE IF NOT EXISTS content_groups(id INTEGER PRIMARY KEY AUTOINCREMENT)"));

    // Drop on SHAPE mismatch only, never on the schema_version number: the
    // v3 -> v4 upgrade is purely additive (file_features below), and a
    // version-based drop here would wipe every existing store's feature
    // rows on first open after an upgrade. Only the pre-v3 bliss-era table
    // fails the shape check.
    if (!featuresTableHasCurrentShape(database)) {
        execSql(database, QStringLiteral("DROP TABLE IF EXISTS features"));
    }
    execSql(database, QStringLiteral(
                          "CREATE TABLE IF NOT EXISTS features("
                          " content_group_id INTEGER PRIMARY KEY,"
                          " tempo_bpm REAL,"
                          " loudness_lufs REAL,"
                          " loudness_std_db REAL,"
                          " spectral_centroid_mean_hz REAL,"
                          " spectral_centroid_std_hz REAL,"
                          " spectral_flatness_mean REAL,"
                          " zero_crossing_rate REAL,"
                          " onset_rate_hz REAL,"
                          " energy REAL,"
                          " extractor TEXT NOT NULL,"
                          " version TEXT NOT NULL)"));

    // Per-file scalar cache (schema v4): the durable form of the in-memory
    // `extracted` map. Written whenever a file is decoded and analyzed, so
    // the group feature phase can copy a representative's scalars instead
    // of decoding the audio a second time. NULL scalars mean "analyzed,
    // no features" (short input) — a known result, not missing work.
    execSql(database, QStringLiteral(
                          "CREATE TABLE IF NOT EXISTS file_features("
                          " path TEXT PRIMARY KEY,"
                          " tempo_bpm REAL,"
                          " loudness_lufs REAL,"
                          " loudness_std_db REAL,"
                          " spectral_centroid_mean_hz REAL,"
                          " spectral_centroid_std_hz REAL,"
                          " spectral_flatness_mean REAL,"
                          " zero_crossing_rate REAL,"
                          " onset_rate_hz REAL,"
                          " energy REAL,"
                          " extractor TEXT NOT NULL,"
                          " version TEXT NOT NULL)"));

    initSemanticSchema(database);

    QSqlQuery created(database);
    prepareOrFail(created, QStringLiteral("INSERT OR IGNORE INTO meta(key, value) VALUES('created_at', ?)"));
    created.addBindValue(QString::number(QDateTime::currentSecsSinceEpoch()));
    execPrepared(created);

    QSqlQuery schema(database);
    prepareOrFail(schema, QStringLiteral(
                              "INSERT INTO meta(key, value) VALUES('schema_version', ?) "
                              "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    schema.addBindValue(QString::number(kSchemaVersion));
    execPrepared(schema);

    QSqlQuery indexer(database);
    prepareOrFail(indexer, QStringLiteral(
                               "INSERT INTO meta(key, value) VALUES('indexer_version', 'cpp') "
                               "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    execPrepared(indexer);

    QSqlQuery dsp(database);
    prepareOrFail(dsp, QStringLiteral(
                           "INSERT INTO meta(key, value) VALUES('dsp_version', ?) "
                           "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    dsp.addBindValue(QString::fromLatin1(Dsp::kDspVersion));
    execPrepared(dsp);

    execSql(database, QStringLiteral("DELETE FROM meta WHERE key = 'bliss_version'"));
}

std::vector<Candidate> loadCandidates(const QString &libraryPath, int limit)
{
    SqlConnection connection;
    QSqlDatabase &database = connection.database();
    database.setDatabaseName(libraryPath);
    database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000"));
    if (!database.open()) {
        fail(QStringLiteral("opening library %1: %2").arg(libraryPath, database.lastError().text()));
    }

    QString sql = QStringLiteral(
        "SELECT path, file_mtime, file_size FROM tracks "
        "WHERE COALESCE(missing, 0) = 0 AND path IS NOT NULL AND path <> '' "
        "ORDER BY path");
    if (limit > 0) {
        sql += QStringLiteral(" LIMIT %1").arg(limit);
    }

    QSqlQuery query(database);
    if (!query.exec(sql)) {
        fail(query.lastError().text() + QStringLiteral(": ") + sql);
    }
    std::vector<Candidate> candidates;
    while (query.next()) {
        candidates.push_back({
            query.value(0).toString(),
            query.value(1).toLongLong(),
            query.value(2).toLongLong(),
        });
    }
    return candidates;
}

std::pair<std::vector<Candidate>, int> splitPending(QSqlDatabase &database,
                                                    const std::vector<Candidate> &candidates)
{
    struct KnownFile {
        qint64 mtime = 0;
        qint64 size = 0;
        qint64 groupId = -1;
    };
    QHash<QString, KnownFile> known;
    QSqlQuery existing(database);
    if (!existing.exec(QStringLiteral(
            "SELECT path, mtime, size, content_group_id FROM files"))) {
        fail(existing.lastError().text());
    }
    while (existing.next()) {
        known.insert(existing.value(0).toString(), {
            existing.value(1).toLongLong(),
            existing.value(2).toLongLong(),
            existing.value(3).isNull() ? -1 : existing.value(3).toLongLong(),
        });
    }

    std::vector<Candidate> pending;
    int skipped = 0;
    for (const Candidate &candidate : candidates) {
        const auto it = known.constFind(candidate.path);
        if (it != known.constEnd() && it->mtime == candidate.mtime && it->size == candidate.size) {
            ++skipped;
        } else {
            Candidate pendingCandidate = candidate;
            if (it != known.constEnd()) {
                pendingCandidate.previousGroupId = it->groupId;
            }
            pending.push_back(std::move(pendingCandidate));
        }
    }
    return {pending, skipped};
}

QString compactProcessError(QProcess &process)
{
    QString value = QString::fromUtf8(process.readAllStandardError()).trimmed();
    value.replace(QLatin1Char('\n'), QLatin1Char(' '));
    value.replace(QLatin1Char('\r'), QLatin1Char(' '));
    if (value.size() > 500) {
        value = value.left(500).trimmed() + QStringLiteral("...");
    }
    return value;
}

// keepRawPcm retains the raw f32le byte stream alongside the float samples;
// only the identity path needs it (decode_hash is defined over those bytes).
// Callers that just analyze should drop it — for an hour-long track the byte
// copy alone is ~320 MB per worker.
DecodedAudio decodeCanonical(const QString &path, bool keepRawPcm = true)
{
    QProcess process;
    process.start(QStringLiteral("ffmpeg"), {
        QStringLiteral("-hide_banner"),
        QStringLiteral("-loglevel"),
        QStringLiteral("error"),
        QStringLiteral("-i"),
        path,
        QStringLiteral("-vn"),
        QStringLiteral("-f"),
        QStringLiteral("f32le"),
        QStringLiteral("-ac"),
        QStringLiteral("1"),
        QStringLiteral("-ar"),
        QString::number(Dsp::kSampleRateHz),
        QStringLiteral("-"),
    });
    if (!process.waitForStarted(5000)) {
        fail(QStringLiteral("spawning ffmpeg for %1").arg(path));
    }
    if (!process.waitForFinished(kProcessTimeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        fail(QStringLiteral("ffmpeg timed out for %1").arg(path));
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString detail = compactProcessError(process);
        fail(QStringLiteral("ffmpeg failed for %1%2").arg(path, detail.isEmpty() ? QString() : QStringLiteral(": ") + detail));
    }

    QByteArray pcm = process.readAllStandardOutput();
    if (pcm.isEmpty() || pcm.size() % static_cast<qsizetype>(sizeof(float)) != 0) {
        fail(QStringLiteral("ffmpeg produced invalid f32le PCM for %1").arg(path));
    }

    const qsizetype sampleCount = pcm.size() / static_cast<qsizetype>(sizeof(float));
    std::vector<float> samples;
    samples.resize(static_cast<std::size_t>(sampleCount));
    if constexpr (std::endian::native == std::endian::little) {
        std::memcpy(samples.data(), pcm.constData(), static_cast<std::size_t>(pcm.size()));
    } else {
        const auto *data = reinterpret_cast<const uchar *>(pcm.constData());
        for (qsizetype index = 0; index < sampleCount; ++index) {
            const quint32 raw = qFromLittleEndian<quint32>(
                data + index * static_cast<qsizetype>(sizeof(quint32)));
            float sample = 0.0F;
            static_assert(sizeof(sample) == sizeof(raw));
            std::memcpy(&sample, &raw, sizeof(sample));
            samples[static_cast<std::size_t>(index)] = sample;
        }
    }

    DecodedAudio decoded;
    if (keepRawPcm) {
        decoded.pcm = std::move(pcm);
    } else {
        pcm = QByteArray();
    }
    decoded.samples = std::move(samples);
    decoded.durationMs = static_cast<qint64>(
        std::llround(static_cast<double>(sampleCount) * 1000.0 / static_cast<double>(Dsp::kSampleRateHz)));
    return decoded;
}

QByteArray encodeRawFingerprint(const uint32_t *fingerprint, int size)
{
    QByteArray blob;
    blob.resize(static_cast<qsizetype>(size * static_cast<int>(sizeof(qint32))));
    auto *data = reinterpret_cast<uchar *>(blob.data());
    for (int index = 0; index < size; ++index) {
        qToLittleEndian<qint32>(static_cast<qint32>(fingerprint[index]),
                                data + static_cast<qsizetype>(index) * static_cast<qsizetype>(sizeof(qint32)));
    }
    return blob;
}

QByteArray chromaprintFingerprint(const std::vector<float> &samples, int sampleRate)
{
    const std::size_t cappedSamples = std::min<std::size_t>(
        samples.size(), static_cast<std::size_t>(sampleRate) * 120U);
    std::vector<int16_t> pcm;
    pcm.reserve(cappedSamples);
    for (std::size_t index = 0; index < cappedSamples; ++index) {
        const float clamped = std::clamp(samples[index], -1.0F, 1.0F);
        pcm.push_back(static_cast<int16_t>(std::lrint(static_cast<double>(clamped) * 32767.0)));
    }

    ChromaprintContext *ctx = chromaprint_new(CHROMAPRINT_ALGORITHM_DEFAULT);
    if (ctx == nullptr) {
        fail(QStringLiteral("creating chromaprint context"));
    }
    auto freeContext = [&]() {
        chromaprint_free(ctx);
        ctx = nullptr;
    };
    if (!chromaprint_start(ctx, sampleRate, 1)) {
        freeContext();
        fail(QStringLiteral("starting chromaprint"));
    }
    if (!pcm.empty()
        && !chromaprint_feed(ctx, pcm.data(), static_cast<int>(pcm.size()))) {
        freeContext();
        fail(QStringLiteral("feeding chromaprint"));
    }
    if (!chromaprint_finish(ctx)) {
        freeContext();
        fail(QStringLiteral("finishing chromaprint"));
    }

    uint32_t *raw = nullptr;
    int size = 0;
    if (!chromaprint_get_raw_fingerprint(ctx, &raw, &size) || raw == nullptr || size <= 0) {
        if (raw != nullptr) {
            chromaprint_dealloc(raw);
        }
        freeContext();
        fail(QStringLiteral("chromaprint returned an empty fingerprint"));
    }
    const QByteArray encoded = encodeRawFingerprint(raw, size);
    chromaprint_dealloc(raw);
    freeContext();
    return encoded;
}

FileAnalysis analyzeCandidate(const Candidate &candidate)
{
    FileAnalysis analysis;
    analysis.candidate = candidate;
    try {
        const auto decodeStarted = std::chrono::steady_clock::now();
        DecodedAudio decoded = decodeCanonical(candidate.path);
        analysis.timings.decodeMs = elapsedMs(decodeStarted);
        analysis.durationMs = decoded.durationMs;

        const auto hashStarted = std::chrono::steady_clock::now();
        analysis.decodeHash = QString::fromLatin1(
            QCryptographicHash::hash(decoded.pcm, QCryptographicHash::Sha256).toHex());
        analysis.timings.hashMs = elapsedMs(hashStarted);
        // The raw byte copy exists only for the hash above; release it so the
        // worker holds one PCM copy, not two, through DSP and Chromaprint.
        decoded.pcm = QByteArray();

        const auto dspStarted = std::chrono::steady_clock::now();
        analysis.scalars = Dsp::analyze(decoded.samples, Dsp::kSampleRateHz);
        analysis.timings.dspMs = elapsedMs(dspStarted);
        try {
            const auto fpStarted = std::chrono::steady_clock::now();
            analysis.chromaprint = chromaprintFingerprint(decoded.samples, Dsp::kSampleRateHz);
            analysis.timings.fpMs = elapsedMs(fpStarted);
            analysis.status = QStringLiteral("ok");
        } catch (const std::exception &) {
            analysis.status = QStringLiteral("fp_failed");
        }
    } catch (const std::exception &) {
        analysis.status = QStringLiteral("decode_failed");
    }
    return analysis;
}

void analyzePending(const std::vector<Candidate> &pending, int jobs,
                    const std::function<void(FileAnalysis &&, int, int)> &completion)
{
    if (pending.empty()) {
        return;
    }
    const std::size_t workerCount = jobs > 0 ? static_cast<std::size_t>(jobs) : std::thread::hardware_concurrency();
    const std::size_t boundedWorkers = std::max<std::size_t>(1, workerCount);
    const int total = static_cast<int>(pending.size());

    std::atomic_size_t nextIndex = 0;
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<FileAnalysis> completed;
    std::size_t runningWorkers = boundedWorkers;

    std::vector<std::thread> workers;
    workers.reserve(boundedWorkers);
    for (std::size_t worker = 0; worker < boundedWorkers; ++worker) {
        workers.emplace_back([&]() {
            while (!stopRequested()) {
                const std::size_t index = nextIndex.fetch_add(1, std::memory_order_relaxed);
                if (index >= pending.size()) {
                    break;
                }
                FileAnalysis analysis = analyzeCandidate(pending[index]);
                {
                    std::lock_guard lock(mutex);
                    completed.push_back(std::move(analysis));
                }
                ready.notify_one();
            }
            {
                std::lock_guard lock(mutex);
                --runningWorkers;
            }
            ready.notify_one();
        });
    }

    int analyzed = 0;
    while (true) {
        std::unique_lock lock(mutex);
        ready.wait(lock, [&]() { return !completed.empty() || runningWorkers == 0; });
        while (!completed.empty()) {
            FileAnalysis analysis = std::move(completed.front());
            completed.pop_front();
            lock.unlock();
            ++analyzed;
            completion(std::move(analysis), analyzed, total);
            lock.lock();
        }
        if (runningWorkers == 0) {
            break;
        }
    }

    for (std::thread &worker : workers) {
        worker.join();
    }
}

void upsertFile(QSqlDatabase &database, const FileAnalysis &analysis)
{
    QSqlQuery query(database);
    prepareOrFail(query, QStringLiteral(
                             "INSERT INTO files(path, mtime, size, duration_ms, decode_hash, chromaprint_fp, "
                             "content_group_id, analyzed_at, status) "
                             "VALUES(?, ?, ?, ?, ?, ?, NULL, ?, ?) "
                             "ON CONFLICT(path) DO UPDATE SET "
                             "mtime = excluded.mtime,"
                             "size = excluded.size,"
                             "duration_ms = excluded.duration_ms,"
                             "decode_hash = excluded.decode_hash,"
                             "chromaprint_fp = excluded.chromaprint_fp,"
                             "content_group_id = NULL,"
                             "analyzed_at = excluded.analyzed_at,"
                             "status = excluded.status"));
    query.addBindValue(analysis.candidate.path);
    query.addBindValue(analysis.candidate.mtime);
    query.addBindValue(analysis.candidate.size);
    query.addBindValue(analysis.durationMs >= 0 ? QVariant(analysis.durationMs) : QVariant());
    query.addBindValue(analysis.decodeHash.isEmpty() ? QVariant() : QVariant(analysis.decodeHash));
    query.addBindValue(analysis.chromaprint.isEmpty() ? QVariant() : QVariant(analysis.chromaprint));
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    query.addBindValue(analysis.status);
    execPrepared(query);
}

void prepareFileFeaturesUpsert(QSqlQuery &query)
{
    prepareOrFail(query, QStringLiteral(
                             "INSERT INTO file_features("
                             " path, tempo_bpm, loudness_lufs, loudness_std_db,"
                             " spectral_centroid_mean_hz, spectral_centroid_std_hz,"
                             " spectral_flatness_mean, zero_crossing_rate, onset_rate_hz,"
                             " energy, extractor, version)"
                             " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
                             " ON CONFLICT(path) DO UPDATE SET"
                             " tempo_bpm = excluded.tempo_bpm,"
                             " loudness_lufs = excluded.loudness_lufs,"
                             " loudness_std_db = excluded.loudness_std_db,"
                             " spectral_centroid_mean_hz = excluded.spectral_centroid_mean_hz,"
                             " spectral_centroid_std_hz = excluded.spectral_centroid_std_hz,"
                             " spectral_flatness_mean = excluded.spectral_flatness_mean,"
                             " zero_crossing_rate = excluded.zero_crossing_rate,"
                             " onset_rate_hz = excluded.onset_rate_hz,"
                             " energy = excluded.energy,"
                             " extractor = excluded.extractor,"
                             " version = excluded.version"));
}

// Durable twin of recordScalarExtraction: persist the per-file scalars so a
// later feature phase (this run or any future one) can copy them instead of
// decoding the file again. The caller owns one prepared query for the whole
// phase; preparing this upsert once per file would add avoidable SQLite work
// to the hot completion path.
void upsertFileFeatures(QSqlQuery &query, const QString &path,
                        const std::optional<Dsp::ScalarFeatures> &features)
{
    bindScalarRow(query, path, features);
    execPrepared(query);
}

std::vector<qint32> decodeFingerprint(const QByteArray &blob)
{
    std::vector<qint32> values;
    if (blob.size() % static_cast<qsizetype>(sizeof(qint32)) != 0) {
        return values;
    }
    const qsizetype count = blob.size() / static_cast<qsizetype>(sizeof(qint32));
    values.reserve(static_cast<std::size_t>(count));
    const auto *data = reinterpret_cast<const uchar *>(blob.constData());
    for (qsizetype index = 0; index < count; ++index) {
        values.push_back(qFromLittleEndian<qint32>(
            data + index * static_cast<qsizetype>(sizeof(qint32))));
    }
    return values;
}

std::vector<GroupRow> loadGroupRows(QSqlDatabase &database)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral(
            "SELECT path, duration_ms, decode_hash, chromaprint_fp, content_group_id "
            "FROM files "
            "WHERE status = 'ok' "
            "  AND duration_ms IS NOT NULL "
            "  AND decode_hash IS NOT NULL "
            "  AND chromaprint_fp IS NOT NULL "
            "ORDER BY path"))) {
        fail(query.lastError().text());
    }

    std::vector<GroupRow> rows;
    while (query.next()) {
        rows.push_back({
            query.value(0).toString(),
            query.value(1).toLongLong(),
            query.value(2).toString(),
            decodeFingerprint(query.value(3).toByteArray()),
            query.value(4).isNull() ? -1 : query.value(4).toLongLong(),
        });
    }
    return rows;
}

bool groupRowAffected(const GroupRow &row, const QSet<qint64> &affectedGroupIds)
{
    return row.oldGroupId < 0 || affectedGroupIds.contains(row.oldGroupId);
}

// Incremental grouping needs metadata for every row so stable components and
// exact hashes remain globally visible, but Chromaprint payloads only for an
// affected row and its duration-neighborhood candidates. On the field store
// the blobs alone occupy roughly 339 MiB; materializing all of them after five
// changes would throw away most of the incremental path's memory advantage.
std::vector<GroupRow> loadIncrementalGroupRows(
    QSqlDatabase &database, const QSet<qint64> &affectedGroupIds)
{
    QSqlQuery metadata(database);
    if (!metadata.exec(QStringLiteral(
            "SELECT path, duration_ms, decode_hash, content_group_id "
            "FROM files "
            "WHERE status = 'ok' "
            "  AND duration_ms IS NOT NULL "
            "  AND decode_hash IS NOT NULL "
            "  AND chromaprint_fp IS NOT NULL "
            "ORDER BY path"))) {
        fail(metadata.lastError().text());
    }

    std::vector<GroupRow> rows;
    while (metadata.next()) {
        rows.push_back({
            metadata.value(0).toString(),
            metadata.value(1).toLongLong(),
            metadata.value(2).toString(),
            {},
            metadata.value(3).isNull() ? -1 : metadata.value(3).toLongLong(),
        });
    }

    std::vector<std::size_t> byDuration(rows.size());
    std::iota(byDuration.begin(), byDuration.end(), 0);
    std::sort(byDuration.begin(), byDuration.end(), [&rows](std::size_t left, std::size_t right) {
        if (rows[left].durationMs != rows[right].durationMs) {
            return rows[left].durationMs < rows[right].durationMs;
        }
        return rows[left].path < rows[right].path;
    });

    std::vector<bool> fingerprintNeeded(rows.size(), false);
    for (std::size_t affected = 0; affected < rows.size(); ++affected) {
        if (!groupRowAffected(rows[affected], affectedGroupIds)) {
            continue;
        }
        const qint64 minimum = rows[affected].durationMs - kDurationBucketMs;
        const qint64 maximum = rows[affected].durationMs + kDurationBucketMs;
        auto candidate = std::lower_bound(
            byDuration.begin(), byDuration.end(), minimum,
            [&rows](std::size_t index, qint64 duration) {
                return rows[index].durationMs < duration;
            });
        for (; candidate != byDuration.end()
               && rows[*candidate].durationMs <= maximum; ++candidate) {
            fingerprintNeeded[*candidate] = true;
        }
    }

    const std::size_t needed = static_cast<std::size_t>(
        std::count(fingerprintNeeded.begin(), fingerprintNeeded.end(), true));
    if (needed == 0) {
        return rows;
    }

    // Point lookups minimize blob I/O for the normal small-delta case. Once a
    // quarter of the corpus is needed, one ordered scan is cheaper than many
    // B-tree probes and the memory is inherent in that large affected set.
    if (needed * 4 >= rows.size()) {
        QSqlQuery fingerprints(database);
        if (!fingerprints.exec(QStringLiteral(
                "SELECT chromaprint_fp FROM files "
                "WHERE status = 'ok' "
                "  AND duration_ms IS NOT NULL "
                "  AND decode_hash IS NOT NULL "
                "  AND chromaprint_fp IS NOT NULL "
                "ORDER BY path"))) {
            fail(fingerprints.lastError().text());
        }
        std::size_t index = 0;
        while (fingerprints.next() && index < rows.size()) {
            if (fingerprintNeeded[index]) {
                rows[index].chromaprint = decodeFingerprint(fingerprints.value(0).toByteArray());
            }
            ++index;
        }
        if (index != rows.size()) {
            fail(QStringLiteral("incremental fingerprint row count changed during grouping"));
        }
        return rows;
    }

    QSqlQuery fingerprint(database);
    prepareOrFail(fingerprint, QStringLiteral(
        "SELECT chromaprint_fp FROM files WHERE path = ?"));
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (!fingerprintNeeded[index]) {
            continue;
        }
        fingerprint.bindValue(0, rows[index].path);
        execPrepared(fingerprint);
        if (!fingerprint.next()) {
            fail(QStringLiteral("incremental fingerprint row disappeared during grouping"));
        }
        rows[index].chromaprint = decodeFingerprint(fingerprint.value(0).toByteArray());
        fingerprint.finish();
    }
    return rows;
}

double bitErrorRate(const std::vector<qint32> &left, const std::vector<qint32> &right, int offset)
{
    const std::size_t leftStart = offset >= 0 ? static_cast<std::size_t>(offset) : 0;
    const std::size_t rightStart = offset < 0 ? static_cast<std::size_t>(-offset) : 0;
    if (leftStart >= left.size() || rightStart >= right.size()) {
        return std::numeric_limits<double>::infinity();
    }
    const std::size_t overlap = std::min(left.size() - leftStart, right.size() - rightStart);
    if (overlap == 0) {
        return std::numeric_limits<double>::infinity();
    }

    quint64 errors = 0;
    for (std::size_t index = 0; index < overlap; ++index) {
        const std::uint32_t diff = static_cast<std::uint32_t>(
            left[leftStart + index] ^ right[rightStart + index]);
        errors += std::popcount(diff);
    }
    return static_cast<double>(errors) / (static_cast<double>(overlap) * 32.0);
}

double bestBitErrorRate(const std::vector<qint32> &left, const std::vector<qint32> &right)
{
    double best = std::numeric_limits<double>::infinity();
    for (int offset = -kChromaprintOffsetFrames; offset <= kChromaprintOffsetFrames; ++offset) {
        best = std::min(best, bitErrorRate(left, right, offset));
    }
    return best;
}

class UnionFind final {
public:
    explicit UnionFind(std::size_t size)
        : m_parent(size)
    {
        for (std::size_t index = 0; index < size; ++index) {
            m_parent[index] = index;
        }
    }

    std::size_t find(std::size_t index)
    {
        const std::size_t parent = m_parent[index];
        if (parent == index) {
            return index;
        }
        const std::size_t root = find(parent);
        m_parent[index] = root;
        return root;
    }

    void unite(std::size_t left, std::size_t right)
    {
        const std::size_t leftRoot = find(left);
        const std::size_t rightRoot = find(right);
        if (leftRoot != rightRoot) {
            m_parent[rightRoot] = leftRoot;
        }
    }

private:
    std::vector<std::size_t> m_parent;
};

std::vector<std::vector<std::size_t>> collectGroups(const std::vector<GroupRow> &rows,
                                                    UnionFind &unionFind)
{
    std::map<std::size_t, std::vector<std::size_t>> byRoot;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        byRoot[unionFind.find(index)].push_back(index);
    }

    std::vector<std::vector<std::size_t>> groups;
    groups.reserve(byRoot.size());
    for (auto &entry : byRoot) {
        std::vector<std::size_t> members = std::move(entry.second);
        std::sort(members.begin(), members.end(), [&rows](std::size_t left, std::size_t right) {
            return rows[left].path < rows[right].path;
        });
        groups.push_back(std::move(members));
    }
    std::sort(groups.begin(), groups.end(), [&rows](const auto &left, const auto &right) {
        return rows[left.front()].path < rows[right.front()].path;
    });
    return groups;
}

std::vector<std::vector<std::size_t>> groupedRows(const std::vector<GroupRow> &rows)
{
    UnionFind unionFind(rows.size());
    std::map<QString, std::size_t> byHash;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto [it, inserted] = byHash.emplace(rows[index].decodeHash, index);
        if (!inserted) {
            unionFind.unite(it->second, index);
        }
    }

    std::vector<std::size_t> byDuration(rows.size());
    std::iota(byDuration.begin(), byDuration.end(), 0);
    std::sort(byDuration.begin(), byDuration.end(), [&rows](std::size_t left, std::size_t right) {
        if (rows[left].durationMs != rows[right].durationMs) {
            return rows[left].durationMs < rows[right].durationMs;
        }
        return rows[left].path < rows[right].path;
    });
    for (std::size_t leftPos = 0; leftPos < byDuration.size(); ++leftPos) {
        const std::size_t left = byDuration[leftPos];
        for (std::size_t rightPos = leftPos + 1; rightPos < byDuration.size(); ++rightPos) {
            const std::size_t right = byDuration[rightPos];
            if (rows[right].durationMs - rows[left].durationMs > kDurationBucketMs) {
                break;
            }
            if (bestBitErrorRate(rows[left].chromaprint, rows[right].chromaprint) < kChromaprintBerThreshold) {
                unionFind.unite(left, right);
            }
        }
    }

    return collectGroups(rows, unionFind);
}

// Existing groups are already connected components. A normal incremental
// scan only needs to reconsider new/changed rows and every old component that
// lost a changed member; stable components can be unioned by id without
// repeating all of their pairwise Chromaprint comparisons. Comparisons with at
// least one affected row still use the exact full-regroup predicates, so a
// changed bridge can split its former group and a new bridge can merge groups.
std::vector<std::vector<std::size_t>> incrementallyGroupedRows(
    const std::vector<GroupRow> &rows, const QSet<qint64> &affectedGroupIds)
{
    UnionFind unionFind(rows.size());
    std::vector<bool> affected(rows.size(), false);
    QHash<qint64, std::size_t> stableGroupRepresentative;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const qint64 oldGroupId = rows[index].oldGroupId;
        affected[index] = groupRowAffected(rows[index], affectedGroupIds);
        if (affected[index]) {
            continue;
        }
        const auto it = stableGroupRepresentative.constFind(oldGroupId);
        if (it == stableGroupRepresentative.constEnd()) {
            stableGroupRepresentative.insert(oldGroupId, index);
        } else {
            unionFind.unite(it.value(), index);
        }
    }

    // Exact decoded identity is cheap to index and authoritative. Unioning it
    // globally also heals any historical duplicate groups without adding a
    // quadratic pass.
    std::map<QString, std::size_t> byHash;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto [it, inserted] = byHash.emplace(rows[index].decodeHash, index);
        if (!inserted) {
            unionFind.unite(it->second, index);
        }
    }

    std::vector<std::size_t> byDuration(rows.size());
    std::iota(byDuration.begin(), byDuration.end(), 0);
    std::sort(byDuration.begin(), byDuration.end(), [&rows](std::size_t left, std::size_t right) {
        if (rows[left].durationMs != rows[right].durationMs) {
            return rows[left].durationMs < rows[right].durationMs;
        }
        return rows[left].path < rows[right].path;
    });
    for (std::size_t leftPos = 0; leftPos < byDuration.size(); ++leftPos) {
        const std::size_t left = byDuration[leftPos];
        for (std::size_t rightPos = leftPos + 1; rightPos < byDuration.size(); ++rightPos) {
            const std::size_t right = byDuration[rightPos];
            if (rows[right].durationMs - rows[left].durationMs > kDurationBucketMs) {
                break;
            }
            if ((!affected[left] && !affected[right])
                || unionFind.find(left) == unionFind.find(right)) {
                continue;
            }
            if (bestBitErrorRate(rows[left].chromaprint, rows[right].chromaprint)
                < kChromaprintBerThreshold) {
                unionFind.unite(left, right);
            }
        }
    }

    return collectGroups(rows, unionFind);
}

// Group ids must be STABLE across rescans: features, embeddings, and
// track_neighbors rows all key on content_group_id, and embeddings are
// expensive to recompute. A group that keeps (part of) its membership keeps
// its id; only genuinely new content gets a new id, and rows keyed to ids
// that no longer exist are removed.
std::vector<qint64> assignStableGroupIds(const std::vector<GroupRow> &rows,
                                         const std::vector<std::vector<std::size_t>> &groups,
                                         qint64 reservedMaxId)
{
    // The old representative (first path in load order, which is sorted) is
    // the tie-breaker when an old group's members end up split across new
    // groups: the half holding the old representative keeps the id.
    QHash<qint64, QString> oldRepresentative;
    qint64 maxOldId = 0;
    for (const GroupRow &row : rows) {
        if (row.oldGroupId < 0) {
            continue;
        }
        maxOldId = std::max(maxOldId, row.oldGroupId);
        if (!oldRepresentative.contains(row.oldGroupId)) {
            oldRepresentative.insert(row.oldGroupId, row.path);
        }
    }

    std::vector<qint64> assigned(groups.size(), -1);
    QSet<qint64> claimed;

    // Pass 1: a group that contains an old group's representative keeps that
    // old id (smallest such id on merges, for determinism).
    for (std::size_t g = 0; g < groups.size(); ++g) {
        qint64 best = -1;
        for (std::size_t member : groups[g]) {
            const qint64 oldId = rows[member].oldGroupId;
            if (oldId < 0 || claimed.contains(oldId)) {
                continue;
            }
            if (oldRepresentative.value(oldId) == rows[member].path && (best < 0 || oldId < best)) {
                best = oldId;
            }
        }
        if (best >= 0) {
            assigned[g] = best;
            claimed.insert(best);
        }
    }

    // Pass 2: otherwise reuse the smallest still-unclaimed old id among the
    // group's members (covers representative files that vanished).
    for (std::size_t g = 0; g < groups.size(); ++g) {
        if (assigned[g] >= 0) {
            continue;
        }
        qint64 best = -1;
        for (std::size_t member : groups[g]) {
            const qint64 oldId = rows[member].oldGroupId;
            if (oldId >= 0 && !claimed.contains(oldId) && (best < 0 || oldId < best)) {
                best = oldId;
            }
        }
        if (best >= 0) {
            assigned[g] = best;
            claimed.insert(best);
        }
    }

    // Pass 3: genuinely new content gets fresh ids past every id ever seen.
    // reservedMaxId covers ids held by rows outside the regroup input
    // (surviving features/embeddings/neighbors rows), so a fresh id can never
    // collide with — and silently inherit — a stale row's data.
    qint64 nextId = std::max(maxOldId, reservedMaxId) + 1;
    for (qint64 &id : assigned) {
        if (id < 0) {
            id = nextId++;
        }
    }
    return assigned;
}

void deleteOrphanedGroupRows(QSqlDatabase &database)
{
    const QStringList tables = database.tables();
    execSql(database, QStringLiteral(
                          "DELETE FROM features WHERE content_group_id NOT IN "
                          "(SELECT id FROM content_groups)"));
    // The embedder owns these tables, so they exist only once it has run.
    if (tables.contains(QStringLiteral("embeddings"))) {
        execSql(database, QStringLiteral(
                              "DELETE FROM embeddings WHERE content_group_id NOT IN "
                              "(SELECT id FROM content_groups)"));
    }
    if (tables.contains(QStringLiteral("track_neighbors"))) {
        execSql(database, QStringLiteral(
                              "DELETE FROM track_neighbors WHERE content_group_id NOT IN "
                              "(SELECT id FROM content_groups) OR neighbor_group_id NOT IN "
                              "(SELECT id FROM content_groups)"));
    }
    // Per-file scalar rows follow their files row's lifecycle exactly.
    execSql(database, QStringLiteral(
                          "DELETE FROM file_features WHERE path NOT IN "
                          "(SELECT path FROM files)"));
}

qint64 countScalar(QSqlDatabase &database, const QString &sql);

// Highest group id referenced anywhere in the store, including rows the
// regroup input does not see (features/embeddings/neighbors whose files
// changed or vanished). Fresh ids must start past ALL of them.
qint64 maxReservedGroupId(QSqlDatabase &database)
{
    qint64 maxId = 0;
    const auto consider = [&](const QString &sql) {
        maxId = std::max(maxId, countScalar(database, sql));
    };
    consider(QStringLiteral("SELECT COALESCE(MAX(content_group_id), 0) FROM files"));
    consider(QStringLiteral("SELECT COALESCE(MAX(id), 0) FROM content_groups"));
    consider(QStringLiteral("SELECT COALESCE(MAX(content_group_id), 0) FROM features"));
    const QStringList tables = database.tables();
    if (tables.contains(QStringLiteral("embeddings"))) {
        consider(QStringLiteral("SELECT COALESCE(MAX(content_group_id), 0) FROM embeddings"));
    }
    if (tables.contains(QStringLiteral("track_neighbors"))) {
        consider(QStringLiteral(
            "SELECT COALESCE(MAX(MAX(content_group_id), MAX(neighbor_group_id)), 0) FROM track_neighbors"));
    }
    return maxId;
}

// Feature rows are NOT cleared here: they stay keyed to their stable group
// ids, featureRowFresh() skips the still-valid ones, and
// deleteOrphanedGroupRows() removes the rest. Wiping them would force
// ensureFeatureRows to re-decode every unchanged representative serially
// on any incremental scan or cancel+resume.
int regroupContent(QSqlDatabase &database)
{
    const std::vector<GroupRow> rows = loadGroupRows(database);
    const std::vector<std::vector<std::size_t>> groups = groupedRows(rows);
    const std::vector<qint64> groupIds = assignStableGroupIds(rows, groups, maxReservedGroupId(database));

    if (!database.transaction()) {
        fail(database.lastError().text());
    }
    execSql(database, QStringLiteral("UPDATE files SET content_group_id = NULL"));
    execSql(database, QStringLiteral("DELETE FROM content_groups"));

    for (std::size_t g = 0; g < groups.size(); ++g) {
        QSqlQuery insertGroup(database);
        prepareOrFail(insertGroup, QStringLiteral("INSERT INTO content_groups(id) VALUES(?)"));
        insertGroup.addBindValue(groupIds[g]);
        execPrepared(insertGroup);

        for (std::size_t member : groups[g]) {
            QSqlQuery updateFile(database);
            prepareOrFail(updateFile, QStringLiteral("UPDATE files SET content_group_id = ? WHERE path = ?"));
            updateFile.addBindValue(groupIds[g]);
            updateFile.addBindValue(rows[member].path);
            execPrepared(updateFile);
        }
    }
    deleteOrphanedGroupRows(database);
    if (!database.commit()) {
        fail(database.lastError().text());
    }
    return static_cast<int>(groups.size());
}

int regroupContentIncrementally(QSqlDatabase &database,
                                const QSet<qint64> &affectedGroupIds)
{
    const std::vector<GroupRow> rows = loadIncrementalGroupRows(database, affectedGroupIds);
    const std::vector<std::vector<std::size_t>> groups =
        incrementallyGroupedRows(rows, affectedGroupIds);
    const std::vector<qint64> groupIds =
        assignStableGroupIds(rows, groups, maxReservedGroupId(database));

    if (!database.transaction()) {
        fail(database.lastError().text());
    }
    QSqlQuery insertGroup(database);
    prepareOrFail(insertGroup, QStringLiteral(
        "INSERT OR IGNORE INTO content_groups(id) VALUES(?)"));
    QSqlQuery updateFile(database);
    prepareOrFail(updateFile, QStringLiteral(
        "UPDATE files SET content_group_id = ? WHERE path = ?"));

    for (std::size_t g = 0; g < groups.size(); ++g) {
        insertGroup.bindValue(0, groupIds[g]);
        execPrepared(insertGroup);
        for (std::size_t member : groups[g]) {
            if (rows[member].oldGroupId == groupIds[g]) {
                continue;
            }
            updateFile.bindValue(0, groupIds[g]);
            updateFile.bindValue(1, rows[member].path);
            execPrepared(updateFile);
        }
    }

    // Match the full rebuild's treatment of rows that cannot participate in
    // grouping, then remove only group ids no longer referenced after a
    // split/merge. Stable components and their rows are never rewritten.
    execSql(database, QStringLiteral(
        "UPDATE files SET content_group_id = NULL "
        "WHERE status != 'ok' OR duration_ms IS NULL OR decode_hash IS NULL "
        "OR chromaprint_fp IS NULL"));
    execSql(database, QStringLiteral(
        "DELETE FROM content_groups WHERE id NOT IN ("
        " SELECT DISTINCT content_group_id FROM files"
        " WHERE content_group_id IS NOT NULL)"));
    deleteOrphanedGroupRows(database);
    if (!database.commit()) {
        fail(database.lastError().text());
    }
    return static_cast<int>(groups.size());
}

void recordScalarExtraction(QHash<QString, ScalarExtraction> &extracted, const FileAnalysis &analysis)
{
    if (analysis.status != QLatin1String("ok")) {
        return;
    }
    ScalarExtraction row;
    row.known = analysis.scalars.has_value();
    if (analysis.scalars) {
        row.features = *analysis.scalars;
    }
    extracted.insert(analysis.candidate.path, row);
}

std::optional<Dsp::ScalarFeatures> scalarsForRepresentative(const QString &path,
                                                            const QHash<QString, ScalarExtraction> &extracted,
                                                            StageTimings &timings)
{
    const auto it = extracted.constFind(path);
    if (it != extracted.constEnd()) {
        return it->known ? std::optional<Dsp::ScalarFeatures>(it->features) : std::nullopt;
    }
    // Feature refresh never hashes, so skip the raw byte copy entirely.
    const auto decodeStarted = std::chrono::steady_clock::now();
    const DecodedAudio decoded = decodeCanonical(path, false);
    timings.decodeMs = elapsedMs(decodeStarted);
    const auto dspStarted = std::chrono::steady_clock::now();
    const auto scalars = Dsp::analyze(decoded.samples, Dsp::kSampleRateHz);
    timings.dspMs = elapsedMs(dspStarted);
    return scalars;
}

struct FeatureFillResult {
    int processed = 0;
    int written = 0;
    int failed = 0;
    int total = 0;
    bool canceled = false;
    TimingAccumulator timings;
};

struct StaleRep {
    qint64 groupId = 0;
    QString path;
};

struct FeatureAnalysis {
    StaleRep representative;
    std::optional<Dsp::ScalarFeatures> scalars;
    bool failed = false;
    StageTimings timings;
};

void emitVerboseFeatureRepresentative(const FeatureAnalysis &analysis);

FeatureAnalysis analyzeFeatureRepresentative(const StaleRep &representative,
                                             const QHash<QString, ScalarExtraction> &extracted)
{
    FeatureAnalysis analysis;
    analysis.representative = representative;
    try {
        analysis.scalars = scalarsForRepresentative(representative.path, extracted, analysis.timings);
    } catch (const std::exception &) {
        analysis.failed = true;
    }
    return analysis;
}

void analyzeFeatureRepresentatives(const std::vector<StaleRep> &representatives,
                                   const QHash<QString, ScalarExtraction> &extracted, int jobs,
                                   const std::function<void(FeatureAnalysis &&, int, int)> &completion)
{
    if (representatives.empty()) {
        return;
    }

    const std::size_t workerCount = static_cast<std::size_t>(std::max(1, jobs));
    const std::size_t maxCompleted = workerCount;
    const int total = static_cast<int>(representatives.size());
    std::atomic_size_t nextIndex = 0;
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<FeatureAnalysis> completed;
    std::size_t runningWorkers = workerCount;

    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (std::size_t worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&]() {
            while (!stopRequested()) {
                const std::size_t index = nextIndex.fetch_add(1, std::memory_order_relaxed);
                if (index >= representatives.size()) {
                    break;
                }
                FeatureAnalysis analysis = analyzeFeatureRepresentative(representatives[index], extracted);
                {
                    std::unique_lock lock(mutex);
                    ready.wait(lock, [&]() { return completed.size() < maxCompleted || stopRequested(); });
                    // Push even on stop: this analysis is already paid for, and
                    // the caller drains the queue before finishing, so the row
                    // stays durable. The bound may overshoot by one per worker
                    // during shutdown, which is harmless.
                    completed.push_back(std::move(analysis));
                }
                ready.notify_one();
            }
            {
                std::lock_guard lock(mutex);
                --runningWorkers;
            }
            ready.notify_one();
        });
    }

    int processed = 0;
    while (true) {
        std::unique_lock lock(mutex);
        ready.wait(lock, [&]() { return !completed.empty() || runningWorkers == 0; });
        while (!completed.empty()) {
            FeatureAnalysis analysis = std::move(completed.front());
            completed.pop_front();
            ready.notify_all();
            lock.unlock();
            ++processed;
            completion(std::move(analysis), processed, total);
            lock.lock();
        }
        if (runningWorkers == 0) {
            break;
        }
    }

    for (std::thread &worker : workers) {
        worker.join();
    }
}

// Phase-local rate for feature fill: emit rate=- eta=- until the phase window
// is warm (≥2 s span with movement). Never falls back to lifetime n/elapsed
// after the feature phase starts (that would re-show file-phase thruput).
class FeatureProgressRate final {
public:
    struct Snapshot {
        QString rateText = QStringLiteral("-");
        QString etaText = QStringLiteral("-");
    };

    Snapshot update(double phaseElapsedSecs, int processed, int total)
    {
        m_samples.push_back({phaseElapsedSecs, processed});
        while (m_samples.size() > 2 && phaseElapsedSecs - m_samples.front().elapsedSecs > kWindowSecs) {
            m_samples.pop_front();
        }
        const Sample &oldest = m_samples.front();
        const double span = phaseElapsedSecs - oldest.elapsedSecs;
        if (span < kMinSpanSecs || processed <= oldest.processed) {
            return {};
        }
        const double rate = static_cast<double>(processed - oldest.processed) / span;
        Snapshot snapshot;
        snapshot.rateText = QString::number(rate, 'f', 1);
        if (rate > 0.0 && total > processed) {
            snapshot.etaText =
                QString::number(static_cast<qint64>(std::ceil(static_cast<double>(total - processed) / rate)));
        } else if (total <= processed) {
            snapshot.etaText = QStringLiteral("0");
        }
        return snapshot;
    }

private:
    struct Sample {
        double elapsedSecs = 0.0;
        int processed = 0;
    };
    static constexpr double kWindowSecs = 60.0;
    static constexpr double kMinSpanSecs = 2.0;
    std::deque<Sample> m_samples;
};

void emitFeatureProgress(int processed, int total,
                         std::chrono::steady_clock::time_point scanStarted,
                         std::chrono::steady_clock::time_point phaseStarted,
                         FeatureProgressRate &phaseRate)
{
    const auto now = std::chrono::steady_clock::now();
    const double elapsedSecs = std::chrono::duration<double>(now - scanStarted).count();
    const double phaseElapsedSecs = std::chrono::duration<double>(now - phaseStarted).count();
    const auto snapshot = phaseRate.update(phaseElapsedSecs, processed, total);
    if (g_progressJsonl) {
        QJsonObject payload{
            {QStringLiteral("protocol_version"), 1},
            {QStringLiteral("request_id"), g_progressRequestId},
            {QStringLiteral("event"), QStringLiteral("progress")},
            {QStringLiteral("phase"), QStringLiteral("scalar-features")},
            {QStringLiteral("completed"), processed},
            {QStringLiteral("total"), total},
            {QStringLiteral("unit"), QStringLiteral("groups")},
        };
        if (snapshot.rateText != QLatin1String("-")) {
            payload.insert(QStringLiteral("rate"), snapshot.rateText.toDouble());
        }
        if (snapshot.etaText != QLatin1String("-")) {
            payload.insert(QStringLiteral("eta_seconds"), snapshot.etaText.toLongLong());
        }
        QTextStream(stdout) << QJsonDocument(payload).toJson(QJsonDocument::Compact) << '\n';
        return;
    }
    QTextStream err(stderr);
    err.setEncoding(QStringConverter::Utf8);
    err << "progress " << processed << '/' << total
        << " elapsed=" << QString::number(elapsedSecs, 'f', 1)
        << " rate=" << snapshot.rateText
        << " eta=" << snapshot.etaText << '\n';
    err.flush();
}

// A stale group whose representative already has fresh per-file scalars:
// the group row is written by copying these values — no decode. The ten
// variants are the nine scalar columns plus extractor, preserved exactly as
// the file row stores them.
struct StaleRepCopy {
    qint64 groupId = 0;
    QVariantList scalarRow;
};

struct MaterializedStaleReps {
    std::vector<StaleRepCopy> copies;
    std::vector<StaleRep> decodes;
};

MaterializedStaleReps materializeStaleReps(QSqlDatabase &database)
{
    // Matches featureRowFresh: missing row, older version, and NULL version
    // are stale; exact current version is fresh. Representative is MIN(path)
    // over ok file rows — same as the previous serial loop. The outer join
    // against file_features splits the stale set into copyable groups
    // (representative has fresh per-file scalars) and groups that still
    // need a decode.
    QSqlQuery reps(database);
    prepareOrFail(reps, QStringLiteral(
                            "SELECT s.content_group_id, s.path, ff.version IS NOT NULL,"
                            " ff.tempo_bpm, ff.loudness_lufs, ff.loudness_std_db,"
                            " ff.spectral_centroid_mean_hz, ff.spectral_centroid_std_hz,"
                            " ff.spectral_flatness_mean, ff.zero_crossing_rate,"
                            " ff.onset_rate_hz, ff.energy, ff.extractor "
                            "FROM (SELECT f.content_group_id AS content_group_id,"
                            "             MIN(f.path) AS path "
                            "      FROM files f "
                            "      LEFT JOIN features feat"
                            "        ON feat.content_group_id = f.content_group_id "
                            "      WHERE f.content_group_id IS NOT NULL AND f.status = 'ok' "
                            "        AND (feat.version IS NULL OR feat.version != ?) "
                            "      GROUP BY f.content_group_id) s "
                            "LEFT JOIN file_features ff ON ff.path = s.path AND ff.version = ? "
                            "ORDER BY s.content_group_id"));
    reps.addBindValue(QString::fromLatin1(Dsp::kDspVersion));
    reps.addBindValue(QString::fromLatin1(Dsp::kDspVersion));
    execPrepared(reps);

    MaterializedStaleReps stale;
    while (reps.next()) {
        if (reps.value(2).toBool()) {
            QVariantList row;
            row.reserve(10);
            for (int column = 3; column <= 12; ++column) {
                row.push_back(reps.value(column));
            }
            stale.copies.push_back(StaleRepCopy{reps.value(0).toLongLong(), std::move(row)});
        } else {
            stale.decodes.push_back(StaleRep{reps.value(0).toLongLong(), reps.value(1).toString()});
        }
    }
    return stale;
}

FeatureFillResult ensureFeatureRows(QSqlDatabase &database,
                                    const QHash<QString, ScalarExtraction> &extracted,
                                    int jobs, bool progress, bool verbose,
                                    std::chrono::steady_clock::time_point scanStarted)
{
    FeatureFillResult result;
    const MaterializedStaleReps stale = materializeStaleReps(database);
    result.total = static_cast<int>(stale.copies.size() + stale.decodes.size());

    const auto phaseStarted = std::chrono::steady_clock::now();
    FeatureProgressRate phaseRate;
    int lastProgress = -1;
    auto lastProgressEmit = phaseStarted;
    const auto maybeEmit = [&](bool force) {
        if (!progress) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force && result.processed != result.total && result.processed - lastProgress < 25
            && std::chrono::duration_cast<std::chrono::seconds>(now - lastProgressEmit).count() < 2) {
            return;
        }
        emitFeatureProgress(result.processed, result.total, scanStarted, phaseStarted, phaseRate);
        lastProgress = result.processed;
        lastProgressEmit = now;
    };

    if (progress) {
        emitFeatureProgress(0, result.total, scanStarted, phaseStarted, phaseRate);
        lastProgress = 0;
        lastProgressEmit = std::chrono::steady_clock::now();
    }

    QSqlQuery upsert(database);
    prepareOrFail(upsert, QStringLiteral(
                              "INSERT INTO features("
                              " content_group_id, tempo_bpm, loudness_lufs, loudness_std_db,"
                              " spectral_centroid_mean_hz, spectral_centroid_std_hz,"
                              " spectral_flatness_mean, zero_crossing_rate, onset_rate_hz,"
                              " energy, extractor, version)"
                              " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"
                              " ON CONFLICT(content_group_id) DO UPDATE SET"
                              " tempo_bpm = excluded.tempo_bpm,"
                              " loudness_lufs = excluded.loudness_lufs,"
                              " loudness_std_db = excluded.loudness_std_db,"
                              " spectral_centroid_mean_hz = excluded.spectral_centroid_mean_hz,"
                              " spectral_centroid_std_hz = excluded.spectral_centroid_std_hz,"
                              " spectral_flatness_mean = excluded.spectral_flatness_mean,"
                              " zero_crossing_rate = excluded.zero_crossing_rate,"
                              " onset_rate_hz = excluded.onset_rate_hz,"
                              " energy = excluded.energy,"
                              " extractor = excluded.extractor,"
                              " version = excluded.version"));
    // Copy path first: stale groups whose representative already has fresh
    // per-file scalars become pure SQL upserts — no decode, no workers. The
    // variants pass through exactly as the file row stores them; version is
    // the expected one by construction of the materializing join.
    for (const StaleRepCopy &copy : stale.copies) {
        if (stopRequested()) {
            break;
        }
        int bindIndex = 0;
        upsert.bindValue(bindIndex++, copy.groupId);
        for (const QVariant &value : copy.scalarRow) {
            upsert.bindValue(bindIndex++, value);
        }
        upsert.bindValue(bindIndex, QString::fromLatin1(Dsp::kDspVersion));
        execPrepared(upsert);
        ++result.processed;
        ++result.written;
        maybeEmit(result.processed == result.total);
    }
    const int copiesDone = result.processed;

    // Decode fallback for representatives without a fresh per-file row —
    // and the backfill: every decoded representative gets its file_features
    // row written here, so the next version bump's refresh copies instead.
    QSqlQuery fileUpsert(database);
    prepareFileFeaturesUpsert(fileUpsert);
    const auto completion = [&](FeatureAnalysis &&analysis, int processed, int) {
        result.processed = copiesDone + processed;
        result.timings.add(analysis.timings);
        if (verbose) {
            emitVerboseFeatureRepresentative(analysis);
        }
        if (analysis.failed) {
            ++result.failed;
        } else {
            bindScalarRow(upsert, analysis.representative.groupId, analysis.scalars);
            execPrepared(upsert);
            upsertFileFeatures(fileUpsert, analysis.representative.path, analysis.scalars);
            ++result.written;
        }
        maybeEmit(result.processed == result.total);
    };
    // Workers only decode and analyze. The main thread owns these QSqlQuery
    // objects so the feature database remains a single-writer connection.
    if (!stopRequested()) {
        analyzeFeatureRepresentatives(stale.decodes, extracted, jobs, completion);
    }
    result.canceled = stopRequested();

    if (progress && (result.canceled || lastProgress != result.processed)) {
        emitFeatureProgress(result.processed, result.total, scanStarted, phaseStarted, phaseRate);
    }
    return result;
}

qint64 countScalar(QSqlDatabase &database, const QString &sql)
{
    QSqlQuery query(database);
    if (!query.exec(sql) || !query.next()) {
        return 0;
    }
    return query.value(0).toLongLong();
}

// Feature rows written by a different analyzer version than this binary's;
// they are recomputed on the next scan and, until then, hidden from the
// app-side read path (FeatureStore filters by expected version).
qint64 staleFeatureCount(QSqlDatabase &database)
{
    QSqlQuery query(database);
    prepareOrFail(query, QStringLiteral("SELECT COUNT(*) FROM features WHERE version != ?"));
    query.addBindValue(QString::fromLatin1(Dsp::kDspVersion));
    execPrepared(query);
    return query.next() ? query.value(0).toLongLong() : 0;
}

QJsonObject statusJson(QSqlDatabase &database)
{
    QJsonObject statuses;
    qint64 files = 0;
    QSqlQuery statusQuery(database);
    if (!statusQuery.exec(QStringLiteral("SELECT status, COUNT(*) FROM files GROUP BY status ORDER BY status"))) {
        fail(statusQuery.lastError().text());
    }
    while (statusQuery.next()) {
        const qint64 count = statusQuery.value(1).toLongLong();
        files += count;
        statuses.insert(statusQuery.value(0).toString(), static_cast<double>(count));
    }

    const qint64 groups = countScalar(database, QStringLiteral("SELECT COUNT(*) FROM content_groups"));
    const qint64 featured = countScalar(database, QStringLiteral("SELECT COUNT(*) FROM features"));
    const qint64 featuredStale = staleFeatureCount(database);
    return QJsonObject{
        {QStringLiteral("schema_version"), kSchemaVersion},
        {QStringLiteral("dsp_version"), QString::fromLatin1(Dsp::kDspVersion)},
        {QStringLiteral("files"), static_cast<double>(files)},
        {QStringLiteral("statuses"), statuses},
        {QStringLiteral("groups"), static_cast<double>(groups)},
        {QStringLiteral("features"), static_cast<double>(featured)},
        {QStringLiteral("featured_groups"), static_cast<double>(featured)},
        {QStringLiteral("featured_fresh"), static_cast<double>(featured - featuredStale)},
        {QStringLiteral("featured_stale"), static_cast<double>(featuredStale)},
        {QStringLiteral("pending"), 0},
    };
}

QJsonObject timingStatsJson(std::vector<qint64> values)
{
    if (values.empty()) {
        return QJsonObject{
            {QStringLiteral("total_ms"), 0},
            {QStringLiteral("mean_ms"), 0.0},
            {QStringLiteral("p50_ms"), 0},
            {QStringLiteral("p95_ms"), 0},
        };
    }
    std::sort(values.begin(), values.end());
    qint64 total = 0;
    for (const qint64 value : values) {
        total += value;
    }
    const auto percentile = [&](double fraction) {
        const auto index = static_cast<std::size_t>(
            std::clamp<std::size_t>(
                static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(values.size()))) - 1,
                0,
                values.size() - 1));
        return values.at(index);
    };
    return QJsonObject{
        {QStringLiteral("total_ms"), static_cast<double>(total)},
        {QStringLiteral("mean_ms"), static_cast<double>(total) / static_cast<double>(values.size())},
        {QStringLiteral("p50_ms"), static_cast<double>(percentile(0.50))},
        {QStringLiteral("p95_ms"), static_cast<double>(percentile(0.95))},
    };
}

QJsonObject timingsJson(const TimingAccumulator &timings)
{
    return QJsonObject{
        {QStringLiteral("decode"), timingStatsJson(timings.decode)},
        {QStringLiteral("hash"), timingStatsJson(timings.hash)},
        {QStringLiteral("dsp"), timingStatsJson(timings.dsp)},
        {QStringLiteral("fp"), timingStatsJson(timings.fp)},
    };
}

QJsonObject featureFillTimingsJson(const TimingAccumulator &timings)
{
    return QJsonObject{
        {QStringLiteral("decode"), timingStatsJson(timings.decode)},
        {QStringLiteral("dsp"), timingStatsJson(timings.dsp)},
    };
}

QJsonObject scanJson(int scanned, int skipped, int failed, int groups, int featured,
                     int featuredStale, double elapsedSecs = 0.0, const QJsonObject &timings = {},
                     bool canceled = false, const QString &power = QStringLiteral("turbo"),
                     int jobs = 1, const FeatureFillResult *featureFill = nullptr)
{
    QJsonObject object{
        {QStringLiteral("schema_version"), kSchemaVersion},
        {QStringLiteral("scanned"), scanned},
        {QStringLiteral("skipped"), skipped},
        {QStringLiteral("failed"), failed},
        {QStringLiteral("canceled"), canceled},
        {QStringLiteral("power"), power},
        {QStringLiteral("jobs"), jobs},
        {QStringLiteral("groups"), groups},
        {QStringLiteral("dsp_version"), QString::fromLatin1(Dsp::kDspVersion)},
        {QStringLiteral("featured_groups"), featured},
        {QStringLiteral("featured_fresh"), featured - featuredStale},
        {QStringLiteral("featured_stale"), featuredStale},
        {QStringLiteral("elapsed_secs"), elapsedSecs},
        {QStringLiteral("timings"), timings.isEmpty() ? timingsJson({}) : timings},
    };
    if (featureFill != nullptr) {
        object.insert(QStringLiteral("feature_groups_processed"), featureFill->processed);
        object.insert(QStringLiteral("features_written"), featureFill->written);
        object.insert(QStringLiteral("feature_groups_failed"), featureFill->failed);
        object.insert(QStringLiteral("feature_fill_timings"), featureFillTimingsJson(featureFill->timings));
    }
    return object;
}

int currentGroupCount(QSqlDatabase &database)
{
    return static_cast<int>(countScalar(database, QStringLiteral("SELECT COUNT(*) FROM content_groups")));
}

int currentFeatureCount(QSqlDatabase &database)
{
    return static_cast<int>(countScalar(database, QStringLiteral("SELECT COUNT(*) FROM features")));
}

bool hasUngroupedOkRows(QSqlDatabase &database)
{
    return countScalar(database, QStringLiteral(
               "SELECT COUNT(*) FROM files WHERE status = 'ok' AND content_group_id IS NULL")) > 0;
}

void beginTransaction(QSqlDatabase &database)
{
    if (!database.transaction()) {
        fail(database.lastError().text());
    }
}

void commitTransaction(QSqlDatabase &database)
{
    if (!database.commit()) {
        fail(database.lastError().text());
    }
}

void upsertMeta(QSqlDatabase &database, const QString &key, const QString &value)
{
    QSqlQuery query(database);
    prepareOrFail(query, QStringLiteral(
                             "INSERT INTO meta(key, value) VALUES(?, ?) "
                             "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    query.addBindValue(key);
    query.addBindValue(value);
    execPrepared(query);
}

void writeLastScanSummary(QSqlDatabase &database, int scanned, int skipped, int failed,
                          double elapsedSecs, const QString &power)
{
    const double meanMsPerTrack = scanned > 0
        ? elapsedSecs * 1000.0 / static_cast<double>(scanned)
        : 0.0;
    upsertMeta(database, QStringLiteral("last_scan_finished_at"),
               QString::number(QDateTime::currentSecsSinceEpoch()));
    upsertMeta(database, QStringLiteral("last_scan_elapsed_secs"), QString::number(elapsedSecs, 'f', 3));
    upsertMeta(database, QStringLiteral("last_scan_scanned"), QString::number(scanned));
    upsertMeta(database, QStringLiteral("last_scan_skipped"), QString::number(skipped));
    upsertMeta(database, QStringLiteral("last_scan_failed"), QString::number(failed));
    upsertMeta(database, QStringLiteral("last_scan_mean_ms_per_track"), QString::number(meanMsPerTrack, 'f', 3));
    upsertMeta(database, QStringLiteral("last_scan_power"), power);
}

void emitJson(const QJsonObject &object)
{
    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);
    out << QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)) << '\n';
}

void emitPlain(const QJsonObject &object)
{
    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.value().isObject()) {
            out << it.key() << ": " << QString::fromUtf8(QJsonDocument(it.value().toObject()).toJson(QJsonDocument::Compact)) << '\n';
        } else {
            out << it.key() << ": " << it.value().toVariant().toString() << '\n';
        }
    }
}

void emitVerboseFile(const FileAnalysis &analysis)
{
    QTextStream err(stderr);
    err.setEncoding(QStringConverter::Utf8);
    err << "file " << analysis.status
        << " decode=" << analysis.timings.decodeMs
        << " hash=" << analysis.timings.hashMs
        << " dsp=" << analysis.timings.dspMs
        << " fp=" << analysis.timings.fpMs
        << ' ' << analysis.candidate.path << '\n';
    err.flush();
}

void emitVerboseFeatureRepresentative(const FeatureAnalysis &analysis)
{
    QTextStream err(stderr);
    err.setEncoding(QStringConverter::Utf8);
    err << "representative " << (analysis.failed ? "decode_failed" : "ok")
        << " decode=" << analysis.timings.decodeMs
        << " dsp=" << analysis.timings.dspMs
        << ' ' << analysis.representative.path << '\n';
    err.flush();
}

// Recent throughput over a sliding window, not the lifetime average: after
// hours of scanning the lifetime mean barely moves, which hides the effect
// of a power change (or any slowdown) from the rate/ETA display.
class ProgressRate final {
public:
    double update(double elapsedSecs, int analyzed)
    {
        m_samples.push_back({elapsedSecs, analyzed});
        while (m_samples.size() > 2 && elapsedSecs - m_samples.front().elapsedSecs > kWindowSecs) {
            m_samples.pop_front();
        }
        const Sample &oldest = m_samples.front();
        const double span = elapsedSecs - oldest.elapsedSecs;
        if (span >= kMinSpanSecs && analyzed > oldest.analyzed) {
            return static_cast<double>(analyzed - oldest.analyzed) / span;
        }
        return elapsedSecs > 0.0 ? static_cast<double>(analyzed) / elapsedSecs : 0.0;
    }

private:
    struct Sample {
        double elapsedSecs = 0.0;
        int analyzed = 0;
    };
    static constexpr double kWindowSecs = 60.0;
    static constexpr double kMinSpanSecs = 2.0;
    std::deque<Sample> m_samples;
};

void emitProgress(int analyzed, int total, std::chrono::steady_clock::time_point started,
                  ProgressRate &recentRate)
{
    const double elapsedSecs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const double rate = recentRate.update(elapsedSecs, analyzed);
    const QString eta = rate > 0.0
        ? QString::number(static_cast<qint64>(std::ceil(static_cast<double>(total - analyzed) / rate)))
        : QStringLiteral("-");
    if (g_progressJsonl) {
        QJsonObject payload{
            {QStringLiteral("protocol_version"), 1},
            {QStringLiteral("request_id"), g_progressRequestId},
            {QStringLiteral("event"), QStringLiteral("progress")},
            {QStringLiteral("phase"), QStringLiteral("native-files")},
            {QStringLiteral("completed"), analyzed},
            {QStringLiteral("total"), total},
            {QStringLiteral("unit"), QStringLiteral("files")},
            {QStringLiteral("rate"), rate},
        };
        if (eta != QLatin1String("-")) {
            payload.insert(QStringLiteral("eta_seconds"), eta.toLongLong());
        }
        QTextStream(stdout) << QJsonDocument(payload).toJson(QJsonDocument::Compact) << '\n';
        return;
    }
    QTextStream err(stderr);
    err.setEncoding(QStringConverter::Utf8);
    err << "progress " << analyzed << '/' << total
        << " elapsed=" << QString::number(elapsedSecs, 'f', 1)
        << " rate=" << QString::number(rate, 'f', 1)
        << " eta=" << eta << '\n';
    err.flush();
}

void emitPhase(const QString &phase)
{
    if (g_progressJsonl) {
        const QJsonObject payload{
            {QStringLiteral("protocol_version"), 1},
            {QStringLiteral("request_id"), g_progressRequestId},
            {QStringLiteral("event"), QStringLiteral("phase")},
            {QStringLiteral("phase"), phase},
        };
        QTextStream(stdout) << QJsonDocument(payload).toJson(QJsonDocument::Compact) << '\n';
        return;
    }
    QTextStream err(stderr);
    err.setEncoding(QStringConverter::Utf8);
    err << "phase " << phase << '\n';
    err.flush();
}

QJsonObject runNativeRefresh(const RefreshOptions &options)
{
    g_stopRequested.store(false, std::memory_order_relaxed);
    std::signal(SIGTERM, requestStop);
    std::signal(SIGINT, requestStop);

    const EffectivePower effective = resolvePower(options);
    if (options.power != Power::Unspecified) {
        applyPowerPriority(effective.power);
    }
    const QString effectivePowerName = powerName(effective.power);

    const auto scanStarted = std::chrono::steady_clock::now();
    SqlConnection connection;
    QSqlDatabase database = openFeaturesDatabase(options.featuresPath, connection);
    initSchema(database);

    if (options.stage == Stage::Features) {
        if (options.progress) {
            emitPhase(QStringLiteral("grouping"));
        }
        // Identity scans leave every successful file grouped before they
        // return. A feature-only refresh must not rebuild 99k-file
        // Chromaprint groups merely to copy stale scalar rows; regroup only
        // when an interrupted/external workflow actually left ok rows
        // ungrouped. Keep emitting the existing phase marker so the progress
        // wire protocol and GUI state machine do not change.
        const int groups = hasUngroupedOkRows(database)
            ? regroupContent(database)
            : currentGroupCount(database);
        if (options.progress) {
            emitPhase(QStringLiteral("features"));
        }
        const FeatureFillResult fill = ensureFeatureRows(database, {}, effective.jobs, options.progress, options.verbose,
                                                         scanStarted);
        const int featured = currentFeatureCount(database);
        const QJsonObject payload = scanJson(0, 0, 0, groups, featured,
                                             static_cast<int>(staleFeatureCount(database)),
                                             std::chrono::duration<double>(
                                                 std::chrono::steady_clock::now() - scanStarted)
                                                 .count(),
                                             {},
                                             fill.canceled,
                                             effectivePowerName,
                                             effective.jobs,
                                             &fill);
        return payload;
    }

    const std::vector<Candidate> candidates = loadCandidates(options.libraryPath, options.limit);
    const auto [pending, skipped] = splitPending(database, candidates);
    // Ungrouped successful rows that predate this process (for example after
    // canceling between file analysis and grouping) have lost their previous
    // group-id context. Preserve exact split semantics by taking the full
    // fallback in that rare recovery case; rows made pending below retain
    // their previous ids in Candidate and use the incremental path.
    const bool hadUngroupedOkRows = hasUngroupedOkRows(database);
    if (pending.empty()) {
        FeatureFillResult fill;
        const FeatureFillResult *fillPtr = nullptr;
        if (options.stage == Stage::All) {
            if (hasUngroupedOkRows(database)) {
                if (options.progress) {
                    emitPhase(QStringLiteral("grouping"));
                }
                regroupContent(database);
            }
            if (options.progress) {
                emitPhase(QStringLiteral("features"));
            }
            fill = ensureFeatureRows(database, {}, effective.jobs, options.progress, options.verbose, scanStarted);
            fillPtr = &fill;
        }
        // No last-scan summary here: a run that analyzed nothing must not
        // clobber the meta rows describing the last real scan.
        const double elapsedSecs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - scanStarted).count();
        const QJsonObject payload =
            scanJson(0, skipped, 0, currentGroupCount(database), currentFeatureCount(database),
                     static_cast<int>(staleFeatureCount(database)),
                     elapsedSecs,
                     {},
                     fill.canceled,
                     effectivePowerName,
                     effective.jobs,
                     fillPtr);
        return payload;
    }

    int lastProgress = 0;
    auto lastProgressEmit = scanStarted;
    TimingAccumulator timings;
    timings.reserve(pending.size());
    QHash<QString, ScalarExtraction> extracted;
    QSet<qint64> affectedGroupIds;
    for (const Candidate &candidate : pending) {
        if (candidate.previousGroupId >= 0) {
            affectedGroupIds.insert(candidate.previousGroupId);
        }
    }
    int scanned = 0;
    int failed = 0;

    QSqlQuery fileFeaturesUpsert(database);
    prepareFileFeaturesUpsert(fileFeaturesUpsert);
    beginTransaction(database);
    int uncommitted = 0;
    auto lastCommit = std::chrono::steady_clock::now();
    const auto commitIfNeeded = [&](bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && uncommitted < 25
            && std::chrono::duration_cast<std::chrono::seconds>(now - lastCommit).count() < 5) {
            return;
        }
        if (uncommitted == 0) {
            return;
        }
        commitTransaction(database);
        beginTransaction(database);
        uncommitted = 0;
        lastCommit = now;
    };

    ProgressRate recentRate;
    const auto completion = [&](FileAnalysis &&analysis, int analyzed, int total) {
        if (options.verbose) {
            emitVerboseFile(analysis);
        }
        if (options.progress) {
            const auto now = std::chrono::steady_clock::now();
            if (analyzed == total || analyzed - lastProgress >= 25
                || std::chrono::duration_cast<std::chrono::seconds>(now - lastProgressEmit).count() >= 2) {
                emitProgress(analyzed, total, scanStarted, recentRate);
                lastProgress = analyzed;
                lastProgressEmit = now;
            }
        }
        if (analysis.status != QLatin1String("ok")) {
            ++failed;
        }
        timings.add(analysis.timings);
        recordScalarExtraction(extracted, analysis);
        upsertFile(database, analysis);
        if (analysis.status == QLatin1String("ok")) {
            upsertFileFeatures(fileFeaturesUpsert, analysis.candidate.path, analysis.scalars);
        }
        ++scanned;
        ++uncommitted;
        commitIfNeeded(false);
    };
    analyzePending(pending, effective.jobs, completion);
    commitIfNeeded(true);
    commitTransaction(database);

    if (stopRequested()) {
        const QJsonObject payload =
            scanJson(scanned, skipped, failed, currentGroupCount(database), currentFeatureCount(database),
                     static_cast<int>(staleFeatureCount(database)),
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - scanStarted).count(),
                     timingsJson(timings),
                     true,
                     effectivePowerName,
                     effective.jobs);
        return payload;
    }

    if (options.progress) {
        emitPhase(QStringLiteral("grouping"));
    }
    const int groups = hadUngroupedOkRows
        ? regroupContent(database)
        : regroupContentIncrementally(database, affectedGroupIds);
    FeatureFillResult fill;
    const FeatureFillResult *fillPtr = nullptr;
    if (options.stage == Stage::All) {
        if (options.progress) {
            emitPhase(QStringLiteral("features"));
        }
        fill = ensureFeatureRows(database, extracted, effective.jobs, options.progress, options.verbose, scanStarted);
        fillPtr = &fill;
    }
    const double elapsedSecs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - scanStarted).count();
    if (options.stage == Stage::All && !fill.canceled) {
        writeLastScanSummary(database, scanned, skipped, failed, elapsedSecs, effectivePowerName);
    }
    const QJsonObject payload =
        scanJson(scanned, skipped, failed, groups, currentFeatureCount(database),
                 static_cast<int>(staleFeatureCount(database)),
                 elapsedSecs,
                 timingsJson(timings),
                 fill.canceled,
                 effectivePowerName,
                 effective.jobs,
                 fillPtr);
    return payload;
}

void emitTerminalResult(const QJsonObject &payload, bool json, bool progressJsonl)
{
    if (progressJsonl) {
        const QJsonObject event{
            {QStringLiteral("protocol_version"), 1},
            {QStringLiteral("request_id"), g_progressRequestId},
            {QStringLiteral("event"), QStringLiteral("result")},
            {QStringLiteral("result"), payload},
        };
        QTextStream(stdout) << QJsonDocument(event).toJson(QJsonDocument::Compact) << '\n';
    } else if (json) {
        emitJson(payload);
    } else {
        emitPlain(payload);
    }
}

int providerFailureCode(const FeatureProvider::Invocation &invocation)
{
    if (invocation.exitCode == 2 || invocation.exitCode == 3 || invocation.exitCode == 5
        || invocation.exitCode == 130) {
        return invocation.exitCode;
    }
    return 4;
}

int runRefresh(const RefreshOptions &options)
{
    g_progressJsonl = options.progress;
    g_progressRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QLockFile lock(options.featuresPath + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) {
        failWithCode(5, QStringLiteral("feature store is busy: %1").arg(lock.error()));
    }

    QJsonObject result = runNativeRefresh(options);
    if (result.value(QStringLiteral("canceled")).toBool() || stopRequested()) {
        emitTerminalResult(result, options.json, options.progress);
        return 130;
    }

    const bool semanticEnabled = options.semantic.value_or(settingEnabled(readStateSetting(
        options.statePath, QStringLiteral("analysis.semantic.enabled"), QStringLiteral("false"))));
    if (!semanticEnabled) {
        result.insert(QStringLiteral("semantic"), QJsonObject{{QStringLiteral("state"), QStringLiteral("disabled")}});
        emitTerminalResult(result, options.json, options.progress);
        return 0;
    }

    const QString savedProvider = readStateSetting(
        options.statePath, QStringLiteral("analysis.semantic.providerPath"));
    const auto provider = FeatureProvider::discover(options.providerPath, savedProvider);
    if (!provider) {
        failWithCode(3, QStringLiteral("semantic analysis is enabled but muzaiten-features-clap was not found or failed its capability handshake"));
    }
    const QString device = readStateSetting(
        options.statePath, QStringLiteral("analysis.semantic.device"), QStringLiteral("auto"));
    QJsonObject scanParameters{
        {QStringLiteral("features"), options.featuresPath},
        {QStringLiteral("device"), device},
    };
    if (options.limit > 0) {
        scanParameters.insert(QStringLiteral("limit"), options.limit);
    }
    const FeatureProvider::Invocation scan = FeatureProvider::invoke(
        provider->path,
        QStringLiteral("scan"),
        scanParameters,
        options.progress,
        stopRequested,
        0);
    if (scan.exitCode != 0) {
        failWithCode(providerFailureCode(scan),
                     QStringLiteral("CLAP provider scan failed (%1): %2")
                         .arg(scan.errorCode, scan.errorMessage));
    }

    QJsonObject semantic{
        {QStringLiteral("state"), QStringLiteral("ready")},
        {QStringLiteral("provider_path"), provider->path},
        {QStringLiteral("provider_source"), provider->source},
        {QStringLiteral("capabilities"), provider->capabilities},
        {QStringLiteral("scan"), scan.result},
    };
    if (scan.result.value(QStringLiteral("embedded")).toInt() > 0) {
        const FeatureProvider::Invocation neighbor = FeatureProvider::invoke(
            provider->path,
            QStringLiteral("neighbors"),
            QJsonObject{{QStringLiteral("features"), options.featuresPath}},
            options.progress,
            stopRequested,
            0);
        if (neighbor.exitCode != 0) {
            failWithCode(providerFailureCode(neighbor),
                         QStringLiteral("CLAP provider neighbor rebuild failed (%1): %2")
                             .arg(neighbor.errorCode, neighbor.errorMessage));
        }
        semantic.insert(QStringLiteral("neighbors"), neighbor.result);
    } else {
        semantic.insert(QStringLiteral("neighbors"), QJsonObject{{QStringLiteral("state"), QStringLiteral("unchanged")}});
    }
    result.insert(QStringLiteral("semantic"), semantic);
    emitTerminalResult(result, options.json, options.progress);
    return 0;
}

int runStatus(const StatusOptions &options)
{
    SqlConnection connection;
    QSqlDatabase database = openFeaturesDatabase(options.featuresPath, connection);
    initSchema(database);
    QJsonObject payload = statusJson(database);
    const bool enabled = settingEnabled(readStateSetting(
        options.statePath, QStringLiteral("analysis.semantic.enabled"), QStringLiteral("false")));
    QJsonObject semantic{{QStringLiteral("enabled"), enabled}};
    const QString savedProvider = readStateSetting(
        options.statePath, QStringLiteral("analysis.semantic.providerPath"));
    if (const auto provider = FeatureProvider::discover(options.providerPath, savedProvider)) {
        semantic.insert(QStringLiteral("state"), QStringLiteral("provider-ready"));
        semantic.insert(QStringLiteral("provider_path"), provider->path);
        semantic.insert(QStringLiteral("provider_source"), provider->source);
        const auto providerStatus = FeatureProvider::invoke(
            provider->path,
            QStringLiteral("status"),
            QJsonObject{{QStringLiteral("features"), options.featuresPath}},
            false,
            [] { return false; },
            kProviderStatusTimeoutMs);
        if (providerStatus.exitCode == 0) {
            semantic.insert(QStringLiteral("provider"), providerStatus.result);
            const QJsonObject model = providerStatus.result.value(QStringLiteral("model")).toObject();
            if (!model.value(QStringLiteral("present")).toBool()
                || !model.value(QStringLiteral("valid")).toBool()) {
                semantic.insert(QStringLiteral("state"), QStringLiteral("model-missing"));
            } else if (!providerStatus.result.value(QStringLiteral("model_extra_installed")).toBool()) {
                semantic.insert(QStringLiteral("state"), QStringLiteral("provider-missing-component"));
            }
        } else {
            semantic.insert(QStringLiteral("state"), QStringLiteral("provider-error"));
            semantic.insert(QStringLiteral("error"), providerStatus.errorMessage);
        }
    } else {
        semantic.insert(QStringLiteral("state"), QStringLiteral("provider-missing"));
    }
    payload.insert(QStringLiteral("semantic"), semantic);
    options.json ? emitJson(payload) : emitPlain(payload);
    return 0;
}

ProviderOptions parseProviderOptions(QStringList arguments, bool queryCommand)
{
    ProviderOptions options;
    for (int index = 0; index < arguments.size(); ++index) {
        const QString word = arguments.at(index);
        if (word == QLatin1String("--features") || word == QLatin1String("--state")
            || word == QLatin1String("--provider") || word == QLatin1String("--device")) {
            if (index + 1 >= arguments.size()) {
                failWithCode(2, QStringLiteral("%1 needs a value").arg(word));
            }
            const QString value = arguments.at(++index);
            if (word == QLatin1String("--features")) {
                options.featuresPath = value;
            } else if (word == QLatin1String("--state")) {
                options.statePath = value;
            } else if (word == QLatin1String("--provider")) {
                options.providerPath = value;
            } else {
                if (value != QLatin1String("auto") && value != QLatin1String("cuda")
                    && value != QLatin1String("cpu")) {
                    failWithCode(2, QStringLiteral("--device must be auto, cuda, or cpu"));
                }
                options.device = value;
            }
        } else if (word == QLatin1String("--json")) {
            options.json = true;
        } else if (word == QLatin1String("--progress=jsonl")) {
            options.progress = true;
        } else if (word == QLatin1String("--force")) {
            options.force = true;
        } else if (queryCommand && !word.startsWith(QLatin1String("--")) && options.text.isEmpty()) {
            options.text = word;
        } else {
            failWithCode(2, QStringLiteral("unknown option \"%1\"").arg(word));
        }
    }
    if (options.featuresPath.isEmpty()) {
        options.featuresPath = QDir(AppPaths::dataDir()).filePath(QStringLiteral("features.sqlite"));
    }
    if (options.statePath.isEmpty()) {
        options.statePath = QDir(AppPaths::stateDir()).filePath(QStringLiteral("state.sqlite"));
    }
    if (options.json && options.progress) {
        failWithCode(2, QStringLiteral("accepts only one of --json and --progress=jsonl"));
    }
    if (queryCommand && options.text.trimmed().isEmpty()) {
        failWithCode(2, QStringLiteral("query needs non-empty text"));
    }
    return options;
}

FeatureProvider::Resolved requireProvider(const ProviderOptions &options)
{
    const QString saved = readStateSetting(
        options.statePath, QStringLiteral("analysis.semantic.providerPath"));
    const auto provider = FeatureProvider::discover(options.providerPath, saved);
    if (!provider) {
        failWithCode(3, QStringLiteral("muzaiten-features-clap was not found or failed its capability handshake"));
    }
    return *provider;
}

int runProviderOperation(const ProviderOptions &options,
                         const QString &operation,
                         const QJsonObject &parameters,
                         bool lockStore)
{
    g_stopRequested.store(false, std::memory_order_relaxed);
    std::signal(SIGTERM, requestStop);
    std::signal(SIGINT, requestStop);
    g_progressJsonl = options.progress;
    g_progressRequestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    std::optional<QLockFile> lock;
    if (lockStore) {
        lock.emplace(options.featuresPath + QStringLiteral(".lock"));
        if (!lock->tryLock(0)) {
            failWithCode(5, QStringLiteral("feature store is busy"));
        }
    }
    // Interactive queries skip the capabilities handshake: it doubles the
    // provider process count for an operation whose result is provenance-
    // checked downstream anyway. When the trusted candidate fails, fall back
    // to full discovery once in case a later candidate is the working one.
    const bool fastResolve = operation == QLatin1String("query");
    const int timeoutMs = fastResolve ? kProcessTimeoutMs : 0;
    FeatureProvider::Resolved provider;
    FeatureProvider::Invocation invocation;
    if (fastResolve) {
        const auto trusted = FeatureProvider::resolveTrusted(
            options.providerPath,
            readStateSetting(options.statePath, QStringLiteral("analysis.semantic.providerPath")));
        if (trusted) {
            provider = *trusted;
            invocation = FeatureProvider::invoke(
                provider.path, operation, parameters, options.progress, stopRequested, timeoutMs);
        }
        if (!trusted || invocation.exitCode != 0) {
            const QString trustedPath = trusted ? trusted->path : QString();
            const FeatureProvider::Resolved discovered = requireProvider(options);
            if (QFileInfo(discovered.path).canonicalFilePath()
                != QFileInfo(trustedPath).canonicalFilePath()) {
                provider = discovered;
                invocation = FeatureProvider::invoke(
                    provider.path, operation, parameters, options.progress, stopRequested, timeoutMs);
            }
        }
    } else {
        provider = requireProvider(options);
        invocation = FeatureProvider::invoke(
            provider.path, operation, parameters, options.progress, stopRequested, timeoutMs);
    }
    if (invocation.exitCode != 0) {
        failWithCode(providerFailureCode(invocation),
                     QStringLiteral("provider %1 failed (%2): %3")
                         .arg(operation, invocation.errorCode, invocation.errorMessage));
    }
    QJsonObject result = invocation.result;
    result.insert(QStringLiteral("provider_path"), provider.path);
    result.insert(QStringLiteral("provider_source"), provider.source);
    emitTerminalResult(result, options.json, options.progress);
    return 0;
}

int runModelDownload(const ProviderOptions &options)
{
    return runProviderOperation(options, QStringLiteral("model-download"), {}, false);
}

int runQuery(const ProviderOptions &options)
{
    return runProviderOperation(
        options,
        QStringLiteral("query"),
        QJsonObject{{QStringLiteral("text"), options.text},
                    {QStringLiteral("device"), options.device}},
        false);
}

int runNeighbors(const ProviderOptions &options)
{
    if (!options.force) {
        failWithCode(2, QStringLiteral("neighbors requires --force"));
    }
    return runProviderOperation(
        options,
        QStringLiteral("neighbors"),
        QJsonObject{{QStringLiteral("features"), options.featuresPath}},
        true);
}

void printUsage()
{
    std::fputs(
        "Usage: muzaiten-features <refresh|status|doctor|model download|query|neighbors> [options]\n"
        "\n"
        "refresh [--library PATH] [--features PATH] [--state PATH] [--semantic|--no-semantic] [--provider PATH]\n"
        "        [--limit N] [--jobs N] [--power background|balanced|turbo] [--json|--progress=jsonl] [--verbose]\n"
        "status [--features PATH] [--state PATH] [--provider PATH] [--json]\n"
        "doctor [--features PATH] [--state PATH] [--provider PATH] [--json]\n"
        "model download [--state PATH] [--provider PATH] [--json|--progress=jsonl]\n"
        "query TEXT [--state PATH] [--provider PATH] [--device auto|cuda|cpu] [--json]\n"
        "neighbors --force [--features PATH] [--state PATH] [--provider PATH] [--json|--progress=jsonl]\n",
        stderr);
}

Stage parseStage(const QString &value)
{
    if (value == QLatin1String("identity")) {
        return Stage::Identity;
    }
    if (value == QLatin1String("features")) {
        return Stage::Features;
    }
    if (value == QLatin1String("all")) {
        return Stage::All;
    }
    failWithCode(2, QStringLiteral("--stage must be identity, features, or all"));
}

Power parsePower(const QString &value)
{
    if (value == QLatin1String("background")) {
        return Power::Background;
    }
    if (value == QLatin1String("balanced")) {
        return Power::Balanced;
    }
    if (value == QLatin1String("turbo")) {
        return Power::Turbo;
    }
    failWithCode(2, QStringLiteral("--power must be background, balanced, or turbo"));
}

RefreshOptions parseRefresh(QStringList arguments)
{
    RefreshOptions options;
    for (int index = 0; index < arguments.size(); ++index) {
        const QString word = arguments.at(index);
        if (word == QLatin1String("--library")) {
            if (index + 1 >= arguments.size()) {
                failWithCode(2, QStringLiteral("refresh --library needs a path"));
            }
            options.libraryPath = arguments.at(++index);
        } else if (word == QLatin1String("--features")) {
            if (index + 1 >= arguments.size()) {
                failWithCode(2, QStringLiteral("refresh --features needs a path"));
            }
            options.featuresPath = arguments.at(++index);
        } else if (word == QLatin1String("--state")) {
            if (index + 1 >= arguments.size()) {
                failWithCode(2, QStringLiteral("refresh --state needs a path"));
            }
            options.statePath = arguments.at(++index);
        } else if (word == QLatin1String("--provider")) {
            if (index + 1 >= arguments.size()) {
                failWithCode(2, QStringLiteral("refresh --provider needs a path"));
            }
            options.providerPath = arguments.at(++index);
        } else if (word == QLatin1String("--limit")) {
            if (index + 1 >= arguments.size()) {
                failWithCode(2, QStringLiteral("refresh --limit needs a positive integer"));
            }
            bool ok = false;
            options.limit = arguments.at(++index).toInt(&ok);
            if (!ok || options.limit <= 0) {
                failWithCode(2, QStringLiteral("refresh --limit needs a positive integer"));
            }
        } else if (word == QLatin1String("--jobs")) {
            if (index + 1 >= arguments.size()) {
                failWithCode(2, QStringLiteral("refresh --jobs needs a positive integer"));
            }
            bool ok = false;
            options.jobs = arguments.at(++index).toInt(&ok);
            if (!ok || options.jobs <= 0) {
                failWithCode(2, QStringLiteral("refresh --jobs needs a positive integer"));
            }
        } else if (word == QLatin1String("--stage")) {
            if (index + 1 >= arguments.size()) {
                failWithCode(2, QStringLiteral("refresh --stage needs a value"));
            }
            options.stage = parseStage(arguments.at(++index));
        } else if (word == QLatin1String("--power")) {
            if (index + 1 >= arguments.size()) {
                failWithCode(2, QStringLiteral("refresh --power needs a value"));
            }
            options.power = parsePower(arguments.at(++index));
        } else if (word == QLatin1String("--json")) {
            options.json = true;
        } else if (word == QLatin1String("--progress=jsonl")) {
            options.progress = true;
        } else if (word == QLatin1String("--semantic")) {
            options.semantic = true;
        } else if (word == QLatin1String("--no-semantic")) {
            options.semantic = false;
        } else if (word == QLatin1String("--verbose")) {
            options.verbose = true;
        } else {
            failWithCode(2, QStringLiteral("unknown refresh option \"%1\"").arg(word));
        }
    }
    if (options.featuresPath.isEmpty()) {
        options.featuresPath = QDir(AppPaths::dataDir()).filePath(QStringLiteral("features.sqlite"));
    }
    if (options.libraryPath.isEmpty()) {
        options.libraryPath = QDir(AppPaths::dataDir()).filePath(QStringLiteral("library.sqlite"));
    }
    if (options.statePath.isEmpty()) {
        options.statePath = QDir(AppPaths::stateDir()).filePath(QStringLiteral("state.sqlite"));
    }
    if (options.json && options.progress) {
        failWithCode(2, QStringLiteral("refresh accepts only one of --json and --progress=jsonl"));
    }
    return options;
}

StatusOptions parseStatus(QStringList arguments)
{
    StatusOptions options;
    for (int index = 0; index < arguments.size(); ++index) {
        const QString word = arguments.at(index);
        if (word == QLatin1String("--features")) {
            if (index + 1 >= arguments.size()) {
                failWithCode(2, QStringLiteral("status --features needs a path"));
            }
            options.featuresPath = arguments.at(++index);
        } else if (word == QLatin1String("--json")) {
            options.json = true;
        } else if (word == QLatin1String("--state")) {
            if (index + 1 >= arguments.size()) {
                failWithCode(2, QStringLiteral("status --state needs a path"));
            }
            options.statePath = arguments.at(++index);
        } else if (word == QLatin1String("--provider")) {
            if (index + 1 >= arguments.size()) {
                failWithCode(2, QStringLiteral("status --provider needs a path"));
            }
            options.providerPath = arguments.at(++index);
        } else {
            failWithCode(2, QStringLiteral("unknown status option \"%1\"").arg(word));
        }
    }
    if (options.featuresPath.isEmpty()) {
        options.featuresPath = QDir(AppPaths::dataDir()).filePath(QStringLiteral("features.sqlite"));
    }
    if (options.statePath.isEmpty()) {
        options.statePath = QDir(AppPaths::stateDir()).filePath(QStringLiteral("state.sqlite"));
    }
    return options;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QStringList arguments = app.arguments();
    arguments.removeFirst();
    if (arguments.isEmpty()) {
        printUsage();
        return 2;
    }

    const QString command = arguments.takeFirst();
    try {
        if (command == QLatin1String("refresh")) {
            return runRefresh(parseRefresh(arguments));
        }
        if (command == QLatin1String("status")) {
            return runStatus(parseStatus(arguments));
        }
        if (command == QLatin1String("doctor")) {
            return runStatus(parseStatus(arguments));
        }
        if (command == QLatin1String("model")) {
            if (arguments.isEmpty() || arguments.takeFirst() != QLatin1String("download")) {
                failWithCode(2, QStringLiteral("model requires the download subcommand"));
            }
            return runModelDownload(parseProviderOptions(arguments, false));
        }
        if (command == QLatin1String("query")) {
            return runQuery(parseProviderOptions(arguments, true));
        }
        if (command == QLatin1String("neighbors")) {
            return runNeighbors(parseProviderOptions(arguments, false));
        }
        if (command == QLatin1String("--help") || command == QLatin1String("-h")) {
            printUsage();
            return 0;
        }
        failWithCode(2, QStringLiteral("unknown command \"%1\"").arg(command));
    } catch (const CommandError &error) {
        std::fprintf(stderr, "muzaiten-features: %s\n", error.what());
        return error.code();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "muzaiten-features: %s\n", error.what());
        return 4;
    }
}
