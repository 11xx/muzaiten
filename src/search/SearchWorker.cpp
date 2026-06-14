#include "search/SearchWorker.h"

#include "db/Database.h"
#include "search/SearchQuery.h"
#include "search/SearchRecord.h"

#include <QUuid>

namespace Search {

SearchWorker::SearchWorker(const QString &dbPath, QObject *parent)
    : QObject(parent)
    , m_dbPath(dbPath)
{
}

SearchWorker::~SearchWorker()
{
    delete m_db;
}

void SearchWorker::buildIndex()
{
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

    const CacheSignature current = IndexCache::currentSignature(*m_db);
    IndexCache::Loaded cached = IndexCache::read(IndexCache::defaultPath());

    if (cached.ok && cached.signature == current) {
        // Warm + fresh: load the cache and we're done — no DB read, no fold.
        ++m_buildGeneration; // cancel any in-flight stream
        m_cursor.reset();
        m_index.build(std::move(cached.records));
        emit indexLoaded(m_index.size());
        return;
    }

    // Either show the (stale but usable) cache immediately and refresh quietly
    // in the background, or — with no usable cache — stream a cold build into
    // the live index so results appear as the data loads.
    const bool haveStaleCache = cached.ok;
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
    m_cursor.reset();
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
        IndexCache::write(IndexCache::defaultPath(), m_pendingSignature, m_index.records());
    }
}

void SearchWorker::clearIndex()
{
    ++m_buildGeneration; // abort any in-flight stream
    m_cursor.reset();
    m_staging.clear();
    m_index.clear();
}

void SearchWorker::setExclusions(QVector<Search::ExcludeRule> rules)
{
    m_excludes = compileExcludes(rules);
}

void SearchWorker::runQuery(quint64 queryId, const QString &queryString, bool fuzzyMode)
{
    // Record the latest query id; if a newer one arrives while we're computing,
    // we don't need to emit results for this one.
    m_latestQueryId.store(queryId);

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
