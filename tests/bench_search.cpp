// Offline search latency probe. Built by default, deliberately absent from CTest.
// Usage: bench_search --library PATH [--scale N] [--queries FILE] [--repeat R]

#include "db/Database.h"
#include "search/IndexCache.h"
#include "search/SearchIndex.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <QTextStream>

#include <algorithm>
#include <cstdio>

namespace {

using Records = QVector<Search::SearchRecord>;

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

void queries(const Search::SearchIndex &index, const QStringList &querySet, int repeats)
{
    QVector<qint64> overall[2];
    std::printf("  %-22s %-6s %10s %10s %10s\n", "query", "mode", "p50 ms", "max ms", "matches");
    for (const QString &query : querySet) {
        const QStringList parts = prefixes(query);
        for (int mode = 0; mode < 2; ++mode) {
            QVector<qint64> samples;
            int matches = 0;
            for (const QString &prefix : parts) {
                QVector<qint64> repeatsNs;
                for (int rep = 0; rep < repeats; ++rep) {
                    QElapsedTimer timer;
                    timer.start();
                    const auto parsed = Search::SearchQuery::parse(prefix);
                    (void)index.match(parsed, mode == 1, {}, &matches);
                    repeatsNs.append(timer.nsecsElapsed());
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
        const auto &v = overall[mode];
        std::printf("  overall %-6s p50=%.3f ms p95=%.3f ms max=%.3f ms (%lld prefixes)\n",
                    mode ? "fuzzy" : "plain", ms(percentile(v, 0.5)), ms(percentile(v, 0.95)),
                    ms(*std::max_element(v.begin(), v.end())), static_cast<long long>(v.size()));
    }
}

bool runScale(const Records &original, int scale, const Search::CacheSignature &signature,
              const QString &cachePath, const QStringList &querySet, int repeats)
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
    queries(index, querySet, repeats);
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QString library;
    QString queriesFile;
    int scale = 1;
    int repeats = 5;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if ((arg == QStringLiteral("--library") || arg == QStringLiteral("--scale")
             || arg == QStringLiteral("--queries") || arg == QStringLiteral("--repeat")) && i + 1 < argc) {
            const QString value = QString::fromLocal8Bit(argv[++i]);
            if (arg == QStringLiteral("--library")) library = value;
            else if (arg == QStringLiteral("--queries")) queriesFile = value;
            else {
                bool ok = false;
                const int number = value.toInt(&ok);
                if (!ok || number < 1 || (arg == QStringLiteral("--scale") && number > 100)) {
                    std::fprintf(stderr, "invalid %s: %s\n", qPrintable(arg), qPrintable(value));
                    return 2;
                }
                if (arg == QStringLiteral("--scale")) scale = number; else repeats = number;
            }
        } else {
            std::fprintf(stderr, "usage: bench_search --library PATH [--scale N] [--queries FILE] [--repeat R]\n");
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
    foldAgain(records);
    const auto signature = Search::IndexCache::currentSignature(db);
    if (!signature.valid()) return 1;
    if (!runScale(records, 1, signature, temporary.filePath(QStringLiteral("search.cache")), querySet, repeats)) {
        std::fprintf(stderr, "cache write/read failed at scale 1\n");
        return 1;
    }
    if (scale > 1 && !runScale(records, scale, signature, temporary.filePath(QStringLiteral("search.cache")), querySet, repeats)) {
        std::fprintf(stderr, "cache write/read failed at scale %d\n", scale);
        return 1;
    }
    return 0;
}
