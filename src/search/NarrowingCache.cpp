#include "search/NarrowingCache.h"

namespace Search {

QVector<ScoredResult> NarrowingCache::run(const SearchIndex &index,
                                           const SearchQuery &query, bool fuzzyMode,
                                           const ExclusionSet &excludes,
                                           quint64 exclusionVersion, int *totalMatches,
                                           const std::function<bool()> &cancelled)
{
    const bool reuse = m_valid && m_fuzzyMode == fuzzyMode
        && m_generation == index.generation()
        && m_exclusionVersion == exclusionVersion && narrows(m_query, query);
    QVector<int> matches;
    auto results = index.match(query, fuzzyMode, excludes, totalMatches, &matches,
                               reuse ? &m_matchingIndices : nullptr, cancelled);
    if (cancelled && cancelled()) {
        if (totalMatches) *totalMatches = 0;
        return {};
    }
    m_query = query;
    m_fuzzyMode = fuzzyMode;
    m_generation = index.generation();
    m_exclusionVersion = exclusionVersion;
    m_matchingIndices = std::move(matches);
    m_valid = true;
    return results;
}

} // namespace Search
