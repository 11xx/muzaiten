#include "indexer/Dsp.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <QUuid>
#include <QVariant>
#include <QtEndian>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <future>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

constexpr int kSchemaVersion = 3;
constexpr int kProcessTimeoutMs = 10 * 60 * 1000;
constexpr double kChromaprintBerThreshold = 0.15;
constexpr int kChromaprintOffsetFrames = 3;
constexpr qint64 kDurationBucketMs = 2'000;
constexpr const char *kExtractor = "muzaiten-dsp";

enum class Stage {
    Identity,
    Features,
    All,
};

struct ScanOptions {
    QString libraryPath;
    QString featuresPath;
    Stage stage = Stage::All;
    int limit = -1;
    int jobs = 0;
    bool json = false;
};

struct StatusOptions {
    QString featuresPath;
    bool json = false;
};

struct Candidate {
    QString path;
    qint64 mtime = 0;
    qint64 size = 0;
};

struct DecodedAudio {
    QByteArray pcm;
    std::vector<float> samples;
    qint64 durationMs = 0;
    QString decodeHash;
};

struct FileAnalysis {
    Candidate candidate;
    qint64 durationMs = -1;
    QString decodeHash;
    QByteArray chromaprint;
    std::optional<Dsp::ScalarFeatures> scalars;
    QString status = QStringLiteral("decode_failed");
};

struct GroupRow {
    QString path;
    qint64 durationMs = 0;
    QString decodeHash;
    std::vector<qint32> chromaprint;
};

struct ScalarExtraction {
    bool known = false;
    Dsp::ScalarFeatures features;
};

class SqlConnection final {
public:
    SqlConnection()
        : m_name(QStringLiteral("muzaiten-index-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
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

[[noreturn]] void fail(const QString &message)
{
    throw std::runtime_error(message.toStdString());
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

QVariant optionalVariant(const std::optional<double> &value)
{
    return value ? QVariant(*value) : QVariant();
}

QVariant scalarVariant(const std::optional<Dsp::ScalarFeatures> &features, double Dsp::ScalarFeatures::*member)
{
    return features ? QVariant((*features).*member) : QVariant();
}

void bindScalarRow(QSqlQuery &query, qint64 groupId, const std::optional<Dsp::ScalarFeatures> &features)
{
    query.addBindValue(groupId);
    query.addBindValue(features ? optionalVariant(features->tempoBpm) : QVariant());
    query.addBindValue(features ? optionalVariant(features->loudnessLufs) : QVariant());
    query.addBindValue(features ? optionalVariant(features->loudnessStdDb) : QVariant());
    query.addBindValue(scalarVariant(features, &Dsp::ScalarFeatures::spectralCentroidMeanHz));
    query.addBindValue(scalarVariant(features, &Dsp::ScalarFeatures::spectralCentroidStdHz));
    query.addBindValue(scalarVariant(features, &Dsp::ScalarFeatures::spectralFlatnessMean));
    query.addBindValue(scalarVariant(features, &Dsp::ScalarFeatures::zeroCrossingRate));
    query.addBindValue(scalarVariant(features, &Dsp::ScalarFeatures::onsetRateHz));
    query.addBindValue(features ? optionalVariant(features->energy) : QVariant());
    query.addBindValue(QString::fromLatin1(kExtractor));
    query.addBindValue(QString::fromLatin1(Dsp::kDspVersion));
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

int readSchemaVersion(QSqlDatabase &database)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("SELECT value FROM meta WHERE key = 'schema_version'")) || !query.next()) {
        return -1;
    }
    bool ok = false;
    const int version = query.value(0).toString().toInt(&ok);
    return ok ? version : -1;
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

bool featuresTableIsV3(QSqlDatabase &database)
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

void initSchema(QSqlDatabase &database)
{
    execSql(database, QStringLiteral(
                          "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT)"));
    const int version = readSchemaVersion(database);

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

    if (version != kSchemaVersion || !featuresTableIsV3(database)) {
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
    std::vector<Candidate> pending;
    int skipped = 0;
    for (const Candidate &candidate : candidates) {
        QSqlQuery query(database);
        prepareOrFail(query, QStringLiteral("SELECT mtime, size FROM files WHERE path = ?"));
        query.addBindValue(candidate.path);
        execPrepared(query);
        if (query.next() && query.value(0).toLongLong() == candidate.mtime
            && query.value(1).toLongLong() == candidate.size) {
            ++skipped;
        } else {
            pending.push_back(candidate);
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

DecodedAudio decodeCanonical(const QString &path)
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
    samples.reserve(static_cast<std::size_t>(sampleCount));
    const auto *data = reinterpret_cast<const uchar *>(pcm.constData());
    for (qsizetype index = 0; index < sampleCount; ++index) {
        const quint32 raw = qFromLittleEndian<quint32>(
            data + index * static_cast<qsizetype>(sizeof(quint32)));
        float sample = 0.0F;
        static_assert(sizeof(sample) == sizeof(raw));
        std::memcpy(&sample, &raw, sizeof(sample));
        samples.push_back(sample);
    }

    DecodedAudio decoded;
    decoded.pcm = std::move(pcm);
    decoded.samples = std::move(samples);
    decoded.durationMs = static_cast<qint64>(
        std::llround(static_cast<double>(sampleCount) * 1000.0 / static_cast<double>(Dsp::kSampleRateHz)));
    decoded.decodeHash = QString::fromLatin1(
        QCryptographicHash::hash(decoded.pcm, QCryptographicHash::Sha256).toHex());
    return decoded;
}

QByteArray encodeFingerprint(const QJsonArray &fingerprint)
{
    QByteArray blob;
    blob.resize(static_cast<qsizetype>(fingerprint.size() * static_cast<qsizetype>(sizeof(qint32))));
    auto *data = reinterpret_cast<uchar *>(blob.data());
    for (qsizetype index = 0; index < fingerprint.size(); ++index) {
        const QJsonValue value = fingerprint.at(index);
        if (!value.isDouble()) {
            fail(QStringLiteral("fpcalc returned a non-numeric fingerprint value"));
        }
        bool ok = false;
        const qint32 raw = QString::number(value.toInt()).toInt(&ok);
        if (!ok) {
            fail(QStringLiteral("fpcalc returned an invalid fingerprint value"));
        }
        qToLittleEndian<qint32>(raw, data + index * static_cast<qsizetype>(sizeof(qint32)));
    }
    return blob;
}

QByteArray fpcalcFingerprint(const QString &path)
{
    QProcess process;
    process.start(QStringLiteral("fpcalc"), {
        QStringLiteral("-raw"),
        QStringLiteral("-json"),
        QStringLiteral("-length"),
        QStringLiteral("120"),
        path,
    });
    if (!process.waitForStarted(5000)) {
        fail(QStringLiteral("spawning fpcalc for %1").arg(path));
    }
    if (!process.waitForFinished(kProcessTimeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        fail(QStringLiteral("fpcalc timed out for %1").arg(path));
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString detail = compactProcessError(process);
        fail(QStringLiteral("fpcalc failed for %1%2").arg(path, detail.isEmpty() ? QString() : QStringLiteral(": ") + detail));
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(process.readAllStandardOutput(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        fail(QStringLiteral("fpcalc returned invalid JSON for %1").arg(path));
    }
    const QJsonArray fingerprint = document.object().value(QStringLiteral("fingerprint")).toArray();
    if (fingerprint.isEmpty()) {
        fail(QStringLiteral("fpcalc returned an empty fingerprint for %1").arg(path));
    }
    return encodeFingerprint(fingerprint);
}

FileAnalysis analyzeCandidate(const Candidate &candidate)
{
    FileAnalysis analysis;
    analysis.candidate = candidate;
    try {
        DecodedAudio decoded = decodeCanonical(candidate.path);
        analysis.durationMs = decoded.durationMs;
        analysis.decodeHash = decoded.decodeHash;
        analysis.scalars = Dsp::analyze(decoded.samples, Dsp::kSampleRateHz);
        try {
            analysis.chromaprint = fpcalcFingerprint(candidate.path);
            analysis.status = QStringLiteral("ok");
        } catch (const std::exception &) {
            analysis.status = QStringLiteral("fp_failed");
        }
    } catch (const std::exception &) {
        analysis.status = QStringLiteral("decode_failed");
    }
    return analysis;
}

std::vector<FileAnalysis> analyzePending(const std::vector<Candidate> &pending, int jobs)
{
    if (pending.empty()) {
        return {};
    }
    const std::size_t workerCount = jobs > 0 ? static_cast<std::size_t>(jobs) : std::thread::hardware_concurrency();
    const std::size_t boundedWorkers = std::max<std::size_t>(1, workerCount);
    if (boundedWorkers == 1 || pending.size() == 1) {
        std::vector<FileAnalysis> analyses;
        analyses.reserve(pending.size());
        for (const Candidate &candidate : pending) {
            analyses.push_back(analyzeCandidate(candidate));
        }
        return analyses;
    }

    std::deque<std::future<FileAnalysis>> futures;
    std::vector<FileAnalysis> analyses;
    analyses.reserve(pending.size());
    for (const Candidate &candidate : pending) {
        while (futures.size() >= boundedWorkers) {
            analyses.push_back(futures.front().get());
            futures.pop_front();
        }
        futures.push_back(std::async(std::launch::async, [candidate]() {
            return analyzeCandidate(candidate);
        }));
    }
    while (!futures.empty()) {
        analyses.push_back(futures.front().get());
        futures.pop_front();
    }
    return analyses;
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
            "SELECT path, duration_ms, decode_hash, chromaprint_fp "
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
        });
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

    for (std::size_t left = 0; left < rows.size(); ++left) {
        for (std::size_t right = left + 1; right < rows.size(); ++right) {
            if (std::llabs(rows[left].durationMs - rows[right].durationMs) > kDurationBucketMs) {
                continue;
            }
            if (bestBitErrorRate(rows[left].chromaprint, rows[right].chromaprint) < kChromaprintBerThreshold) {
                unionFind.unite(left, right);
            }
        }
    }

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

int regroupContent(QSqlDatabase &database, bool clearFeatures)
{
    const std::vector<GroupRow> rows = loadGroupRows(database);
    const std::vector<std::vector<std::size_t>> groups = groupedRows(rows);

    if (!database.transaction()) {
        fail(database.lastError().text());
    }
    execSql(database, QStringLiteral("UPDATE files SET content_group_id = NULL"));
    execSql(database, QStringLiteral("DELETE FROM content_groups"));
    QSqlQuery reset(database);
    reset.exec(QStringLiteral("DELETE FROM sqlite_sequence WHERE name = 'content_groups'"));
    if (clearFeatures) {
        execSql(database, QStringLiteral("DELETE FROM features"));
    }

    qint64 groupId = 1;
    for (const std::vector<std::size_t> &members : groups) {
        QSqlQuery insertGroup(database);
        prepareOrFail(insertGroup, QStringLiteral("INSERT INTO content_groups(id) VALUES(?)"));
        insertGroup.addBindValue(groupId);
        execPrepared(insertGroup);

        for (std::size_t member : members) {
            QSqlQuery updateFile(database);
            prepareOrFail(updateFile, QStringLiteral("UPDATE files SET content_group_id = ? WHERE path = ?"));
            updateFile.addBindValue(groupId);
            updateFile.addBindValue(rows[member].path);
            execPrepared(updateFile);
        }
        ++groupId;
    }
    execSql(database, QStringLiteral(
                          "DELETE FROM features WHERE content_group_id NOT IN "
                          "(SELECT id FROM content_groups)"));
    if (!database.commit()) {
        fail(database.lastError().text());
    }
    return static_cast<int>(groups.size());
}

QHash<QString, ScalarExtraction> scalarMapFromAnalyses(const std::vector<FileAnalysis> &analyses)
{
    QHash<QString, ScalarExtraction> extracted;
    for (const FileAnalysis &analysis : analyses) {
        if (analysis.status != QLatin1String("ok")) {
            continue;
        }
        ScalarExtraction row;
        row.known = analysis.scalars.has_value();
        if (analysis.scalars) {
            row.features = *analysis.scalars;
        }
        extracted.insert(analysis.candidate.path, row);
    }
    return extracted;
}

bool featureRowFresh(QSqlDatabase &database, qint64 groupId)
{
    QSqlQuery query(database);
    prepareOrFail(query, QStringLiteral("SELECT version FROM features WHERE content_group_id = ?"));
    query.addBindValue(groupId);
    execPrepared(query);
    return query.next() && query.value(0).toString() == QLatin1String(Dsp::kDspVersion);
}

std::optional<Dsp::ScalarFeatures> scalarsForRepresentative(const QString &path,
                                                            const QHash<QString, ScalarExtraction> &extracted)
{
    const auto it = extracted.constFind(path);
    if (it != extracted.constEnd()) {
        return it->known ? std::optional<Dsp::ScalarFeatures>(it->features) : std::nullopt;
    }
    DecodedAudio decoded = decodeCanonical(path);
    return Dsp::analyze(decoded.samples, Dsp::kSampleRateHz);
}

int ensureFeatureRows(QSqlDatabase &database, const QHash<QString, ScalarExtraction> &extracted)
{
    QSqlQuery reps(database);
    if (!reps.exec(QStringLiteral(
            "SELECT content_group_id, MIN(path) AS path "
            "FROM files "
            "WHERE content_group_id IS NOT NULL AND status = 'ok' "
            "GROUP BY content_group_id ORDER BY content_group_id"))) {
        fail(reps.lastError().text());
    }

    int written = 0;
    while (reps.next()) {
        const qint64 groupId = reps.value(0).toLongLong();
        if (featureRowFresh(database, groupId)) {
            continue;
        }
        const QString path = reps.value(1).toString();
        std::optional<Dsp::ScalarFeatures> scalars;
        try {
            scalars = scalarsForRepresentative(path, extracted);
        } catch (const std::exception &) {
            continue;
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
        bindScalarRow(upsert, groupId, scalars);
        execPrepared(upsert);
        ++written;
    }
    return written;
}

qint64 countScalar(QSqlDatabase &database, const QString &sql)
{
    QSqlQuery query(database);
    if (!query.exec(sql) || !query.next()) {
        return 0;
    }
    return query.value(0).toLongLong();
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
    return QJsonObject{
        {QStringLiteral("schema_version"), kSchemaVersion},
        {QStringLiteral("dsp_version"), QString::fromLatin1(Dsp::kDspVersion)},
        {QStringLiteral("files"), static_cast<double>(files)},
        {QStringLiteral("statuses"), statuses},
        {QStringLiteral("groups"), static_cast<double>(groups)},
        {QStringLiteral("features"), static_cast<double>(featured)},
        {QStringLiteral("featured_groups"), static_cast<double>(featured)},
        {QStringLiteral("pending"), 0},
    };
}

QJsonObject scanJson(int scanned, int skipped, int failed, int groups, int featured)
{
    return QJsonObject{
        {QStringLiteral("schema_version"), kSchemaVersion},
        {QStringLiteral("scanned"), scanned},
        {QStringLiteral("skipped"), skipped},
        {QStringLiteral("failed"), failed},
        {QStringLiteral("groups"), groups},
        {QStringLiteral("dsp_version"), QString::fromLatin1(Dsp::kDspVersion)},
        {QStringLiteral("featured_groups"), featured},
    };
}

int currentGroupCount(QSqlDatabase &database)
{
    return static_cast<int>(countScalar(database, QStringLiteral("SELECT COUNT(*) FROM content_groups")));
}

int currentFeatureCount(QSqlDatabase &database)
{
    return static_cast<int>(countScalar(database, QStringLiteral("SELECT COUNT(*) FROM features")));
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

int runScan(const ScanOptions &options)
{
    SqlConnection connection;
    QSqlDatabase database = openFeaturesDatabase(options.featuresPath, connection);
    initSchema(database);

    if (options.stage == Stage::Features) {
        const int groups = regroupContent(database, false);
        ensureFeatureRows(database, {});
        const int featured = currentFeatureCount(database);
        const QJsonObject payload = scanJson(0, 0, 0, groups, featured);
        options.json ? emitJson(payload) : emitPlain(payload);
        return 0;
    }

    const std::vector<Candidate> candidates = loadCandidates(options.libraryPath, options.limit);
    const auto [pending, skipped] = splitPending(database, candidates);
    if (pending.empty()) {
        if (options.stage == Stage::All) {
            ensureFeatureRows(database, {});
        }
        const QJsonObject payload =
            scanJson(0, skipped, 0, currentGroupCount(database), currentFeatureCount(database));
        options.json ? emitJson(payload) : emitPlain(payload);
        return 0;
    }

    const std::vector<FileAnalysis> analyses = analyzePending(pending, options.jobs);
    int failed = 0;
    for (const FileAnalysis &analysis : analyses) {
        if (analysis.status != QLatin1String("ok")) {
            ++failed;
        }
        upsertFile(database, analysis);
    }

    const int groups = regroupContent(database, options.stage == Stage::All);
    if (options.stage == Stage::All) {
        ensureFeatureRows(database, scalarMapFromAnalyses(analyses));
    }
    const QJsonObject payload =
        scanJson(static_cast<int>(analyses.size()), skipped, failed, groups, currentFeatureCount(database));
    options.json ? emitJson(payload) : emitPlain(payload);
    return 0;
}

int runStatus(const StatusOptions &options)
{
    SqlConnection connection;
    QSqlDatabase database = openFeaturesDatabase(options.featuresPath, connection);
    initSchema(database);
    const QJsonObject payload = statusJson(database);
    options.json ? emitJson(payload) : emitPlain(payload);
    return 0;
}

void printUsage()
{
    std::fputs(
        "Usage: muzaiten-index <scan|status> [options]\n"
        "\n"
        "scan --library PATH --features PATH [--limit N] [--jobs N] [--json]\n"
        "status --features PATH [--json]\n",
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
    fail(QStringLiteral("--stage must be identity, features, or all"));
}

ScanOptions parseScan(QStringList arguments)
{
    ScanOptions options;
    for (int index = 0; index < arguments.size(); ++index) {
        const QString word = arguments.at(index);
        if (word == QLatin1String("--library")) {
            if (index + 1 >= arguments.size()) {
                fail(QStringLiteral("scan --library needs a path"));
            }
            options.libraryPath = arguments.at(++index);
        } else if (word == QLatin1String("--features")) {
            if (index + 1 >= arguments.size()) {
                fail(QStringLiteral("scan --features needs a path"));
            }
            options.featuresPath = arguments.at(++index);
        } else if (word == QLatin1String("--limit")) {
            if (index + 1 >= arguments.size()) {
                fail(QStringLiteral("scan --limit needs a positive integer"));
            }
            bool ok = false;
            options.limit = arguments.at(++index).toInt(&ok);
            if (!ok || options.limit <= 0) {
                fail(QStringLiteral("scan --limit needs a positive integer"));
            }
        } else if (word == QLatin1String("--jobs")) {
            if (index + 1 >= arguments.size()) {
                fail(QStringLiteral("scan --jobs needs a positive integer"));
            }
            bool ok = false;
            options.jobs = arguments.at(++index).toInt(&ok);
            if (!ok || options.jobs <= 0) {
                fail(QStringLiteral("scan --jobs needs a positive integer"));
            }
        } else if (word == QLatin1String("--stage")) {
            if (index + 1 >= arguments.size()) {
                fail(QStringLiteral("scan --stage needs a value"));
            }
            options.stage = parseStage(arguments.at(++index));
        } else if (word == QLatin1String("--json")) {
            options.json = true;
        } else {
            fail(QStringLiteral("unknown scan option \"%1\"").arg(word));
        }
    }
    if (options.featuresPath.isEmpty()) {
        fail(QStringLiteral("scan needs --features"));
    }
    if (options.stage != Stage::Features && options.libraryPath.isEmpty()) {
        fail(QStringLiteral("scan needs --library"));
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
                fail(QStringLiteral("status --features needs a path"));
            }
            options.featuresPath = arguments.at(++index);
        } else if (word == QLatin1String("--json")) {
            options.json = true;
        } else {
            fail(QStringLiteral("unknown status option \"%1\"").arg(word));
        }
    }
    if (options.featuresPath.isEmpty()) {
        fail(QStringLiteral("status needs --features"));
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
        if (command == QLatin1String("scan")) {
            return runScan(parseScan(arguments));
        }
        if (command == QLatin1String("status")) {
            return runStatus(parseStatus(arguments));
        }
        if (command == QLatin1String("--help") || command == QLatin1String("-h")) {
            printUsage();
            return 0;
        }
        fail(QStringLiteral("unknown command \"%1\"").arg(command));
    } catch (const std::exception &error) {
        std::fprintf(stderr, "muzaiten-index: %s\n", error.what());
        return 1;
    }
}
