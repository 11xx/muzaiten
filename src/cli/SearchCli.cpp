#include "cli/SearchCli.h"

#include "app/AppPaths.h"
#include "db/Database.h"
#include "search/IndexCache.h"
#include "search/SearchRecord.h"

#include <QDir>
#include <QFile>
#include <QUuid>

namespace SearchCli {

QString libraryDbPath()
{
    return QDir(AppPaths::dataDir()).filePath(QStringLiteral("library.sqlite"));
}

QString cachePath()
{
    return Search::IndexCache::defaultPath();
}

bool clearCache(QString *path)
{
    const QString file = cachePath();
    if (path) {
        *path = file;
    }
    return QFile::exists(file) && QFile::remove(file);
}

namespace {

// Drain the streaming cursor synchronously into one vector — the CLI has no
// event loop to interleave with, so a straight read is simplest.
QVector<Search::SearchRecord> readAll(const Database &db)
{
    QVector<Search::SearchRecord> all;
    auto cursor = db.beginTrackSearchStream();
    if (!cursor) {
        return all;
    }
    QVector<Search::SearchRecord> batch;
    while (cursor->nextBatch(4096, batch)) {
        all.reserve(all.size() + batch.size());
        for (Search::SearchRecord &rec : batch) {
            all.push_back(std::move(rec));
        }
    }
    return all;
}

} // namespace

LoadResult loadIndex(Search::SearchIndex &index, bool forceRefresh)
{
    LoadResult result;

    const QString dbPath = libraryDbPath();
    if (!QFile::exists(dbPath)) {
        result.error = QStringLiteral("library database not found at %1").arg(dbPath);
        return result;
    }

    Database db(QStringLiteral("muzaitenctl-search-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!db.open(dbPath)) {
        result.error = db.lastError();
        return result;
    }

    const Search::CacheSignature current = Search::IndexCache::currentSignature(db);

    if (!forceRefresh) {
        Search::IndexCache::Loaded cached = Search::IndexCache::read(cachePath());
        if (cached.ok) {
            result.usedCache = true;
            result.wasStale = !(cached.signature == current);
            index.build(std::move(cached.records));
            result.ok = true;
            result.trackCount = index.size();
            return result;
        }
    }

    // Missing/corrupt cache, or an explicit refresh: build from the DB and write
    // the cache back so the next run is fast.
    QVector<Search::SearchRecord> records = readAll(db);
    result.trackCount = static_cast<int>(records.size());
    index.build(std::move(records));
    Search::IndexCache::write(cachePath(), current, index.records());
    result.ok = true;
    result.rebuilt = true;
    return result;
}

LoadResult streamRecords(const std::function<void(const Search::SearchRecord &)> &sink, bool forceRefresh)
{
    LoadResult result;

    // Cache fast-path FIRST: stream straight from the cache without opening the
    // DB or computing the staleness signature. The picker shows cached data
    // regardless of staleness (a rebuild is --refresh), and the signature does a
    // cold full-table scan — doing it up front delayed the first rows by seconds.
    if (!forceRefresh) {
        const bool ok = Search::IndexCache::forEachRecord(
            cachePath(), nullptr, [&sink](Search::SearchRecord rec) { sink(rec); });
        if (ok) {
            result.ok = true;
            result.usedCache = true;
            return result;
        }
    }

    // Miss or forced refresh: now we need the DB to (re)build and rewrite cache.
    const QString dbPath = libraryDbPath();
    if (!QFile::exists(dbPath)) {
        result.error = QStringLiteral("library database not found at %1").arg(dbPath);
        return result;
    }
    Database db(QStringLiteral("muzaitenctl-stream-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!db.open(dbPath)) {
        result.error = db.lastError();
        return result;
    }
    const Search::CacheSignature current = Search::IndexCache::currentSignature(db);

    // Stream the DB build to the sink as batches arrive, accumulating so we can
    // write the cache for next time.
    QVector<Search::SearchRecord> all;
    if (auto cursor = db.beginTrackSearchStream()) {
        QVector<Search::SearchRecord> batch;
        while (cursor->nextBatch(4096, batch)) {
            all.reserve(all.size() + batch.size());
            for (Search::SearchRecord &rec : batch) {
                sink(rec);
                all.push_back(std::move(rec));
            }
        }
    }
    Search::IndexCache::write(cachePath(), current, all);
    result.ok = true;
    result.rebuilt = true;
    result.trackCount = static_cast<int>(all.size());
    return result;
}

} // namespace SearchCli
