#include "search/SearchWorker.h"

#include "db/Database.h"
#include "search/SearchQuery.h"
#include "search/SearchRecord.h"

#include <QUuid>
#include <QFileInfo>

namespace Search {

SearchWorker::SearchWorker(const QString &dbPath, QObject *parent)
    : QObject(parent)
    , m_dbPath(dbPath)
{
}

SearchWorker::~SearchWorker()
{
    closeSnapshot();
    delete m_db;
}

void SearchWorker::closeSnapshot()
{
    m_cursor.reset();
    if (m_snapshotOpen) {
        m_db->rollbackTransaction();
        m_snapshotOpen = false;
    }
}

void SearchWorker::buildIndex()
{
    startBuild(false);
}

void SearchWorker::rebuildIndex()
{
    startBuild(true);
}

void SearchWorker::startBuild(bool forceRefresh)
{
    ++m_buildGeneration;
    closeSnapshot();
    m_staging.clear();
    // Open (or reuse) a DB connection on this worker thread.
    if (!m_db) {
        const QString connName = QStringLiteral("search-worker-%1")
                                     .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        m_db = new Database(connName);
        if (!m_db->open(m_dbPath)) {
            const QString err = m_db->lastError();
            delete m_db;
            m_db = nullptr;
            emit indexError(err);
            return;
        }
    }

    if (!m_db->beginTransaction()) {
        emit indexError(m_db->lastError());
        return;
    }
    m_snapshotOpen = true;
    const CacheSignature current = IndexCache::currentSignature(*m_db);
    if (!current.valid()) {
        closeSnapshot();
        emit indexError(QStringLiteral("Search content revision is unavailable"));
        return;
    }
    IndexCache::Loaded cached = IndexCache::read(IndexCache::defaultPath());
    const QString reason = forceRefresh ? QStringLiteral("forced-refresh") : cached.ok
        ? IndexCache::mismatchReason(cached.signature, current)
        : QFileInfo::exists(IndexCache::defaultPath()) ? QStringLiteral("invalid-cache") : QStringLiteral("missing-cache");
    emit cacheDecision(reason, current.contentRevision);

    if (!forceRefresh && cached.ok && cached.signature == current) {
        // Warm + fresh: load the cache and we're done — no DB read, no fold.
        closeSnapshot();
        m_index.build(std::move(cached.records));
        emit indexLoaded(m_index.size());
        if (IndexCache::currentSignature(*m_db) != current) {
            const auto generation = m_buildGeneration;
            QMetaObject::invokeMethod(this, [this, generation] {
                if (m_buildGeneration == generation) buildIndex();
            }, Qt::QueuedConnection);
        }
        return;
    }

    // Either show the (stale but usable) cache immediately and refresh quietly
    // in the background, or — with no usable cache — stream a cold build into
    // the live index so results appear as the data loads.
    const bool haveStaleCache = cached.ok && cached.signature.databaseId == current.databaseId
        && cached.signature.databasePath == current.databasePath && cached.signature.foldVersion == current.foldVersion
        && cached.signature.schemaVersion == current.schemaVersion;
    if (haveStaleCache) {
        m_index.build(std::move(cached.records));
        emit indexLoaded(m_index.size());
        m_buildMode = BuildMode::Background;
        m_staging.clear();
        emit indexRefreshing();
    } else {
        m_index.clear();
        m_buildMode = BuildMode::Foreground;
    }

    m_pendingSignature = current;
    m_cursor = m_db->beginTrackSearchStream();
    const quint64 generation = ++m_buildGeneration;
    QMetaObject::invokeMethod(this, "readChunk", Qt::QueuedConnection, Q_ARG(quint64, generation));
}

void SearchWorker::readChunk(quint64 generation)
{
    if (generation != m_buildGeneration || !m_cursor) {
        return; // superseded by a newer build/clear
    }
    constexpr int kChunk = 3000; // bounds per-batch work so queries stay snappy
    QVector<SearchRecord> batch;
    const bool more = m_cursor->nextBatch(kChunk, batch);
    if (!m_cursor->lastError().isEmpty()) {
        const QString error = m_cursor->lastError();
        closeSnapshot();
        m_staging.clear();
        emit indexError(error);
        return;
    }
    if (!batch.isEmpty()) {
        if (m_buildMode == BuildMode::Foreground) {
            m_index.append(std::move(batch));
            emit indexGrew(m_index.size());
        } else {
            m_staging.append(std::move(batch)); // silent until the swap
        }
    }
    if (more) {
        QMetaObject::invokeMethod(this, "readChunk", Qt::QueuedConnection, Q_ARG(quint64, generation));
    } else {
        finishBuild(generation);
    }
}

void SearchWorker::finishBuild(quint64 generation)
{
    closeSnapshot();
    if (m_buildMode == BuildMode::Background) {
        m_index = std::move(m_staging); // atomic swap (worker is single-threaded)
        m_staging.clear();
        emit indexRefreshed(m_index.size());
    } else {
        emit indexLoaded(m_index.size());
    }
    // Seed/refresh the on-disk cache (best-effort; a failure just means the next
    // start rebuilds). Generation-guarded so a superseded build doesn't write.
    if (generation == m_buildGeneration) {
        const auto live = IndexCache::currentSignature(*m_db);
        if (!live.valid()) {
            emit indexError(QStringLiteral("Search content revision is unavailable"));
        } else if (live == m_pendingSignature) {
            IndexCache::write(IndexCache::defaultPath(), m_pendingSignature, m_index.records());
        } else {
            QMetaObject::invokeMethod(this, [this, generation] {
                if (m_buildGeneration == generation) buildIndex();
            }, Qt::QueuedConnection);
        }
    }
}

void SearchWorker::clearIndex()
{
    ++m_buildGeneration; // abort any in-flight stream
    closeSnapshot();
    m_staging.clear();
    m_index.clear();
}

void SearchWorker::setExclusions(QVector<Search::ExcludeRule> rules)
{
    m_excludes = compileExcludes(rules);
}

void SearchWorker::submitQuery(quint64 queryId, const QString &queryString, bool fuzzyMode)
{
    m_latestQueryId.store(queryId);
    QMetaObject::invokeMethod(this, [this, queryId, queryString, fuzzyMode]() {
        if (m_latestQueryId.load() == queryId) runQuery(queryId, queryString, fuzzyMode);
    }, Qt::QueuedConnection);
}

void SearchWorker::runQuery(quint64 queryId, const QString &queryString, bool fuzzyMode)
{
    if (m_index.isEmpty()) {
        emit resultsReady(queryId, {}, 0);
        return;
    }

    const SearchQuery q = SearchQuery::parse(queryString);
    int totalMatches = 0;
    QVector<ScoredResult> results = m_index.match(q, fuzzyMode, m_excludes, &totalMatches);

    // Only emit if this is still the most recent query
    if (m_latestQueryId.load() == queryId) {
        emit resultsReady(queryId, std::move(results), totalMatches);
    }
}

} // namespace Search
