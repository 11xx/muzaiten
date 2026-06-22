#pragma once

#include "search/SearchIndex.h"

class Database;

namespace PlaylistImport {

// Builds the search index used to resolve playlist-import entries against the
// library, honoring the user's "Search ranking" exclude rules (so .Trash/backup
// dirs etc. never surface as match candidates). Shared by every importer — the
// modal's worker, the background drop-import worker — so the candidate set is
// identical regardless of how the import was started.
Search::SearchIndex buildMatchIndex(Database &db);

} // namespace PlaylistImport
