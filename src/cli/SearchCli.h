#pragma once

// Standalone library search for muzaitenctl.
//
// Opens the library database directly (app-independent — works whether or not a
// muzaiten instance is running), loads the folded-index cache (or builds it from
// the DB on a miss / --refresh and writes it back), and exposes the in-memory
// SearchIndex. The non-interactive `search <text>` and the interactive fzf
// picker both build on this.

#include "search/SearchIndex.h"
#include "search/SearchRecord.h"

#include <QString>

#include <functional>

namespace SearchCli {

// Path to the library database (AppPaths::dataDir()/library.sqlite).
QString libraryDbPath();

// Path to the on-disk search cache (shared with the GUI).
QString cachePath();

// Delete the cache file. Returns true if a file was removed (false if absent);
// *path receives the cache path either way.
bool clearCache(QString *path = nullptr);

struct LoadResult {
    bool    ok = false;       // index is populated and queryable
    QString error;            // populated when ok == false
    bool    usedCache = false;// accepted a matching cache
    bool    wasStale = false; // encountered a cache with a different source signature
    bool    rebuilt = false;  // built fresh from the DB and (re)wrote the cache
    int     trackCount = 0;
    QString cacheReason;
    qint64 sourceRevision = -1;
};

// Populate `index` from a matching cache or a consistent database read snapshot.
// Stale/missing/corrupt caches and forceRefresh trigger a rebuild.
LoadResult loadIndex(Search::SearchIndex &index, bool forceRefresh);

// Like loadIndex, but streams each record to `sink` as it becomes available
// instead of materializing a SearchIndex — so the fzf picker can show rows
// immediately. From a matching cache it streams during deserialization; on a
// miss, stale cache or forceRefresh it streams a database snapshot instead.
LoadResult streamRecords(const std::function<void(const Search::SearchRecord &)> &sink, bool forceRefresh);

} // namespace SearchCli
