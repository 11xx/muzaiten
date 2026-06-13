#pragma once

// Search folding — the "phonetic / ASCII-ish" normalization engine.
//
// This is a small, self-contained, dependency-free library that maps arbitrary
// Unicode text toward a lowercase, ASCII-leaning form so that search matches by
// *sound/shape* rather than by encoding.  It is the single normalization point
// shared by the index (haystack) and the query parser (needle): fold both sides
// with the same function and the existing matcher (orderless AND, fzf fuzzy,
// anchors, exclusions) works on folded text with no further changes.
//
// Pipeline (per source code point, ASCII fast-pathed):
//   1. lowercase
//   2. Unicode NFD (canonical) decomposition
//   3. strip combining diacritical marks            (é→e, ã→a, ş→s, ğ→g, ö→o)
//   4. script transliteration table                 (ß→ss, ı→i, θ→th, ж→zh)
//   5. kana → romaji                                 (さ→sa, きゃ→kya, っか→kka)
// Kanji (and any unmapped code point) passes through verbatim so typing the
// original character still matches; word-level kanji readings are a later slice
// (bundled dict files + per-track reading tags) that plugs in ahead of step 5.
//
// NOTE: unlike a slug generator, folding deliberately preserves spaces and
// punctuation — the matcher relies on word boundaries for its fzf bonuses and
// on substring semantics, so we transliterate but never collapse to separators.
//
// To extend coverage, add rows to the tables in Fold.cpp (one section per
// script); the design intent is that another contributor — or an LLM — can drop
// in a language table without touching the pipeline.

#include <QString>
#include <QVector>

namespace Search::Fold {

// Folded text plus a map from each folded character back to the index of the
// source character it derived from.  Used by highlighting to project match
// positions (computed in folded space) onto the original display string.
//
//   srcIndex[i] = index into the original `src` of the source char that
//                 produced folded char `i`.
// Expansions (三→"san", ß→"ss") map every output char to the one source index;
// deletions (combining marks) simply emit no output char for that source index.
struct FoldResult {
    QString      text;
    QVector<int> srcIndex;
};

// `romanizeCjk` gates the two expensive, CJK-specific stages (kanji dictionary
// and kana→romaji). With it off you get the cheap "basic" fold — lowercase,
// diacritic stripping, and Greek/Cyrillic/Turkish transliteration — while kana
// and kanji pass through unchanged. The search index uses this to become
// queryable immediately (basic) and upgrade to full romaji in the background.

// Full fold with the source-index map.  Used on the (few) visible rows when
// computing highlight positions.
FoldResult fold(const QString &src, bool romanizeCjk = true);

// Full fold without the index map — the hot path used to build the index and to
// normalize query terms.  Produces text identical to fold(src).text.
QString foldText(const QString &src, bool romanizeCjk = true);

} // namespace Search::Fold
