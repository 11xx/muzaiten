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

#include "search/FuzzyMatch.h"
#include "search/SearchQuery.h"
#include "search/SearchRecord.h"

#include <QVector>

namespace Search {

// Per-field match ranges for one result row.
struct FieldRanges {
    QVector<int> titlePositions;
    QVector<int> artistPositions;
    QVector<int> albumArtistPositions;
    QVector<int> albumPositions;
    QVector<int> filenamePositions;
    QVector<int> pathPositions;
};

struct ScoredResult {
    SearchRecord rec;               // copy of the matching record (embedded for thread safety)
    int          score       = 0;
    FieldRanges  ranges;
};

class SearchIndex {
public:
    SearchIndex() = default;

    // Replace the index contents. Takes ownership of the supplied records.
    void build(QVector<SearchRecord> records);

    // Release all indexed records, freeing memory.
    void clear();

    // Run a query and return scored, ranked results.
    // results.isEmpty() if the query is empty (show nothing, not everything).
    QVector<ScoredResult> match(const SearchQuery &query, bool fuzzyMode) const;

    int size() const { return static_cast<int>(m_records.size()); }
    bool isEmpty() const { return m_records.isEmpty(); }

private:
    QVector<SearchRecord> m_records;
};

} // namespace Search
