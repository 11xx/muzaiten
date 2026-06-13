#pragma once

// Background worker that owns the search index and runs queries off the GUI
// thread.  Lives on its own QThread (same pattern as MpdImportWorker).
//
// Thread affinity: all slots execute on the worker thread (via queued
// connections).  All signals are queued back to the GUI thread.

#include "search/Exclusion.h"
#include "search/SearchIndex.h"
#include "search/SearchQuery.h"

#include <QHash>
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
    // Rebuild the index from the database. Builds the cheap "basic" fold first
    // and emits indexReady() so queries work immediately, then upgrades records
    // to the full romaji fold in the background (emitting indexUpgraded()).
    void buildIndex();

    // Internal: fold one chunk of records to the extended tier, then re-post
    // itself until the index is fully upgraded. `generation` guards against a
    // rebuild/clear superseding an in-flight upgrade.
    void upgradeChunk(quint64 generation);

    // Run a query against the current index.  Emits resultsReady() with the
    // results if queryId matches the latest submitted query (stale results from
    // superseded queries are silently dropped).
    void runQuery(quint64 queryId, const QString &queryString, bool fuzzyMode);

    // Release the in-memory index, freeing memory. The next buildIndex()
    // reloads it from the database.
    void clearIndex();

    // Update the exclusion rules; compiled once and applied to every query.
    void setExclusions(QVector<Search::ExcludeRule> rules);

signals:
    void indexReady(int trackCount);          // basic fold ready — queries enabled
    void indexUpgraded();                      // full romaji fold complete
    void indexError(const QString &error);
    void resultsReady(quint64 queryId, QVector<Search::ScoredResult> results, int totalMatches);

private:
    QString   m_dbPath;
    SearchIndex m_index;
    ExclusionSet m_excludes;
    std::atomic<quint64> m_latestQueryId{0};
    Database *m_db = nullptr;  // opened on the worker thread in buildIndex()

    // Background upgrade (basic → extended fold) state. m_buildGeneration bumps
    // on every build/clear so stale chunk events abort.
    quint64 m_buildGeneration = 0;
    int m_upgradeCursor = 0;
    QHash<QString, QString> m_upgradePool;
};

} // namespace Search
