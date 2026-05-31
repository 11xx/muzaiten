#pragma once

// Background worker that owns the search index and runs queries off the GUI
// thread.  Lives on its own QThread (same pattern as MpdImportWorker).
//
// Thread affinity: all slots execute on the worker thread (via queued
// connections).  All signals are queued back to the GUI thread.

#include "search/SearchIndex.h"
#include "search/SearchQuery.h"

#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>

class Database;

namespace Search {

class SearchWorker : public QObject {
    Q_OBJECT
public:
    explicit SearchWorker(const QString &dbPath, QObject *parent = nullptr);
    ~SearchWorker() override;

public slots:
    // Rebuild the entire index from the database.  Emits indexReady() when done.
    void buildIndex();

    // Run a query against the current index.  Emits resultsReady() with the
    // results if queryId matches the latest submitted query (stale results from
    // superseded queries are silently dropped).
    void runQuery(quint64 queryId, const QString &queryString, bool fuzzyMode);

    // Release the in-memory index, freeing memory. The next buildIndex()
    // reloads it from the database.
    void clearIndex();

signals:
    void indexReady(int trackCount);
    void indexError(const QString &error);
    void resultsReady(quint64 queryId, QVector<Search::ScoredResult> results);

private:
    QString   m_dbPath;
    SearchIndex m_index;
    std::atomic<quint64> m_latestQueryId{0};
    Database *m_db = nullptr;  // opened on the worker thread in buildIndex()
};

} // namespace Search
