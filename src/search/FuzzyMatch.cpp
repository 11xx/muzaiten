// C++ port of fzf's fuzzy matching algorithm.
// Ported from: github.com/junegunn/fzf src/algo/algo.go (MIT licence)
//
// Key differences from the Go original:
//  - Input is a Qt QString (QChar, UTF-16 internally) instead of util.Chars.
//  - No slab allocator — we use QVector for scratch space.
//  - normalizeRune is omitted (not needed for music metadata matching).
//  - The boundary-scheme is "default" (no path/history variant).

#include "search/FuzzyMatch.h"

#include <QChar>
#include <QString>
#include <QVector>

#include <algorithm>
#include <climits>

namespace Search {

// ---- character class -------------------------------------------------------

enum class CharClass : int {
    White    = 0,
    NonWord  = 1,
    Delimiter= 2,
    Lower    = 3,
    Upper    = 4,
    Letter   = 5,
    Number   = 6,
};

static const char *kDelimiters = "/,:;|";

static CharClass charClassOf(QChar c)
{
    if (c.isLower())      return CharClass::Lower;
    if (c.isUpper())      return CharClass::Upper;
    if (c.isDigit())      return CharClass::Number;
    if (c.isLetter())     return CharClass::Letter;
    if (c.isSpace())      return CharClass::White;
    const char ascii = c.toLatin1();
    if (ascii != 0) {
        for (const char *d = kDelimiters; *d; ++d) {
            if (ascii == *d) return CharClass::Delimiter;
        }
    }
    return CharClass::NonWord;
}

static int16_t bonusFor(CharClass prev, CharClass cur)
{
    // fzf: `class > charNonWord` — a word boundary requires the matched char to
    // be a delimiter or word char, not a non-word symbol itself (which falls
    // through to bonusNonWord below).
    if (cur > CharClass::NonWord) {
        switch (prev) {
        case CharClass::White:     return bonusBoundaryWhite;
        case CharClass::Delimiter: return bonusBoundaryDelimiter;
        case CharClass::NonWord:   return static_cast<int16_t>(bonusBoundary);
        default: break;
        }
    }
    if ((prev == CharClass::Lower && cur == CharClass::Upper) ||
        (prev != CharClass::Number && cur == CharClass::Number)) {
        return static_cast<int16_t>(bonusCamel123);
    }
    switch (cur) {
    case CharClass::NonWord:  return static_cast<int16_t>(bonusNonWord);
    case CharClass::Delimiter:return static_cast<int16_t>(bonusNonWord);
    case CharClass::White:    return bonusBoundaryWhite;
    default: break;
    }
    return 0;
}

static int16_t bonusAt(const QString &text, int idx)
{
    if (idx == 0) return bonusBoundaryWhite;
    return bonusFor(charClassOf(text[idx - 1]), charClassOf(text[idx]));
}

static QChar normalize(QChar c, bool caseSensitive)
{
    if (!caseSensitive && c.isUpper()) return c.toLower();
    return c;
}

// ---- exactMatchNaive -------------------------------------------------------

MatchResult exactMatchNaive(const QString &text,
                              const QString &pattern,
                              bool caseSensitive,
                              bool withPositions)
{
    if (pattern.isEmpty()) return {0, 0, 0, {}};

    const int N = static_cast<int>(text.length());
    const int M = static_cast<int>(pattern.length());
    if (N < M) return {};

    int bestPos  = -1;
    int16_t bestBonus = -1;

    int pidx = 0;
    int16_t firstBonus = 0;

    for (int i = 0; i < N; ++i) {
        const QChar c = normalize(text[i], caseSensitive);
        const QChar p = normalize(pattern[pidx], caseSensitive);

        if (c == p) {
            if (pidx == 0) {
                firstBonus = bonusAt(text, i);
            }
            ++pidx;
            if (pidx == M) {
                if (firstBonus > bestBonus) {
                    bestBonus = firstBonus;
                    bestPos   = i; // position of last char of this match
                }
                // Early exit on boundary match
                if (firstBonus >= bonusBoundaryWhite) break;
                // Reset to look for better occurrence
                i -= pidx - 1;
                pidx = 0;
                firstBonus = 0;
            }
        } else {
            i -= pidx;
            pidx = 0;
            firstBonus = 0;
        }
    }

    if (bestPos < 0) return {};

    const int sidx = bestPos - M + 1;
    const int eidx = bestPos + 1;

    // Score: use the fzf calculateScore helper approach (simplified inline)
    int score = 0;
    int consecutive = 0;
    int16_t firstBonusInRun = 0;
    bool inGap = false;
    CharClass prevClass = (sidx > 0) ? charClassOf(text[sidx - 1]) : CharClass::White;

    QVector<int> positions;
    if (withPositions) positions.reserve(M);

    for (int idx = sidx; idx < eidx; ++idx) {
        const QChar c = normalize(text[idx], caseSensitive);
        const CharClass cc = charClassOf(text[idx]);
        if (c == normalize(pattern[idx - sidx], caseSensitive)) {
            if (withPositions) positions.append(idx);
            score += scoreMatch;
            int16_t bonus = bonusFor(prevClass, cc);
            if (consecutive == 0) {
                firstBonusInRun = bonus;
            } else {
                if (bonus >= bonusBoundary && bonus > firstBonusInRun) {
                    firstBonusInRun = bonus;
                }
                bonus = std::max({static_cast<int16_t>(bonus),
                                  firstBonusInRun,
                                  static_cast<int16_t>(bonusConsecutive)});
            }
            if (idx - sidx == 0) {
                score += static_cast<int>(bonus) * bonusFirstCharMultiplier;
            } else {
                score += static_cast<int>(bonus);
            }
            inGap = false;
            ++consecutive;
        } else {
            score += inGap ? scoreGapExtension : scoreGapStart;
            inGap = true;
            consecutive = 0;
            firstBonusInRun = 0;
        }
        prevClass = cc;
    }

    return {sidx, eidx, score, withPositions ? positions : QVector<int>{}};
}

// ---- fuzzyMatchV2 ----------------------------------------------------------
// Modified Smith-Waterman DP.

// Greedy V1 used as fallback for very large inputs (same as fzf).
static MatchResult fuzzyMatchV1(const QString &text,
                                 const QString &pattern,
                                 bool caseSensitive,
                                 bool withPositions)
{
    const int N = static_cast<int>(text.length());
    const int M = static_cast<int>(pattern.length());

    // Forward scan: find first fuzzy match extent
    int firstIdx = 0, lastIdx = -1;
    int pidx = 0;
    for (int i = 0; i < N && pidx < M; ++i) {
        if (normalize(text[i], caseSensitive) == normalize(pattern[pidx], caseSensitive)) {
            if (pidx == 0) firstIdx = i;
            lastIdx = i;
            ++pidx;
        }
    }
    if (pidx != M) return {};

    // Backward scan to find shortest match
    int sidx = firstIdx, eidx = lastIdx + 1;
    pidx = M - 1;
    for (int i = lastIdx; i >= 0 && pidx >= 0; --i) {
        if (normalize(text[i], caseSensitive) == normalize(pattern[pidx], caseSensitive)) {
            if (pidx == 0) { sidx = i; break; }
            --pidx;
        }
    }

    // Score via calculateScore
    int score = 0;
    int consecutive = 0;
    int16_t firstBonusInRun = 0;
    bool inGap = false;
    CharClass prevClass = (sidx > 0) ? charClassOf(text[sidx - 1]) : CharClass::White;
    int patIdx = 0;

    QVector<int> positions;
    if (withPositions) positions.reserve(M);

    for (int idx = sidx; idx < eidx; ++idx) {
        const QChar textC  = normalize(text[idx], caseSensitive);
        const QChar patC   = normalize(pattern[patIdx], caseSensitive);
        const CharClass cc = charClassOf(text[idx]);
        if (textC == patC) {
            if (withPositions) positions.append(idx);
            score += scoreMatch;
            int16_t bonus = bonusFor(prevClass, cc);
            if (consecutive == 0) {
                firstBonusInRun = bonus;
            } else {
                if (bonus >= bonusBoundary && bonus > firstBonusInRun) {
                    firstBonusInRun = bonus;
                }
                bonus = std::max({static_cast<int16_t>(bonus),
                                  firstBonusInRun,
                                  static_cast<int16_t>(bonusConsecutive)});
            }
            score += (patIdx == 0)
                ? static_cast<int>(bonus) * bonusFirstCharMultiplier
                : static_cast<int>(bonus);
            inGap = false;
            ++consecutive;
            ++patIdx;
        } else {
            score += inGap ? scoreGapExtension : scoreGapStart;
            inGap = true;
            consecutive = 0;
            firstBonusInRun = 0;
        }
        prevClass = cc;
    }
    return {sidx, eidx, score, withPositions ? positions : QVector<int>{}};
}

MatchResult fuzzyMatchV2(const QString &text,
                          const QString &pattern,
                          bool caseSensitive,
                          bool withPositions)
{
    if (pattern.isEmpty()) return {0, 0, 0, {}};

    const int M = static_cast<int>(pattern.length());
    const int N = static_cast<int>(text.length());
    if (M > N) return {};

    // Fallback for large n*m or long pattern (same threshold as fzf)
    constexpr qint64 kSlabSize = 100 * 1024; // ~100k int16 slots
    if (static_cast<qint64>(N) * M > kSlabSize || M > 1000) {
        return fuzzyMatchV1(text, pattern, caseSensitive, withPositions);
    }

    // Phase 1: Find the range [minIdx, maxIdx) where the pattern can appear
    // (first pattern-char occurrence to last pattern-char occurrence).
    // Build lowercased rune array T over this range, bonus array B, first-occurrence F.
    int minIdx = -1, maxIdx = -1;
    {
        // Forward pass: find first occurrence of each pattern char
        int pidx = 0;
        for (int i = 0; i < N && pidx < M; ++i) {
            if (normalize(text[i], caseSensitive) == normalize(pattern[pidx], caseSensitive)) {
                if (pidx == 0) minIdx = (i > 0) ? i - 1 : 0;
                maxIdx = i;
                ++pidx;
            }
        }
        if (pidx != M) return {};
        // Scan backwards for the last occurrence of the last pattern char
        const QChar lastPatC = normalize(pattern[M - 1], caseSensitive);
        for (int i = N - 1; i > maxIdx; --i) {
            if (normalize(text[i], caseSensitive) == lastPatC) {
                maxIdx = i;
                break;
            }
        }
        if (minIdx < 0) minIdx = 0;
        ++maxIdx; // exclusive
    }

    const int rangeN = maxIdx - minIdx;

    // Build T (normalized chars), B (bonus), F (first occurrence per pattern char)
    QVector<QChar> T(rangeN);
    QVector<int16_t> B(rangeN);
    QVector<int32_t> F(M, -1);

    CharClass prevClass = (minIdx > 0) ? charClassOf(text[minIdx - 1]) : CharClass::White;
    int pidx = 0;
    for (int off = 0; off < rangeN; ++off) {
        const int i = minIdx + off;
        T[off] = normalize(text[i], caseSensitive);
        const CharClass cc = charClassOf(text[i]);
        B[off] = bonusFor(prevClass, cc);
        prevClass = cc;
        if (pidx < M && T[off] == normalize(pattern[pidx], caseSensitive)) {
            F[pidx] = static_cast<int32_t>(off);
            ++pidx;
        }
    }
    if (pidx != M) return {};

    // Phase 2: First-row (H0/C0) for pattern[0]
    const int f0 = static_cast<int>(F[0]);
    int lastIdx = 0; // last position where pattern[M-1] could occur
    for (int off = rangeN - 1; off >= 0; --off) {
        if (T[off] == normalize(pattern[M - 1], caseSensitive)) {
            lastIdx = off;
            break;
        }
    }

    QVector<int16_t> H0(rangeN, 0);
    QVector<int16_t> C0(rangeN, 0);
    int16_t maxScore = 0;
    int maxScorePos = 0;
    bool inGap = false;
    int16_t prevH0 = 0;
    const QChar pchar0 = normalize(pattern[0], caseSensitive);
    for (int off = 0; off < rangeN; ++off) {
        if (T[off] == pchar0) {
            const int16_t score = static_cast<int16_t>(scoreMatch + static_cast<int>(B[off]) * bonusFirstCharMultiplier);
            H0[off] = score;
            C0[off] = 1;
            if (M == 1 && score > maxScore) {
                maxScore = score;
                maxScorePos = off;
                if (B[off] >= bonusBoundaryWhite) break;
            }
            inGap = false;
        } else {
            const int16_t gapScore = inGap ? static_cast<int16_t>(prevH0 + scoreGapExtension)
                                            : static_cast<int16_t>(prevH0 + scoreGapStart);
            H0[off] = std::max<int16_t>(gapScore, 0);
            C0[off] = 0;
            inGap = true;
        }
        prevH0 = H0[off];
    }

    if (M == 1) {
        if (!H0[maxScorePos]) return {};
        const int sidx = minIdx + maxScorePos;
        MatchResult res{sidx, sidx + 1, static_cast<int>(maxScore), {}};
        if (withPositions) res.positions = {sidx};
        return res;
    }

    // Phase 3: Fill DP matrix H[M × width], C[M × width] over [f0, lastIdx]
    const int width = lastIdx - f0 + 1;
    if (width <= 0) return {};
    QVector<int16_t> H(width * M, 0);
    QVector<int16_t> C(width * M, 0);
    // Copy H0/C0 into first row
    for (int off = 0; off < width; ++off) {
        H[off]       = H0[f0 + off];
        C[off]       = C0[f0 + off];
    }

    maxScore = 0;
    maxScorePos = 0;

    for (int row = 1; row < M; ++row) {
        const int f = static_cast<int>(F[row]);
        if (f > lastIdx) break;
        const QChar pchar = normalize(pattern[row], caseSensitive);
        inGap = false;
        const int rowBase = row * width;
        const int prevRowBase = (row - 1) * width;
        // H_left[-1] = 0
        int16_t Hleft_prev = 0;
        for (int off = f - f0; off <= lastIdx - f0; ++off) {
            const int col = f0 + off;
            int16_t s1 = 0, s2 = 0;
            int16_t consecutive = 0;

            s2 = inGap ? static_cast<int16_t>(Hleft_prev + scoreGapExtension)
                       : static_cast<int16_t>(Hleft_prev + scoreGapStart);

            if (T[col] == pchar) {
                // Diagonal + match
                const int16_t hdiag = (off > 0 && row > 0) ? H[prevRowBase + off - 1] : 0;
                const int16_t cdiag = (off > 0 && row > 0) ? C[prevRowBase + off - 1] : 0;
                s1 = static_cast<int16_t>(hdiag + scoreMatch);
                int16_t b = B[col];
                consecutive = static_cast<int16_t>(cdiag + 1);
                if (consecutive > 1) {
                    const int16_t fb = (col - consecutive + 1 >= 0) ? B[col - consecutive + 1] : 0;
                    if (b >= bonusBoundary && b > fb) {
                        consecutive = 1;
                    } else {
                        b = std::max({b, static_cast<int16_t>(bonusConsecutive), fb});
                    }
                }
                if (s1 + b < s2) {
                    s1 = static_cast<int16_t>(s1 + B[col]);
                    consecutive = 0;
                } else {
                    s1 = static_cast<int16_t>(s1 + b);
                }
            }

            C[rowBase + off] = consecutive;
            inGap = s1 < s2;
            const int16_t score = std::max({s1, s2, static_cast<int16_t>(0)});
            if (row == M - 1 && score > maxScore) {
                maxScore = score;
                maxScorePos = col;
            }
            H[rowBase + off] = score;
            Hleft_prev = score;
        }
    }

    if (maxScore == 0) return {};

    // Phase 4: Backtrace to find positions (optional)
    QVector<int> positions;
    if (withPositions) {
        positions.reserve(M);
        int i = M - 1;
        int j = maxScorePos - f0;
        bool preferMatch = true;
        while (true) {
            const int I = i * width;
            const int16_t s = H[I + j];
            const int16_t s1 = (i > 0 && j > 0) ? H[(i - 1) * width + j - 1] : 0;
            const int16_t s2 = (j > 0)           ? H[I + j - 1]               : 0;
            if (s > s1 && (s > s2 || (s == s2 && preferMatch))) {
                positions.prepend(minIdx + f0 + j);
                if (i == 0) break;
                --i;
            }
            preferMatch = C[I + j] > 1 ||
                          (I + width + j + 1 < static_cast<int>(C.size()) && C[I + width + j + 1] > 0);
            --j;
            if (j < 0) break;
        }
    }

    return {minIdx + f0, minIdx + maxScorePos + 1, static_cast<int>(maxScore),
            withPositions ? positions : QVector<int>{}};
}

} // namespace Search
