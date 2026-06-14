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

} // namespace SearchCli
