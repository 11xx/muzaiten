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

    QVector<SearchRecord> records = m_db->allTracksForSearch();
    const QVector<SearchRecord> mpdRecords = m_db->allMpdTracksForSearch();
    records.reserve(records.size() + mpdRecords.size());
    for (const auto &r : mpdRecords) {
        records.append(r);
    }

    m_index.build(std::move(records));
    emit indexReady(m_index.size());
}

void SearchWorker::clearIndex()
{
    m_index.clear();
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
    QVector<ScoredResult> results = m_index.match(q, fuzzyMode, &totalMatches);

    // Only emit if this is still the most recent query
    if (m_latestQueryId.load() == queryId) {
        emit resultsReady(queryId, std::move(results), totalMatches);
    }
}

} // namespace Search
