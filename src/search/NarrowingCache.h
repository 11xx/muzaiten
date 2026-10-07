#pragma once

#include "search/SearchIndex.h"

namespace Search {

// Reuses matches only while the indexed data and query interpretation agree.
class NarrowingCache {
public:
    QVector<ScoredResult> run(const SearchIndex &index, const SearchQuery &query,
                              bool fuzzyMode, const ExclusionSet &excludes,
                              quint64 exclusionVersion, int *totalMatches,
                              const std::function<bool()> &cancelled = {},
                              int *hiddenMatches = nullptr);

private:
    SearchQuery m_query;
    bool m_fuzzyMode = false;
    quint64 m_generation = 0;
    quint64 m_exclusionVersion = 0;
    QVector<int> m_matchingIndices;
    bool m_valid = false;
};

} // namespace Search
