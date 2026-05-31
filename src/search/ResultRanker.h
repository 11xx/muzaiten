#pragma once

// Front-end re-ordering of matched search results by the user's ranking config.
// Operates on the (already relevance-capped) result set returned by the engine,
// so it never touches the engine's hot path.

#include "search/RankConfig.h"
#include "search/SearchIndex.h"   // ScoredResult
#include "search/SearchRecord.h"

#include <QVector>

namespace Search {

// Composite "audio quality" score: lossless first, then (future) bit depth,
// then sample rate, then bitrate and channels. Higher = better.
int audioQualityScore(const SearchRecord &rec);

class ResultRanker {
public:
    ResultRanker() = default;
    explicit ResultRanker(const RankConfig &config) : m_config(config) {}

    void setConfig(const RankConfig &config) { m_config = config; }

    // Stable in-place re-sort of results by the configured criteria. With no
    // enabled rules the engine's relevance order is preserved.
    void sort(QVector<ScoredResult> &results) const;

private:
    RankConfig m_config;
};

} // namespace Search
