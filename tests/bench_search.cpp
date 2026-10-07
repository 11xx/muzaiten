// Offline search probes. Built by default, deliberately absent from CTest.
// Usage: bench_search --library PATH [--scale N] [--queries FILE] [--repeat R]
//        bench_search --append-capacity

#include "db/Database.h"
#include "search/IndexCache.h"
#include "search/SearchIndex.h"
#include "search/SearchMatcher.h"

#include <QCoreApplication>
#include <QByteArrayView>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <QTextStream>

#include <algorithm>
#include <cstdio>
#include <random>
#include <climits>

#ifdef Q_OS_LINUX
#include <sys/resource.h>
#endif

namespace Search {

struct SearchIndexBenchmarkAccess {
    struct Snapshot {
        qsizetype recordCapacity = 0;
        const void *recordData = nullptr;
        std::size_t maskCapacity = 0;
        const void *maskData = nullptr;
        std::size_t pathSegmentCapacity = 0;
        const void *pathSegmentData = nullptr;
        std::size_t pathOffsetCapacity = 0;
        const void *pathOffsetData = nullptr;
        std::size_t folderCapacity = 0;
    };

    static Snapshot snapshot(const SearchIndex &index)
    {
        return {
            index.m_records.capacity(), index.m_records.constData(),
            index.m_masks.capacity(), index.m_masks.data(),
            index.m_pathSegments.capacity(), index.m_pathSegments.data(),
            index.m_pathOffsets.capacity(), index.m_pathOffsets.data(),
            static_cast<std::size_t>(index.m_folderIds.capacity()),
        };
    }
};

} // namespace Search

namespace {

using Records = QVector<Search::SearchRecord>;

double ms(qint64 ns);
qint64 percentile(QVector<qint64> values, double fraction);

constexpr int kAppendCapacityBaseRecords = 29'921;
constexpr int kAppendCapacityScale = 10;
constexpr int kAppendCapacityBatchSize = 3'000;
constexpr int kAppendCapacityRepeats = 3;

using IndexSnapshot = Search::SearchIndexBenchmarkAccess::Snapshot;

struct GrowthObservations {
    quint64 capacityGrowths = 0;
    quint64 storageMoves = 0;
};

void observeGrowth(GrowthObservations &observations, qsizetype oldCapacity,
                   const void *oldData, qsizetype newCapacity, const void *newData,
                   bool skipAdoptedStorage)
{
    if (newCapacity > oldCapacity && !(skipAdoptedStorage && oldCapacity == 0)) {
        ++observations.capacityGrowths;
    }
    if (oldCapacity > 0 && oldData != newData) ++observations.storageMoves;
}

void observeGrowth(GrowthObservations &observations, std::size_t oldCapacity,
                   const void *oldData, std::size_t newCapacity, const void *newData,
                   bool skipAdoptedStorage)
{
    if (newCapacity > oldCapacity && !(skipAdoptedStorage && oldCapacity == 0)) {
        ++observations.capacityGrowths;
    }
    if (oldCapacity > 0 && oldData != newData) ++observations.storageMoves;
}

Records syntheticSearchRecords(int first, int count, QHash<QString, QString> &normPool)
{
    Records records;
    records.reserve(count);
    for (int offset = 0; offset < count; ++offset) {
        const int number = first + offset;
        const QString id = QString::number(number).rightJustified(6, QLatin1Char('0'));
        const QString artistNumber = QString::number(number % 128).rightJustified(3, QLatin1Char('0'));
        const QString albumNumber = QString::number(number / 12).rightJustified(5, QLatin1Char('0'));
        Search::SearchRecord record;
        record.title = QStringLiteral("Track ") + id;
        record.artistName = QStringLiteral("Artist ") + artistNumber;
        record.albumArtistName = record.artistName;
        record.albumTitle = QStringLiteral("Album ") + albumNumber;
        record.date = QStringLiteral("2026");
        record.filename = QStringLiteral("Track ") + id + QStringLiteral(".flac");
        record.path = QStringLiteral("/synthetic/artist-") + artistNumber
            + QStringLiteral("/album-") + albumNumber + QLatin1Char('/') + record.filename;
        record.codec = QStringLiteral("flac");
        record.durationMs = 180'000 + number;
        record.trackNumber = number % 12 + 1;
        record.discNumber = number / 1'000 + 1;
        Search::foldRecordNorms(record, &normPool);
        records.push_back(std::move(record));
    }
    return records;
}

void hashString(QCryptographicHash &hash, const QString &value)
{
    const QByteArray bytes = value.toUtf8();
    hash.addData(QByteArrayView(bytes));
    hash.addData(QByteArrayView("\0", 1));
}

QByteArray recordOrderDigest(const Search::SearchIndex &index)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const Search::SearchRecord &record : index.records()) {
        hashString(hash, record.title);
        hashString(hash, record.path);
        hashString(hash, record.normTitle);
        hashString(hash, record.normPath);
    }
    return hash.result().toHex();
}

QByteArray queryDigest(const Search::SearchIndex &index)
{
    static const QStringList queryTexts = {
        QStringLiteral("title:track 000042"),
        QStringLiteral("artist:artist 042"),
        QStringLiteral("path:album 00012"),
        QStringLiteral("track 009920"),
    };
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QString &text : queryTexts) {
        for (const bool fuzzy : {false, true}) {
            const Search::SearchQuery query = Search::SearchQuery::parse(text);
            int total = 0;
            QVector<int> matchingIndices;
            const auto results = index.match(query, fuzzy, {}, &total, &matchingIndices);
            hashString(hash, text);
            hash.addData(QByteArrayView(fuzzy ? "fuzzy\0" : "plain\0", 6));
            hash.addData(QByteArray::number(total));
            hash.addData(QByteArrayView("\0", 1));
            for (int matchIndex : matchingIndices) {
                hash.addData(QByteArray::number(matchIndex));
                hash.addData(QByteArrayView("\0", 1));
            }
            for (const Search::ScoredResult &result : results) {
                hash.addData(QByteArray::number(result.index));
                hash.addData(QByteArrayView(":", 1));
                hash.addData(QByteArray::number(result.score));
                hash.addData(QByteArrayView("\0", 1));
            }
        }
    }
    return hash.result().toHex();
}

qint64 peakResidentKiB()
{
#ifdef Q_OS_LINUX
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) return usage.ru_maxrss;
#endif
    return -1;
}

int appendCapacityWorker(const QString &method, int recordCount)
{
    if ((method != QStringLiteral("build") && method != QStringLiteral("append"))
        || (recordCount != kAppendCapacityBaseRecords
            && recordCount != kAppendCapacityBaseRecords * kAppendCapacityScale)) {
        std::fprintf(stderr, "invalid append-capacity worker arguments\n");
        return 2;
    }

    QHash<QString, QString> normPool;
    Search::SearchIndex index;
    const IndexSnapshot empty = Search::SearchIndexBenchmarkAccess::snapshot(index);
    GrowthObservations recordGrowth;
    GrowthObservations maskGrowth;
    qint64 elapsedNs = 0;
    int batches = 0;

    if (method == QStringLiteral("build")) {
        Records records = syntheticSearchRecords(0, recordCount, normPool);
        QElapsedTimer timer;
        timer.start();
        index.build(std::move(records));
        elapsedNs = timer.nsecsElapsed();
        const IndexSnapshot after = Search::SearchIndexBenchmarkAccess::snapshot(index);
        observeGrowth(maskGrowth, empty.maskCapacity, empty.maskData,
                      after.maskCapacity, after.maskData, false);
    } else {
        for (int first = 0; first < recordCount; first += kAppendCapacityBatchSize) {
            const int count = std::min(kAppendCapacityBatchSize, recordCount - first);
            Records batch = syntheticSearchRecords(first, count, normPool);
            const IndexSnapshot before = Search::SearchIndexBenchmarkAccess::snapshot(index);
            QElapsedTimer timer;
            timer.start();
            index.append(std::move(batch));
            elapsedNs += timer.nsecsElapsed();
            const IndexSnapshot after = Search::SearchIndexBenchmarkAccess::snapshot(index);
            const bool firstCallAdoptsRecords = batches == 0;
            observeGrowth(recordGrowth, before.recordCapacity, before.recordData,
                          after.recordCapacity, after.recordData, firstCallAdoptsRecords);
            observeGrowth(maskGrowth, before.maskCapacity, before.maskData,
                          after.maskCapacity, after.maskData, false);
            ++batches;
        }
    }

    const int expectedBatches = (recordCount + kAppendCapacityBatchSize - 1) / kAppendCapacityBatchSize;
    const quint64 expectedGeneration = method == QStringLiteral("build")
        ? 1 : static_cast<quint64>(expectedBatches);
    const QString recordDigest = QString::fromLatin1(recordOrderDigest(index));
    const QString queriesHash = QString::fromLatin1(queryDigest(index));
    int cancelledTotal = -1;
    QVector<int> cancelledIndices{1};
    const auto cancelledResults = index.match(Search::SearchQuery::parse(QStringLiteral("track")), false,
                                              {}, &cancelledTotal, &cancelledIndices, nullptr,
                                              [] { return true; });
    const bool cancellationPreserved = cancelledResults.isEmpty() && cancelledTotal == 0
        && cancelledIndices.isEmpty();
    const IndexSnapshot final = Search::SearchIndexBenchmarkAccess::snapshot(index);

    QJsonObject result;
    result.insert(QStringLiteral("method"), method);
    result.insert(QStringLiteral("records"), recordCount);
    result.insert(QStringLiteral("batches"), method == QStringLiteral("build") ? expectedBatches : batches);
    result.insert(QStringLiteral("elapsed_ns"), static_cast<double>(elapsedNs));
    result.insert(QStringLiteral("peak_rss_kib"), static_cast<double>(peakResidentKiB()));
    result.insert(QStringLiteral("record_capacity"), static_cast<double>(final.recordCapacity));
    result.insert(QStringLiteral("record_growths"), static_cast<double>(recordGrowth.capacityGrowths));
    result.insert(QStringLiteral("record_storage_moves"), static_cast<double>(recordGrowth.storageMoves));
    result.insert(QStringLiteral("mask_capacity"), static_cast<double>(final.maskCapacity));
    result.insert(QStringLiteral("mask_growths"), static_cast<double>(maskGrowth.capacityGrowths));
    result.insert(QStringLiteral("mask_storage_moves"), static_cast<double>(maskGrowth.storageMoves));
    result.insert(QStringLiteral("path_segment_capacity"), static_cast<double>(final.pathSegmentCapacity));
    result.insert(QStringLiteral("path_offset_capacity"), static_cast<double>(final.pathOffsetCapacity));
    result.insert(QStringLiteral("folder_capacity"), static_cast<double>(final.folderCapacity));
    result.insert(QStringLiteral("generation"), static_cast<double>(index.generation()));
    result.insert(QStringLiteral("generation_expected"), static_cast<double>(expectedGeneration));
    result.insert(QStringLiteral("record_order_sha256"), recordDigest);
    result.insert(QStringLiteral("query_results_sha256"), queriesHash);
    result.insert(QStringLiteral("cancellation_preserved"), cancellationPreserved);
    const QByteArray output = QJsonDocument(result).toJson(QJsonDocument::Compact);
    std::fwrite(output.constData(), 1, static_cast<std::size_t>(output.size()), stdout);
    std::fputc('\n', stdout);

    return index.size() == recordCount && index.generation() == expectedGeneration
            && cancellationPreserved ? 0 : 1;
}

bool runAppendCapacityWorker(const QString &method, int recordCount, QJsonObject *result)
{
    QProcess process;
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments({QStringLiteral("--append-capacity-worker"), method,
                          QString::number(recordCount)});
    process.start();
    if (!process.waitForStarted() || !process.waitForFinished(-1) || process.exitCode() != 0) return false;
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(process.readAllStandardOutput().trimmed(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    *result = document.object();
    return true;
}

bool sameStructuralResult(const QJsonObject &left, const QJsonObject &right)
{
    static const QStringList keys = {
        QStringLiteral("records"), QStringLiteral("batches"),
        QStringLiteral("record_capacity"), QStringLiteral("record_growths"),
        QStringLiteral("record_storage_moves"), QStringLiteral("mask_capacity"),
        QStringLiteral("mask_growths"), QStringLiteral("mask_storage_moves"),
        QStringLiteral("path_segment_capacity"), QStringLiteral("path_offset_capacity"),
        QStringLiteral("folder_capacity"), QStringLiteral("generation"),
        QStringLiteral("generation_expected"), QStringLiteral("record_order_sha256"),
        QStringLiteral("query_results_sha256"), QStringLiteral("cancellation_preserved"),
    };
    for (const QString &key : keys) {
        if (left.value(key) != right.value(key)) return false;
    }
    return true;
}

int appendCapacityBenchmark()
{
    std::printf("SearchIndex append capacity benchmark (synthetic data; 3,000-record batches)\n");
    std::printf("Fixture: %d records at 1x and %d at 10x; each method runs in a separate process\n",
                kAppendCapacityBaseRecords, kAppendCapacityBaseRecords * kAppendCapacityScale);
    std::printf("Elapsed time covers index build or append calls; peak RSS is the process high-water mark\n");

    for (int scale : {1, kAppendCapacityScale}) {
        const int recordCount = kAppendCapacityBaseRecords * scale;
        QJsonObject representative[2];
        QVector<qint64> elapsed[2];
        QVector<qint64> peakRss[2];
        bool initialized[2] = {false, false};
        for (int repeat = 0; repeat < kAppendCapacityRepeats; ++repeat) {
            for (int methodIndex = 0; methodIndex < 2; ++methodIndex) {
                const QString method = methodIndex == 0 ? QStringLiteral("build") : QStringLiteral("append");
                QJsonObject result;
                if (!runAppendCapacityWorker(method, recordCount, &result)) {
                    std::fprintf(stderr, "append-capacity worker failed for %s at scale %dx\n",
                                 qPrintable(method), scale);
                    return 1;
                }
                if (initialized[methodIndex]) {
                    if (!sameStructuralResult(representative[methodIndex], result)) {
                        std::fprintf(stderr, "append-capacity measurements varied structurally across runs\n");
                        return 1;
                    }
                } else {
                    representative[methodIndex] = result;
                    initialized[methodIndex] = true;
                }
                elapsed[methodIndex].append(static_cast<qint64>(result.value(QStringLiteral("elapsed_ns")).toDouble()));
                peakRss[methodIndex].append(static_cast<qint64>(result.value(QStringLiteral("peak_rss_kib")).toDouble()));
            }
        }

        if (representative[0].value(QStringLiteral("record_order_sha256"))
                != representative[1].value(QStringLiteral("record_order_sha256"))
            || representative[0].value(QStringLiteral("query_results_sha256"))
                != representative[1].value(QStringLiteral("query_results_sha256"))) {
            std::fprintf(stderr, "build and append produced different records or query results at scale %dx\n", scale);
            return 1;
        }

        std::printf("\n%d records (%dx)\n", recordCount, scale);
        for (int methodIndex = 0; methodIndex < 2; ++methodIndex) {
            const QJsonObject &result = representative[methodIndex];
            std::printf("  %-6s median=%.3f ms peak_rss=%lld KiB generation=%lld/%lld\n",
                        methodIndex == 0 ? "build" : "append",
                        ms(percentile(elapsed[methodIndex], 0.5)),
                        static_cast<long long>(*std::max_element(peakRss[methodIndex].begin(), peakRss[methodIndex].end())),
                        static_cast<long long>(result.value(QStringLiteral("generation")).toDouble()),
                        static_cast<long long>(result.value(QStringLiteral("generation_expected")).toDouble()));
            std::printf("         records cap=%lld growths=%lld storage_moves=%lld; masks cap=%lld growths=%lld storage_moves=%lld\n",
                        static_cast<long long>(result.value(QStringLiteral("record_capacity")).toDouble()),
                        static_cast<long long>(result.value(QStringLiteral("record_growths")).toDouble()),
                        static_cast<long long>(result.value(QStringLiteral("record_storage_moves")).toDouble()),
                        static_cast<long long>(result.value(QStringLiteral("mask_capacity")).toDouble()),
                        static_cast<long long>(result.value(QStringLiteral("mask_growths")).toDouble()),
                        static_cast<long long>(result.value(QStringLiteral("mask_storage_moves")).toDouble()));
            std::printf("         path segments cap=%lld offsets cap=%lld folders cap=%lld cancellation=%s\n",
                        static_cast<long long>(result.value(QStringLiteral("path_segment_capacity")).toDouble()),
                        static_cast<long long>(result.value(QStringLiteral("path_offset_capacity")).toDouble()),
                        static_cast<long long>(result.value(QStringLiteral("folder_capacity")).toDouble()),
                        result.value(QStringLiteral("cancellation_preserved")).toBool() ? "ok" : "failed");
        }
        std::printf("  record-order SHA-256: %s\n",
                    qPrintable(representative[0].value(QStringLiteral("record_order_sha256")).toString()));
        std::printf("  query-results SHA-256: %s\n",
                    qPrintable(representative[0].value(QStringLiteral("query_results_sha256")).toString()));
    }
    return 0;
}

double ms(qint64 ns) { return static_cast<double>(ns) / 1'000'000.0; }

qint64 percentile(QVector<qint64> values, double fraction)
{
    std::sort(values.begin(), values.end());
    return values[static_cast<qsizetype>((values.size() - 1) * fraction)];
}

void phase(const char *name, qint64 ns)
{
    std::printf("  %-20s %12.3f ms\n", name, ms(ns));
}

QString resolvedPath(const QString &path)
{
    QFileInfo info(path);
    return info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
}

bool livePath(const QString &path)
{
    const QString dataHome = qEnvironmentVariableIsSet("XDG_DATA_HOME")
        ? qEnvironmentVariable("XDG_DATA_HOME") : QDir::homePath() + QStringLiteral("/.local/share");
    const QString root = resolvedPath(dataHome + QStringLiteral("/muzaiten"));
    return path == root || path.startsWith(root + QDir::separator());
}

QStringList defaultQueries()
{
    return {QStringLiteral("the"), QStringLiteral("love"), QStringLiteral("beatles"),
            QStringLiteral("pink floyd"), QStringLiteral("artist:radiohead"),
            QString::fromUtf8("周杰伦"), QStringLiteral("jielun"), QStringLiteral("zhoujielun"),
            QString::fromUtf8("东京"), QStringLiteral("toukyou"), QString::fromUtf8("악단"),
            QStringLiteral("ak dan"), QString::fromUtf8("café"), QStringLiteral("sigur ros"),
            QStringLiteral("mozart piano"), QStringLiteral("love !live")};
}

QStringList prefixes(const QString &query)
{
    QStringList result;
    for (qsizetype end = 0; end < query.size();) {
        if (query[end].isHighSurrogate() && end + 1 < query.size() && query[end + 1].isLowSurrogate()) {
            end += 2;
        } else {
            ++end;
        }
        result.append(query.left(end));
    }
    return result;
}

void foldAgain(const Records &records)
{
    QElapsedTimer timer;
    timer.start();
    QHash<QString, QString> pool;
    for (const auto &record : records) {
        auto copy = record;
        Search::foldRecordNorms(copy, &pool);
    }
    phase("re-fold display fields", timer.nsecsElapsed());
}

Records scaledRecords(const Records &original, int scale)
{
    Records records;
    records.reserve(original.size() * scale);
    for (const auto &record : original) records.append(record);
    QElapsedTimer timer;
    timer.start();
    QHash<QString, QString> pool;
    for (int copy = 1; copy < scale; ++copy) {
        const QString suffix = QStringLiteral(" ~%1").arg(copy);
        for (const auto &source : original) {
            auto record = source;
            record.title += suffix;
            record.path += suffix;
            Search::foldRecordNorms(record, &pool);
            records.append(std::move(record));
        }
    }
    if (scale > 1) phase("scale and fold", timer.nsecsElapsed());
    return records;
}

void memory(const Records &records)
{
    QElapsedTimer timer;
    timer.start();
    QSet<const QChar *> seen;
    quint64 bytes = 0;
    quint64 units = 0;
    for (const auto &r : records) {
        for (const QString *s : {&r.normTitle, &r.normArtist, &r.normAlbumArtist,
                                 &r.normAlbum, &r.normFilename, &r.normPath}) {
            units += s->size();
            if (!s->isEmpty() && !seen.contains(s->constData())) {
                seen.insert(s->constData());
                bytes += static_cast<quint64>(s->size()) * sizeof(QChar);
            }
        }
    }
    phase("memory census", timer.nsecsElapsed());
    std::printf("  records=%lld unique norm bytes=%llu average norm length=%.2f UTF-16 units\n",
                static_cast<long long>(records.size()), static_cast<unsigned long long>(bytes),
                records.isEmpty() ? 0.0 : static_cast<double>(units) / (records.size() * 6));
}

bool queries(const Search::SearchIndex &index, const QStringList &querySet, int repeats,
             int modeChoice, QFile *dump, int scale, bool incremental)
{
    QVector<qint64> overall[2];
    std::printf("  %-22s %-6s %10s %10s %10s\n", "query", "mode", "p50 ms", "max ms", "matches");
    for (const QString &query : querySet) {
        const QStringList parts = prefixes(query);
        for (int mode = 0; mode < 2; ++mode) {
            if (modeChoice != 2 && modeChoice != mode) continue;
            Search::SearchQuery previousQuery;
            QVector<int> previousMatches;
            QVector<qint64> samples;
            int matches = 0;
            for (const QString &prefix : parts) {
                const auto parsed = Search::SearchQuery::parse(prefix);
                const bool narrow = incremental && Search::narrows(previousQuery, parsed);
                QVector<int> nextMatches;
                QVector<qint64> repeatsNs;
                for (int rep = 0; rep < repeats; ++rep) {
                    QElapsedTimer timer;
                    timer.start();
                    const auto results = index.match(parsed, mode == 1, {}, &matches,
                                                     incremental ? &nextMatches : nullptr,
                                                     narrow ? &previousMatches : nullptr);
                    repeatsNs.append(timer.nsecsElapsed());
                    if (dump && rep == 0) {
                        QJsonArray top;
                        for (const auto &result : results) {
                            QJsonArray entry;
                            entry.append(result.index);
                            entry.append(result.score);
                            top.append(entry);
                        }
                        QJsonObject row;
                        row.insert(QStringLiteral("query"), query);
                        row.insert(QStringLiteral("prefix"), prefix);
                        row.insert(QStringLiteral("mode"), mode ? QStringLiteral("fuzzy") : QStringLiteral("plain"));
                        row.insert(QStringLiteral("scale"), scale);
                        row.insert(QStringLiteral("matches"), matches);
                        row.insert(QStringLiteral("top"), top);
                        if (dump->write(QJsonDocument(row).toJson(QJsonDocument::Compact) + '\n') < 0) return false;
                    }
                }
                if (incremental) {
                    previousQuery = parsed;
                    previousMatches = std::move(nextMatches);
                }
                samples.append(percentile(repeatsNs, 0.5));
            }
            overall[mode] += samples;
            std::printf("  %-22s %-6s %10.3f %10.3f %10d\n", qPrintable(query),
                        mode ? "fuzzy" : "plain", ms(percentile(samples, 0.5)),
                        ms(*std::max_element(samples.begin(), samples.end())), matches);
        }
    }
    for (int mode = 0; mode < 2; ++mode) {
        if (modeChoice != 2 && modeChoice != mode) continue;
        const auto &v = overall[mode];
        std::printf("  overall %-6s p50=%.3f ms p95=%.3f ms max=%.3f ms (%lld prefixes)\n",
                    mode ? "fuzzy" : "plain", ms(percentile(v, 0.5)), ms(percentile(v, 0.95)),
                    ms(*std::max_element(v.begin(), v.end())), static_cast<long long>(v.size()));
    }
    return true;
}

bool runScale(const Records &original, int scale, const Search::CacheSignature &signature,
              const QString &cachePath, const QStringList &querySet, int repeats, int modeChoice,
              QFile *dump, bool incremental)
{
    std::printf("\nscale %d\n", scale);
    Records records = scaledRecords(original, scale);
    QElapsedTimer timer;
    timer.start();
    Search::SearchIndex index;
    index.build(records);
    phase("index build", timer.nsecsElapsed());

    timer.start();
    if (!Search::IndexCache::write(cachePath, signature, index.records())) return false;
    phase("cache write", timer.nsecsElapsed());
    std::printf("  cache size           %lld bytes\n", static_cast<long long>(QFileInfo(cachePath).size()));
    timer.start();
    auto loaded = Search::IndexCache::read(cachePath);
    const qint64 readNs = timer.nsecsElapsed();
    if (loaded.ok && loaded.records.size() == index.size()) {
        phase("cache read", readNs);
    } else {
        std::printf("  %-20s unavailable (reader rejected %lld records after %.3f ms)\n",
                    "cache read", static_cast<long long>(index.size()), ms(readNs));
    }
    loaded.records.clear();
    memory(index.records());
    return queries(index, querySet, repeats, modeChoice, dump, scale, incremental);
}

// Randomized check of the matcher's prefilter against the original matcher:
// membership agrees, the bound never undercuts the score, an exact bound is
// the score, and the masked scorer returns the original score.
int verifyBounds(const Records &records, int samples)
{
    std::mt19937_64 rng(20260927);
    const auto pick = [&](qsizetype n) {
        return std::uniform_int_distribution<qsizetype>(0, std::max<qsizetype>(0, n - 1))(rng);
    };
    const auto fieldOf = [&](const Search::SearchRecord &r) -> const QString & {
        switch (pick(6)) {
        case 0: return r.normTitle;
        case 1: return r.normArtist;
        case 2: return r.normAlbumArtist;
        case 3: return r.normAlbum;
        case 4: return r.normFilename;
        default: return r.normPath;
        }
    };
    const auto fragment = [&](const Search::SearchRecord &r) {
        const QStringList segments = fieldOf(r).split(Search::Fold::kSegmentSeparator);
        const QString text = segments[pick(segments.size())];
        if (text.isEmpty()) return QStringLiteral("a");
        const qsizetype start = pick(text.size());
        QString piece = text.mid(start, 1 + pick(8));
        if (pick(3) == 0 && piece.size() > 2) piece.remove(1 + pick(piece.size() - 2), 1);  // subsequence
        return piece.trimmed().isEmpty() ? QStringLiteral("e") : piece.trimmed();
    };
    static const char *prefixes[] = {"", "", "", "^", "'", "!", "artist:", "title:", "album:",
                                     "path:", "file:", "albumartist:"};
    long long checks = 0, failures = 0;
    for (int s = 0; s < samples; ++s) {
        const auto &rec = records[pick(records.size())];
        const auto &other = records[pick(records.size())];
        QString query = QString::fromLatin1(prefixes[pick(std::size(prefixes))])
            + fragment(pick(10) < 7 ? rec : other) + (pick(8) == 0 ? QStringLiteral("$") : QString());
        if (pick(4) == 0) query += QLatin1Char(' ') + fragment(pick(2) ? rec : other);
        const auto parsed = Search::SearchQuery::parse(query);
        const auto masks = Search::recordNormMasks(rec);
        const auto termMasks = Search::termCharacterMasks(parsed);
        for (const bool fuzzy : {false, true}) {
            ++checks;
            const int truth = Search::matchSearchRecord(rec, parsed, fuzzy);
            bool exact = true;
            const int bound = Search::matchSearchRecordBound(rec, masks, parsed, termMasks, fuzzy, &exact);
            const char *why = nullptr;
            if ((bound == INT_MIN) != (truth == INT_MIN)) why = "membership";
            else if (truth != INT_MIN && bound < truth) why = "bound below score";
            else if (truth != INT_MIN && exact && bound != truth) why = "exact bound differs";
            else if (truth != INT_MIN
                     && Search::scoreSearchRecord(rec, masks, parsed, termMasks, fuzzy) != truth) why = "masked score";
            if (why) {
                if (++failures <= 20) {
                    std::printf("FAIL %s: query [%s] %s truth=%d bound=%d exact=%d title=[%s]\n", why,
                                qPrintable(query), fuzzy ? "fuzzy" : "plain", truth, bound, exact,
                                qPrintable(rec.title));
                }
            }
        }
    }
    std::printf("verify-bounds: %lld checks, %lld failures\n", checks, failures);
    return failures ? 1 : 0;
}


// End-to-end check of SearchIndex::match against the original matcher run over
// every record and fully sorted: same total, same top results in order.
int verifyIndex(const Records &records, int samples)
{
    Search::SearchIndex index;
    index.build(records);
    std::mt19937_64 rng(7);
    const auto pick = [&](qsizetype n) {
        return std::uniform_int_distribution<qsizetype>(0, std::max<qsizetype>(0, n - 1))(rng);
    };
    static const char *prefixes[] = {"", "", "", "^", "'", "!", "artist:", "title:", "path:", "album:"};
    int failures = 0;
    int plainFailures = 0;
    int fuzzyFailures = 0;
    int fuzzyDrops = 0;
    int fuzzyGains = 0;
    int fuzzySameTotal = 0;
    int fuzzyTopDifferences = 0;
    int unexpectedFuzzy = 0;
    for (int s = 0; s < samples; ++s) {
        const auto &rec = records[pick(records.size())];
        const QString *fields[] = {&rec.normTitle, &rec.normArtist, &rec.normAlbum, &rec.normPath, &rec.normFilename};
        const QStringList segments = fields[pick(5)]->split(Search::Fold::kSegmentSeparator);
        const QString text = segments[pick(segments.size())];
        QString piece = text.mid(pick(std::max<qsizetype>(1, text.size())), 1 + pick(6)).trimmed();
        if (piece.isEmpty()) piece = QStringLiteral("a");
        QString query = QString::fromLatin1(prefixes[pick(std::size(prefixes))]) + piece;
        if (pick(5) == 0) query += QStringLiteral(" ") + QString(QChar(static_cast<char16_t>(u'a' + pick(26))));
        const auto parsed = Search::SearchQuery::parse(query);
        for (const bool fuzzy : {false, true}) {
            struct Hit { int score; int index; };
            std::vector<Hit> truth;
            for (int i = 0; i < records.size(); ++i) {
                const int score = Search::matchSearchRecord(records[i], parsed, fuzzy);
                if (score != INT_MIN) truth.push_back({score, i});
            }
            std::sort(truth.begin(), truth.end(), [](const Hit &a, const Hit &b) {
                return a.score != b.score ? a.score > b.score : a.index < b.index;
            });
            int total = 0;
            const auto results = index.match(parsed, fuzzy, {}, &total);
            bool sameTop = results.size() == static_cast<qsizetype>(
                std::min<std::size_t>(truth.size(), Search::kMaxResults));
            for (qsizetype r = 0; sameTop && r < results.size(); ++r) {
                sameTop = results[r].index == truth[static_cast<std::size_t>(r)].index
                    && results[r].score == truth[static_cast<std::size_t>(r)].score;
            }
            const bool same = total == static_cast<int>(truth.size()) && sameTop;
            if (!same) {
                ++failures;
                if (fuzzy) {
                    ++fuzzyFailures;
                    if (total < static_cast<int>(truth.size())) ++fuzzyDrops;
                    else if (total > static_cast<int>(truth.size())) ++fuzzyGains;
                    else ++fuzzySameTotal;
                    if (!sameTop) ++fuzzyTopDifferences;
                    if (std::none_of(parsed.terms.begin(), parsed.terms.end(), [](const Search::Term &term) {
                            return term.kind == Search::TermKind::FreeText && !term.forceExact;
                        })) ++unexpectedFuzzy;
                } else {
                    ++plainFailures;
                }
            }
            if (!same && failures <= 20) {
                std::printf("FAIL query [%s] %s: total %d vs %zu\n", qPrintable(query),
                            fuzzy ? "fuzzy" : "plain", total, truth.size());
            }
        }
    }
    std::printf("verify-index: %d queries x 2 modes, %d failures (plain %d, fuzzy %d)\n",
                samples, failures, plainFailures, fuzzyFailures);
    std::printf("fuzzy differences: %d total drops, %d gains, %d unchanged totals, "
                "%d different top lists, %d without a fuzzy free-text term\n",
                fuzzyDrops, fuzzyGains, fuzzySameTotal, fuzzyTopDifferences, unexpectedFuzzy);
    return failures ? 1 : 0;
}



} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--append-capacity")) {
        return appendCapacityBenchmark();
    }
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--append-capacity-worker")) {
        bool ok = false;
        const int records = QString::fromLocal8Bit(argv[3]).toInt(&ok);
        if (!ok) return 2;
        return appendCapacityWorker(QString::fromLocal8Bit(argv[2]), records);
    }
    if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--help")) {
        std::printf("usage: bench_search --library PATH [--scale N] [--queries FILE] [--repeat R]\n"
                    "       bench_search --append-capacity\n");
        return 0;
    }
    QString library;
    QString queriesFile;
    int scale = 1;
    int repeats = 5;
    int modeChoice = 2;
    int verifySamples = 0;
    int verifyIndexSamples = 0;
    QString dumpPath;
    bool incremental = false;
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--compare")) {
        QFile before(QString::fromLocal8Bit(argv[2])), after(QString::fromLocal8Bit(argv[3]));
        if (!before.open(QIODevice::ReadOnly) || !after.open(QIODevice::ReadOnly)) return 2;
        int line = 1;
        while (!before.atEnd() && !after.atEnd()) {
            if (before.readLine() != after.readLine()) { std::fprintf(stderr, "dump differs at line %d\n", line); return 1; }
            ++line;
        }
        if (!before.atEnd() || !after.atEnd()) { std::fprintf(stderr, "dump length differs at line %d\n", line); return 1; }
        std::printf("dumps equal (%d rows)\n", line - 1);
        return 0;
    }
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QStringLiteral("--incremental")) { incremental = true; continue; }
        if ((arg == QStringLiteral("--library") || arg == QStringLiteral("--scale")
             || arg == QStringLiteral("--queries") || arg == QStringLiteral("--repeat")
             || arg == QStringLiteral("--dump") || arg == QStringLiteral("--mode")
             || arg == QStringLiteral("--verify-bounds") || arg == QStringLiteral("--verify-index")) && i + 1 < argc) {
            const QString value = QString::fromLocal8Bit(argv[++i]);
            if (arg == QStringLiteral("--library")) library = value;
            else if (arg == QStringLiteral("--queries")) queriesFile = value;
            else if (arg == QStringLiteral("--dump")) dumpPath = value;
            else if (arg == QStringLiteral("--mode")) {
                if (value == QStringLiteral("plain")) modeChoice = 0;
                else if (value == QStringLiteral("fuzzy")) modeChoice = 1;
                else if (value == QStringLiteral("both")) modeChoice = 2;
                else return 2;
            }
            else {
                bool ok = false;
                const int number = value.toInt(&ok);
                if (!ok || number < 1 || (arg == QStringLiteral("--scale") && number > 100)) {
                    std::fprintf(stderr, "invalid %s: %s\n", qPrintable(arg), qPrintable(value));
                    return 2;
                }
                if (arg == QStringLiteral("--scale")) scale = number;
                else if (arg == QStringLiteral("--verify-bounds")) verifySamples = number;
                else if (arg == QStringLiteral("--verify-index")) verifyIndexSamples = number;
                else repeats = number;
            }
        } else {
            std::fprintf(stderr, "usage: bench_search --library PATH [--scale N] [--queries FILE] [--repeat R]\n"
                                 "       bench_search --append-capacity\n");
            return 2;
        }
    }
    if (library.isEmpty() || !QFileInfo::exists(library)) {
        std::fprintf(stderr, "--library must name an existing database copy\n");
        return 2;
    }
    library = resolvedPath(library);
    if (livePath(library)) {
        std::fprintf(stderr, "refusing library inside the live muzaiten data directory: %s\n", qPrintable(library));
        return 2;
    }
    QStringList querySet = defaultQueries();
    if (!queriesFile.isEmpty()) {
        QFile file(queriesFile);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            std::fprintf(stderr, "cannot read queries file: %s\n", qPrintable(queriesFile));
            return 2;
        }
        querySet.clear();
        QTextStream stream(&file);
        while (!stream.atEnd()) {
            const QString line = stream.readLine();
            if (!line.isEmpty()) querySet.append(line);
        }
        if (querySet.isEmpty()) return 2;
    }
    if (!QDir().mkpath(QStringLiteral("agent-state"))) {
        std::fprintf(stderr, "cannot create private state directory\n");
        return 2;
    }
    QTemporaryDir temporary(QDir::currentPath() + QStringLiteral("/agent-state/bench-search-XXXXXX"));
    if (!temporary.isValid()) {
        std::fprintf(stderr, "cannot create private temporary directory\n");
        return 2;
    }
    const QString privateLibrary = temporary.filePath(QStringLiteral("library.sqlite"));
    if (!QFile::copy(library, privateLibrary)) {
        std::fprintf(stderr, "cannot copy library into private temporary directory\n");
        return 2;
    }
    std::printf("source: %s\nDatabase::open enables WAL and migrations; opening a private copy: %s\n",
                qPrintable(library), qPrintable(privateLibrary));
    Database db(QStringLiteral("bench-search"));
    if (!db.open(privateLibrary)) {
        std::fprintf(stderr, "database open failed: %s\n", qPrintable(db.lastError()));
        return 1;
    }
    QElapsedTimer timer;
    timer.start();
    const Records records = db.allTracksForSearch();
    std::printf("phases (QElapsedTimer nanoseconds, shown in milliseconds)\n");
    phase("load and fold", timer.nsecsElapsed());
    if (verifySamples) return verifyBounds(records, verifySamples);
    if (verifyIndexSamples) return verifyIndex(records, verifyIndexSamples);
    foldAgain(records);
    const auto signature = Search::IndexCache::currentSignature(db);
    if (!signature.valid()) return 1;
    QFile dump(dumpPath);
    if (!dumpPath.isEmpty() && !dump.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 2;
    QFile *dumpOutput = dumpPath.isEmpty() ? nullptr : &dump;
    if (!runScale(records, 1, signature, temporary.filePath(QStringLiteral("search.cache")), querySet, repeats, modeChoice, dumpOutput, incremental)) {
        std::fprintf(stderr, "cache write/read failed at scale 1\n");
        return 1;
    }
    if (scale > 1 && !runScale(records, scale, signature, temporary.filePath(QStringLiteral("search.cache")), querySet, repeats, modeChoice, dumpOutput, incremental)) {
        std::fprintf(stderr, "cache write/read failed at scale %d\n", scale);
        return 1;
    }
    return 0;
}
