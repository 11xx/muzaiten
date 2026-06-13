#include "search/SearchWorker.h"

#include "db/Database.h"
#include "search/SearchQuery.h"
#include "search/SearchRecord.h"

#include <QUuid>

#include <algorithm>

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

    // Basic tier: cheap fold (diacritics/scripts only) so the index is queryable
    // almost immediately. The raw sort names ride along for the upgrade pass.
    QVector<SearchRecord> records = m_db->allTracksForSearch(/*extended=*/false);
    const QVector<SearchRecord> mpdRecords = m_db->allMpdTracksForSearch(/*extended=*/false);
    records.reserve(records.size() + mpdRecords.size());
    for (const auto &r : mpdRecords) {
        records.append(r);
    }

    m_index.build(std::move(records));
    const quint64 generation = ++m_buildGeneration;
    emit indexReady(m_index.size());

    // Extended tier: upgrade to the full romaji fold in the background, chunked
    // so queued queries interleave and stay responsive during the load.
    m_upgradeCursor = 0;
    m_upgradePool.clear();
    QMetaObject::invokeMethod(this, "upgradeChunk", Qt::QueuedConnection, Q_ARG(quint64, generation));
}

void SearchWorker::upgradeChunk(quint64 generation)
{
    if (generation != m_buildGeneration) {
        return; // superseded by a newer build/clear
    }
    constexpr int kChunk = 3000; // ~tens of ms per chunk; bounds query latency
    const int total = m_index.size();
    const int lo = m_upgradeCursor;
    const int hi = std::min(lo + kChunk, total);
    m_index.upgradeFold(lo, hi, m_upgradePool);
    m_upgradeCursor = hi;

    if (hi < total) {
        QMetaObject::invokeMethod(this, "upgradeChunk", Qt::QueuedConnection, Q_ARG(quint64, generation));
    } else {
        m_upgradePool.clear();
        emit indexUpgraded();
    }
}

void SearchWorker::clearIndex()
{
    ++m_buildGeneration; // abort any in-flight upgrade
    m_upgradePool.clear();
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
