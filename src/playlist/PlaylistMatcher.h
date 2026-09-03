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
//   3. Title-anchored query (title:… alone, exact then fuzzy) keeping only the
//      hits whose credited artist agrees with the artist guess by token subset,
//      so a credit written differently on each side ("Usher feat. Lil Jon" for
//      a track tagged "Usher") still resolves. Agreement raises a title hit;
//      it never admits one on its own, so an empty artist guess skips the tier.
//   4. Relaxed free-text query (all words, fuzzy) as the last resort.
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

// Matched: a confident single pick. Approximate: a single pick we auto-chose but
// whose confidence is below the bar — flagged for a glance. MultiMatch: several
// close candidates, no confident pick. Pending: nothing found.
enum class Decision { Matched, Approximate, MultiMatch, Pending };

struct Outcome {
    Decision decision = Decision::Pending;
    Search::SearchRecord best;       // valid when decision == Matched or Approximate
    QStringList candidatePaths;      // close candidates for MultiMatch/Approximate review
    QString queryUsed;               // SearchQuery::parse-compatible; stored on the
                                     // item so the edit modal can re-run it
    int confidence0To100 = 0;        // heuristic certainty of `best` (not a probability)
};

// Score ratio below which a runner-up counts as "close" to the top hit.
inline constexpr double kCloseScoreRatio = 0.85;
// Max candidates remembered for a MultiMatch item.
inline constexpr int kMaxCandidates = 5;
// Duration tiebreaker tolerance.
inline constexpr qint64 kDurationToleranceMs = 3000;

// Confidence is a tuned heuristic, not a probability: a per-tier base reward for
// how the hit was found, plus corroboration. At/above kMatchedConfidence a single
// pick is "Matched"; below it the pick is still auto-chosen but flagged
// "Approximate" for review. Tuned so exact/scoped matches stay Matched, an
// uncontested exact title-anchored hit just reaches the bar, and fuzzy
// title-anchored or free-text fallbacks land in the Approximate band unless
// album or duration corroborate them.
inline constexpr int kConfidencePath               = 100;  // resolved by file path
inline constexpr int kConfidenceScopedExact        = 90;   // artist:/title: exact-mode hit
inline constexpr int kConfidenceScopedFuzzy        = 72;   // artist:/title: fuzzy-mode hit
inline constexpr int kConfidenceTitleAnchoredExact = 70;   // title: exact-mode hit, artist agrees by token subset
inline constexpr int kConfidenceTitleAnchoredFuzzy = 62;   // title: fuzzy-mode hit, artist agrees by token subset
inline constexpr int kConfidenceRelaxed            = 55;   // last-resort free-text hit
inline constexpr int kConfidenceUncontested        = 8;    // only one candidate found
inline constexpr int kConfidenceAlbumMatch         = 10;   // album corroborates
inline constexpr int kConfidenceDuration           = 8;    // duration within tolerance
inline constexpr int kMatchedConfidence            = 78;   // Matched at/above, else Approximate

// Magnet guard (fuzzy tiers only): the minimum fraction of a candidate's
// title+artist length the query must plausibly explain. A short query fuzzily
// hitting a much longer field (e.g. a 4-token query on a 100-char opera title)
// is almost always a false positive, since long strings contain nearly any short
// subsequence. Exact-substring tiers are trustworthy and skip this.
inline constexpr double kMinFuzzyCoverage = 0.2;

// With `exactOnly`, only the exact-substring tiers run (path + scoped phrase,
// exact mode) — no title-anchored, fuzzy, or relaxed fallback. Stricter: nothing
// matches unless both artist and title match exactly, so uncertain rows stay
// Pending instead of being guessed.
Outcome match(const Search::SearchIndex &index, const PlaylistImport::ImportEntry &entry,
              bool exactOnly = false);

// Token-subset agreement between two artist credits, the rule behind the
// title-anchored tier. Both credits are folded like the search index, split at
// anything that is not a letter or digit, and stripped of join words (and, feat,
// ft, featuring, vs, x, the; "&", "+" and "/" fall away as separators). The
// credits agree when every remaining token of one side occurs in the other, in
// any order: "Usher feat. Lil Jon & Ludacris" agrees with "Usher", "Simon and
// Garfunkel" with "Simon & Garfunkel", while "Lil Jon feat. Ying Yang Twins" and
// "Lil Jon & The East Side Boyz" each name someone the other does not and
// disagree. A credit made only of join words keeps them ("The The" is a name),
// and an empty credit agrees with nothing.
bool artistCreditsAgree(const QString &credit, const QString &other);

} // namespace PlaylistMatcher
