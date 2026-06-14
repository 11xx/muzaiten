#pragma once

// Background worker that owns the search index and runs queries off the GUI
// thread.  Lives on its own QThread (same pattern as MpdImportWorker).
//
// Thread affinity: all slots execute on the worker thread (via queued
// connections).  All signals are queued back to the GUI thread.

#include "search/Exclusion.h"
#include "search/IndexCache.h"
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

    // Internal: pull one batch from the streaming cursor and feed it to the
    // build in progress, then re-post itself until the cursor is drained.
    // Re-posting via the event loop lets queued queries interleave between
    // batches. Foreground (cold) builds append to the live index and emit
    // indexGrew() so results stream in; background (cache-refresh) builds fill a
    // staging index silently and swap it in at the end. `generation` guards
    // against a rebuild/clear superseding an in-flight stream.
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
    void indexGrew(int trackCount);   // a cold-build batch landed — queries can run on the partial index
    void indexLoaded(int trackCount); // index queryable: cold build done, or a (possibly stale) cache loaded
    void indexRefreshing();           // a quiet background cache-refresh build has begun
    void indexRefreshed(int trackCount); // background refresh done; live index swapped to the fresh data
    void indexError(const QString &error);
    void resultsReady(quint64 queryId, QVector<Search::ScoredResult> results, int totalMatches);

private:
    // Whether the in-flight stream feeds the live index (cold start, streams in
    // visibly) or a staging index swapped in on completion (background refresh
    // while the cached index keeps serving queries).
    enum class BuildMode { Foreground, Background };

    void finishBuild(quint64 generation);

    QString   m_dbPath;
    SearchIndex m_index;       // live, queryable index
    SearchIndex m_staging;     // background-refresh build target, swapped into m_index when done
    ExclusionSet m_excludes;
    std::atomic<quint64> m_latestQueryId{0};
    Database *m_db = nullptr;  // opened on the worker thread in buildIndex()

    // Streaming build state. m_buildGeneration bumps on every build/clear so
    // stale chunk events from a superseded stream abort.
    quint64 m_buildGeneration = 0;
    BuildMode m_buildMode = BuildMode::Foreground;
    CacheSignature m_pendingSignature;  // signature to stamp the cache the in-flight build will write
    std::unique_ptr<TrackSearchCursor> m_cursor;
};

} // namespace Search
