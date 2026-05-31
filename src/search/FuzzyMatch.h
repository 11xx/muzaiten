#pragma once

// C++ port of the fzf matching algorithms (algo.go / calculateScore).
//
// References:
//   github.com/junegunn/fzf — src/algo/algo.go (MIT licence)
//
// Two algorithms are provided:
//   exactMatchNaive — optimal substring search with boundary bonuses.
//                     O(n·m) worst case but O(n) in the common fast path.
//   fuzzyMatchV2    — modified Smith-Waterman DP, finds the highest-scoring
//                     subsequence. Used only when fuzzy mode is toggled on.
//
// Both return a MatchResult{start, end, score, positions}.
// When positions are not needed pass withPositions=false (faster).

#include <QString>
#include <QVector>

namespace Search {

struct MatchResult {
    int start = -1;      // -1 means no match
    int end   = -1;
    int score = 0;
    QVector<int> positions;  // matched character indices (only filled when withPositions=true)

    bool matched() const { return start >= 0; }
};

// Scoring constants (identical to fzf defaults)
inline constexpr int scoreMatch        =  16;
inline constexpr int scoreGapStart     =  -3;
inline constexpr int scoreGapExtension =  -1;
inline constexpr int bonusBoundary     = scoreMatch / 2;    // 8
inline constexpr int bonusNonWord      = scoreMatch / 2;    // 8
inline constexpr int bonusCamel123     = bonusBoundary + scoreGapExtension;  // 7
inline constexpr int bonusConsecutive  = -(scoreGapStart + scoreGapExtension); // 4
inline constexpr int bonusFirstCharMultiplier = 2;

// Bonus for the first character position after different character classes.
// (Using the fzf "default" scheme: bonusBoundaryWhite = bonusBoundary + 2,
//  bonusBoundaryDelimiter = bonusBoundary + 1.)
inline constexpr int16_t bonusBoundaryWhite     = static_cast<int16_t>(bonusBoundary + 2);
inline constexpr int16_t bonusBoundaryDelimiter = static_cast<int16_t>(bonusBoundary + 1);

// ---- public API -----------------------------------------------------------

// Exact (substring) match: all pattern characters must appear consecutively.
// pattern must already be lowercased when caseSensitive==false.
// Returns the best-scoring (highest boundary bonus) occurrence.
MatchResult exactMatchNaive(const QString &text,
                             const QString &pattern,
                             bool caseSensitive,
                             bool withPositions = false);

// Fuzzy (subsequence) match: finds the highest-scoring subsequence alignment.
// pattern must already be lowercased when caseSensitive==false.
MatchResult fuzzyMatchV2(const QString &text,
                          const QString &pattern,
                          bool caseSensitive,
                          bool withPositions = false);

// Convenience: run exact or fuzzy based on the fuzzyMode flag.
inline MatchResult matchTerm(const QString &text,
                              const QString &pattern,
                              bool caseSensitive,
                              bool fuzzyMode,
                              bool withPositions = false)
{
    if (fuzzyMode) {
        return fuzzyMatchV2(text, pattern, caseSensitive, withPositions);
    }
    return exactMatchNaive(text, pattern, caseSensitive, withPositions);
}

} // namespace Search
