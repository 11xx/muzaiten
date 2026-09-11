#include "cli/SearchCli.h"

#include "app/AppPaths.h"
#include "db/Database.h"
#include "search/IndexCache.h"
#include "search/SearchRecord.h"

#include <QDir>
#include <QFile>
#include <QUuid>
#include <QScopeGuard>

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
QVector<Search::SearchRecord> readAll(const Database &db, QString *error)
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
    if (!cursor->lastError().isEmpty()) {
        *error = cursor->lastError();
        return {};
    }
    return all;
}

} // namespace

LoadResult loadIndex(Search::SearchIndex &index, bool forceRefresh)
{
    LoadResult result;
    const QString path = libraryDbPath();
    if (!QFile::exists(path)) {
        result.error = QStringLiteral("library database not found at %1").arg(path);
        return result;
    }
    Database db(QStringLiteral("search-load-") + QUuid::createUuid().toString());
    if (!db.open(path) || !db.beginTransaction()) {
        result.error = db.lastError();
        return result;
    }
    const auto rollback = qScopeGuard([&db] { db.rollbackTransaction(); });
    const auto current = Search::IndexCache::currentSignature(db);
    if (!current.valid()) {
        result.error = QStringLiteral("Search content revision is unavailable");
        return result;
    }
    result.sourceRevision = current.contentRevision;
    result.cacheReason = QStringLiteral("forced-refresh");
    if (!forceRefresh) {
        auto cached = Search::IndexCache::read(cachePath());
        result.cacheReason = cached.ok ? Search::IndexCache::mismatchReason(cached.signature, current)
            : QFile::exists(cachePath()) ? QStringLiteral("invalid-cache") : QStringLiteral("missing-cache");
        if (cached.ok && cached.signature == current) {
            result.usedCache = true;
            index.build(std::move(cached.records));
            result.ok = true;
            result.trackCount = index.size();
            return result;
        }
        result.wasStale = cached.ok;
    }
    auto records = readAll(db, &result.error);
    if (!result.error.isEmpty()) return result;
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
    const QString path = libraryDbPath();
    if (!QFile::exists(path)) {
        result.error = QStringLiteral("library database not found at %1").arg(path);
        return result;
    }
    Database db(QStringLiteral("search-stream-") + QUuid::createUuid().toString());
    if (!db.open(path) || !db.beginTransaction()) {
        result.error = db.lastError();
        return result;
    }
    const auto rollback = qScopeGuard([&db] { db.rollbackTransaction(); });
    const auto current = Search::IndexCache::currentSignature(db);
    if (!current.valid()) {
        result.error = QStringLiteral("Search content revision is unavailable");
        return result;
    }
    result.sourceRevision = current.contentRevision;
    result.cacheReason = QStringLiteral("forced-refresh");
    if (!forceRefresh) {
        Search::CacheSignature cached;
        const bool ok = Search::IndexCache::forEachRecord(cachePath(), &cached,
            [&](Search::SearchRecord record) {
                ++result.trackCount;
                sink(record);
            }, &current);
        if (ok) {
            result.ok = true;
            result.usedCache = true;
            result.cacheReason = QStringLiteral("hit");
            return result;
        }
        if (result.trackCount > 0) {
            result.error = QStringLiteral("Search cache is corrupt; retry with --refresh");
            return result;
        }
        result.wasStale = cached.valid() && cached != current;
        result.cacheReason = result.wasStale ? Search::IndexCache::mismatchReason(cached, current)
            : QFile::exists(cachePath()) ? QStringLiteral("invalid-cache") : QStringLiteral("missing-cache");
    }
    QVector<Search::SearchRecord> all;
    auto cursor = db.beginTrackSearchStream();
    QVector<Search::SearchRecord> batch;
    while (cursor->nextBatch(4096, batch)) {
        all.reserve(all.size() + batch.size());
        for (auto &record : batch) {
            sink(record);
            all.push_back(std::move(record));
        }
    }
    if (!cursor->lastError().isEmpty()) {
        result.error = cursor->lastError();
        return result;
    }
    Search::IndexCache::write(cachePath(), current, all);
    result.ok = true;
    result.rebuilt = true;
    result.trackCount = static_cast<int>(all.size());
    return result;
}

} // namespace SearchCli
