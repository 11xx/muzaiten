#include "indexer/Dsp.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
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

constexpr int kSchemaVersion = 3;
constexpr int kProcessTimeoutMs = 10 * 60 * 1000;
constexpr double kChromaprintBerThreshold = 0.15;
constexpr int kChromaprintOffsetFrames = 3;
constexpr qint64 kDurationBucketMs = 2'000;
constexpr const char *kExtractor = "muzaiten-dsp";
std::atomic_bool g_stopRequested = false;

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

struct ScanOptions {
    QString libraryPath;
    QString featuresPath;
    Stage stage = Stage::All;
    Power power = Power::Unspecified;
    int limit = -1;
    int jobs = 0;
    bool json = false;
    bool progress = false;
    bool verbose = false;
};

struct EffectivePower {
    Power power = Power::Turbo;
    int jobs = 1;
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

EffectivePower resolvePower(const ScanOptions &options)
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
    QHash<QString, QPair<qint64, qint64>> known;
    QSqlQuery existing(database);
    if (!existing.exec(QStringLiteral("SELECT path, mtime, size FROM files"))) {
        fail(existing.lastError().text());
    }
    while (existing.next()) {
        known.insert(existing.value(0).toString(),
                     {existing.value(1).toLongLong(), existing.value(2).toLongLong()});
    }

    std::vector<Candidate> pending;
    int skipped = 0;
    for (const Candidate &candidate : candidates) {
        const auto it = known.constFind(candidate.path);
        if (it != known.constEnd() && it->first == candidate.mtime && it->second == candidate.size) {
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
    decoded.pcm = std::move(pcm);
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

bool featureRowFresh(QSqlDatabase &database, qint64 groupId)
{
    QSqlQuery query(database);
    prepareOrFail(query, QStringLiteral("SELECT version FROM features WHERE content_group_id = ?"));
    query.addBindValue(groupId);
    execPrepared(query);
    return query.next() && !query.value(0).isNull()
        && query.value(0).toString() == QLatin1String(Dsp::kDspVersion);
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

struct FeatureFillResult {
    int processed = 0;
    int written = 0;
    int failed = 0;
    int total = 0;
    bool canceled = false;
};

struct StaleRep {
    qint64 groupId = 0;
    QString path;
};

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
    QTextStream err(stderr);
    err.setEncoding(QStringConverter::Utf8);
    const double elapsedSecs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - scanStarted).count();
    const double phaseElapsedSecs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - phaseStarted).count();
    const auto snapshot = phaseRate.update(phaseElapsedSecs, processed, total);
    err << "progress " << processed << '/' << total
        << " elapsed=" << QString::number(elapsedSecs, 'f', 1)
        << " rate=" << snapshot.rateText
        << " eta=" << snapshot.etaText << '\n';
    err.flush();
}

std::vector<StaleRep> materializeStaleReps(QSqlDatabase &database)
{
    // Matches featureRowFresh: missing row, older version, and NULL version
    // are stale; exact current version is fresh. Representative is MIN(path)
    // over ok file rows — same as the previous serial loop.
    QSqlQuery reps(database);
    prepareOrFail(reps, QStringLiteral(
                            "SELECT f.content_group_id, MIN(f.path) AS path "
                            "FROM files f "
                            "LEFT JOIN features feat ON feat.content_group_id = f.content_group_id "
                            "WHERE f.content_group_id IS NOT NULL AND f.status = 'ok' "
                            "  AND (feat.version IS NULL OR feat.version != ?) "
                            "GROUP BY f.content_group_id "
                            "ORDER BY f.content_group_id"));
    reps.addBindValue(QString::fromLatin1(Dsp::kDspVersion));
    execPrepared(reps);

    std::vector<StaleRep> stale;
    while (reps.next()) {
        stale.push_back(StaleRep{reps.value(0).toLongLong(), reps.value(1).toString()});
    }
    return stale;
}

FeatureFillResult ensureFeatureRows(QSqlDatabase &database,
                                    const QHash<QString, ScalarExtraction> &extracted,
                                    bool progress,
                                    std::chrono::steady_clock::time_point scanStarted)
{
    FeatureFillResult result;
    const std::vector<StaleRep> stale = materializeStaleReps(database);
    result.total = static_cast<int>(stale.size());

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

    for (const StaleRep &rep : stale) {
        if (stopRequested()) {
            result.canceled = true;
            break;
        }

        std::optional<Dsp::ScalarFeatures> scalars;
        try {
            scalars = scalarsForRepresentative(rep.path, extracted);
        } catch (const std::exception &) {
            ++result.failed;
            ++result.processed;
            maybeEmit(result.processed == result.total || result.canceled);
            if (stopRequested()) {
                result.canceled = true;
                break;
            }
            continue;
        }

        if (stopRequested()) {
            result.canceled = true;
            break;
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
        bindScalarRow(upsert, rep.groupId, scalars);
        execPrepared(upsert);
        ++result.written;
        ++result.processed;
        maybeEmit(result.processed == result.total);
    }

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

QJsonObject scanJson(int scanned, int skipped, int failed, int groups, int featured,
                     double elapsedSecs = 0.0, const QJsonObject &timings = {}, bool canceled = false,
                     const QString &power = QStringLiteral("turbo"), int jobs = 1,
                     const FeatureFillResult *featureFill = nullptr)
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
        {QStringLiteral("elapsed_secs"), elapsedSecs},
        {QStringLiteral("timings"), timings.isEmpty() ? timingsJson({}) : timings},
    };
    if (featureFill != nullptr) {
        object.insert(QStringLiteral("feature_groups_processed"), featureFill->processed);
        object.insert(QStringLiteral("features_written"), featureFill->written);
        object.insert(QStringLiteral("feature_groups_failed"), featureFill->failed);
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
    QTextStream err(stderr);
    err.setEncoding(QStringConverter::Utf8);
    const double elapsedSecs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const double rate = recentRate.update(elapsedSecs, analyzed);
    const QString eta = rate > 0.0
        ? QString::number(static_cast<qint64>(std::ceil(static_cast<double>(total - analyzed) / rate)))
        : QStringLiteral("-");
    err << "progress " << analyzed << '/' << total
        << " elapsed=" << QString::number(elapsedSecs, 'f', 1)
        << " rate=" << QString::number(rate, 'f', 1)
        << " eta=" << eta << '\n';
    err.flush();
}

void emitPhase(const QString &phase)
{
    QTextStream err(stderr);
    err.setEncoding(QStringConverter::Utf8);
    err << "phase " << phase << '\n';
    err.flush();
}

int runScan(const ScanOptions &options)
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
        const int groups = regroupContent(database);
        if (options.progress) {
            emitPhase(QStringLiteral("features"));
        }
        const FeatureFillResult fill = ensureFeatureRows(database, {}, options.progress, scanStarted);
        const int featured = currentFeatureCount(database);
        const QJsonObject payload = scanJson(0, 0, 0, groups, featured,
                                             std::chrono::duration<double>(
                                                 std::chrono::steady_clock::now() - scanStarted)
                                                 .count(),
                                             {},
                                             fill.canceled,
                                             effectivePowerName,
                                             effective.jobs,
                                             &fill);
        options.json ? emitJson(payload) : emitPlain(payload);
        return 0;
    }

    const std::vector<Candidate> candidates = loadCandidates(options.libraryPath, options.limit);
    const auto [pending, skipped] = splitPending(database, candidates);
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
            fill = ensureFeatureRows(database, {}, options.progress, scanStarted);
            fillPtr = &fill;
        }
        // No last-scan summary here: a run that analyzed nothing must not
        // clobber the meta rows describing the last real scan.
        const double elapsedSecs =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - scanStarted).count();
        const QJsonObject payload =
            scanJson(0, skipped, 0, currentGroupCount(database), currentFeatureCount(database),
                     elapsedSecs,
                     {},
                     fill.canceled,
                     effectivePowerName,
                     effective.jobs,
                     fillPtr);
        options.json ? emitJson(payload) : emitPlain(payload);
        return 0;
    }

    int lastProgress = 0;
    auto lastProgressEmit = scanStarted;
    TimingAccumulator timings;
    timings.reserve(pending.size());
    QHash<QString, ScalarExtraction> extracted;
    int scanned = 0;
    int failed = 0;

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
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - scanStarted).count(),
                     timingsJson(timings),
                     true,
                     effectivePowerName,
                     effective.jobs);
        options.json ? emitJson(payload) : emitPlain(payload);
        return 0;
    }

    if (options.progress) {
        emitPhase(QStringLiteral("grouping"));
    }
    const int groups = regroupContent(database);
    FeatureFillResult fill;
    const FeatureFillResult *fillPtr = nullptr;
    if (options.stage == Stage::All) {
        if (options.progress) {
            emitPhase(QStringLiteral("features"));
        }
        fill = ensureFeatureRows(database, extracted, options.progress, scanStarted);
        fillPtr = &fill;
    }
    const double elapsedSecs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - scanStarted).count();
    if (options.stage == Stage::All && !fill.canceled) {
        writeLastScanSummary(database, scanned, skipped, failed, elapsedSecs, effectivePowerName);
    }
    const QJsonObject payload =
        scanJson(scanned, skipped, failed, groups, currentFeatureCount(database),
                 elapsedSecs,
                 timingsJson(timings),
                 fill.canceled,
                 effectivePowerName,
                 effective.jobs,
                 fillPtr);
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
        "scan --library PATH --features PATH [--limit N] [--jobs N] [--power background|balanced|turbo] [--json] [--progress] [--verbose]\n"
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
    fail(QStringLiteral("--power must be background, balanced, or turbo"));
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
        } else if (word == QLatin1String("--power")) {
            if (index + 1 >= arguments.size()) {
                fail(QStringLiteral("scan --power needs a value"));
            }
            options.power = parsePower(arguments.at(++index));
        } else if (word == QLatin1String("--json")) {
            options.json = true;
        } else if (word == QLatin1String("--progress")) {
            options.progress = true;
        } else if (word == QLatin1String("--verbose")) {
            options.verbose = true;
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
