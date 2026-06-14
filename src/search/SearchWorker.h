#pragma once

// Background worker that owns the search index and runs queries off the GUI
// thread.  Lives on its own QThread (same pattern as MpdImportWorker).
//
// Thread affinity: all slots execute on the worker thread (via queued
// connections).  All signals are queued back to the GUI thread.

#include "search/Exclusion.h"
#include "search/SearchIndex.h"
#include "search/SearchQuery.h"

#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include <memory>

class Database;
class TrackSearchCursor;

namespace Search {

class SearchWorker : public QObject {
    Q_OBJECT
public:
    explicit SearchWorker(const QString &dbPath, QObject *parent = nullptr);
    ~SearchWorker() override;

public slots:
    // Rebuild the index from the database. Opens a streaming cursor and pumps
    // it in chunks (see readChunk), so the index fills incrementally and queries
    // can run against a partial index as records arrive — fzf-from-a-pipe style.
    void buildIndex();

    // Internal: pull one batch from the streaming cursor, append it to the
    // index, emit indexGrew(), then re-post itself until the cursor is drained
    // (emitting indexLoaded()). Re-posting via the event loop lets queued
    // queries interleave between batches. `generation` guards against a
    // rebuild/clear superseding an in-flight stream.
    void readChunk(quint64 generation);

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
    void indexGrew(int trackCount);   // a batch landed — queries can run on the partial index
    void indexLoaded(int trackCount); // the stream is fully drained
    void indexError(const QString &error);
    void resultsReady(quint64 queryId, QVector<Search::ScoredResult> results, int totalMatches);

private:
    QString   m_dbPath;
    SearchIndex m_index;
    ExclusionSet m_excludes;
    std::atomic<quint64> m_latestQueryId{0};
    Database *m_db = nullptr;  // opened on the worker thread in buildIndex()

    // Streaming build state. m_buildGeneration bumps on every build/clear so
    // stale chunk events from a superseded stream abort.
    quint64 m_buildGeneration = 0;
    std::unique_ptr<TrackSearchCursor> m_cursor;
};

} // namespace Search
