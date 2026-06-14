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
    bool    usedCache = false;// loaded (at least partly) from the cache
    bool    wasStale = false; // a cache was used whose signature no longer matches the DB
    bool    rebuilt = false;  // built fresh from the DB and (re)wrote the cache
    int     trackCount = 0;
};

// Populate `index`. With a fresh cache, loads it instantly. With a stale cache
// it is still used (wasStale=true) unless forceRefresh is set. A missing/corrupt
// cache or forceRefresh triggers a full build from the DB, which also rewrites
// the cache.
LoadResult loadIndex(Search::SearchIndex &index, bool forceRefresh);

// Like loadIndex, but streams each record to `sink` as it becomes available
// instead of materializing a SearchIndex — so the fzf picker can show rows
// immediately. From the cache it streams during deserialization; on a miss /
// forceRefresh it streams from the DB build and writes the cache afterward.
LoadResult streamRecords(const std::function<void(const Search::SearchRecord &)> &sink, bool forceRefresh);

} // namespace SearchCli
