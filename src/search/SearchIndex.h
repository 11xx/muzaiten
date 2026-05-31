#pragma once

// In-memory search index over all library tracks.
//
// Usage:
//   SearchIndex idx;
//   idx.build(records);               // one-time load, O(n)
//   auto results = idx.match(query, fuzzyMode);  // per-keystroke, O(n·terms)
//
// build() replaces the current index atomically.
// match() is const and thread-safe from a single thread (do NOT call from
// multiple threads simultaneously without external locking).

#include "search/Exclusion.h"
#include "search/FuzzyMatch.h"
#include "search/SearchQuery.h"
#include "search/SearchRecord.h"

#include <QVector>

namespace Search {

struct ScoredResult {
    SearchRecord rec;          // copy of the matching record (embedded for thread safety)
    int          score = 0;
};

// Cap on the number of results materialized/returned per query. A broad query
// (e.g. one character) can match nearly the whole library; we only ever display
// and ship the top-scoring slice, which keeps per-keystroke cost bounded.
inline constexpr int kMaxResults = 2000;

// Which displayed line a highlight request targets (used by the delegate to
// pick the query terms that apply to a given field).
enum class HighlightField { Title, Artist, Album, Path };

// Compute the character positions in `text` highlighted by the query's text
// terms that apply to `field`. Runs only for on-screen rows (the delegate),
// so it can afford to find every occurrence. `text` is the original-case
// display string; matching is case-insensitive.
QVector<int> highlightPositions(const QString &text,
                                 const SearchQuery &query,
                                 HighlightField field,
                                 bool fuzzyMode);

class SearchIndex {
public:
    SearchIndex() = default;

    // Replace the index contents. Takes ownership of the supplied records.
    void build(QVector<SearchRecord> records);

    // Release all indexed records, freeing memory.
    void clear();

    // Run a query and return the top-scoring results (capped at kMaxResults).
    // Empty query → empty result. When totalMatches is non-null it receives the
    // full (uncapped) number of matching records. `excludes`, when non-empty,
    // drop matching records during the filter phase (before scoring).
    QVector<ScoredResult> match(const SearchQuery &query, bool fuzzyMode,
                                 const ExclusionSet &excludes = {},
                                 int *totalMatches = nullptr) const;

    int size() const { return static_cast<int>(m_records.size()); }
    bool isEmpty() const { return m_records.isEmpty(); }

private:
    QVector<SearchRecord> m_records;
};

} // namespace Search
