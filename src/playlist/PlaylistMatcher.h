#pragma once

// Resolves ImportEntry rows against the in-memory search index, reusing the
// fzf search engine rather than hand-rolled string matching. Pure function of
// (index, entry) — offline-testable with a hand-built SearchIndex.
//
// Matching strategy per entry:
//   1. directPath: exact path lookup, then basename lookup (m3u from another
//      machine with the same files).
//   2. Field-scoped query (artist:… title:…) built from normalized,
//      noise-stripped guesses; exact mode first, then fuzzy.
//   3. Relaxed free-text query (all words, fuzzy) as the last resort.
//
// Decision (conservative — bias to MultiMatch/Pending so nothing wrong lands
// silently): a clearly dominant top hit → Matched; several close hits →
// MultiMatch with the candidate paths kept; nothing → Pending. Album and
// duration are tiebreakers, never filters.

#include "playlist/PlaylistImport.h"
#include "search/SearchIndex.h"

#include <QString>
#include <QStringList>

namespace PlaylistMatcher {

enum class Decision { Matched, MultiMatch, Pending };

struct Outcome {
    Decision decision = Decision::Pending;
    Search::SearchRecord best;       // valid when decision == Matched
    QStringList candidatePaths;      // top candidates when decision == MultiMatch
    QString queryUsed;               // SearchQuery::parse-compatible; stored on the
                                     // item so the edit modal can re-run it
};

// Score ratio below which a runner-up counts as "close" to the top hit.
inline constexpr double kCloseScoreRatio = 0.85;
// Max candidates remembered for a MultiMatch item.
inline constexpr int kMaxCandidates = 5;
// Duration tiebreaker tolerance.
inline constexpr qint64 kDurationToleranceMs = 3000;

Outcome match(const Search::SearchIndex &index, const PlaylistImport::ImportEntry &entry);

} // namespace PlaylistMatcher
