#pragma once

// Search folding — the "phonetic / ASCII-ish" normalization engine.
//
// Maps arbitrary Unicode text toward a lowercase, ASCII-leaning form so that
// search matches by *sound/shape* rather than by encoding. It is the single
// normalization point shared by the index (haystack) and the query parser
// (needle): fold both sides with the same function and the matcher (orderless
// AND, fzf fuzzy, anchors, exclusions) works on folded text unchanged.
//
// A string has one primary fold and, for scripts read more than one way, a
// few alternate readings:
//
// - The primary fold is deterministic and compositional, so a query typed in
//   any script folds to a substring of the fold of text containing it. It
//   drops control and format characters, applies NFKC, lowercases, strips
//   diacritics, folds look-alike punctuation onto ASCII, composes canonically
//   equivalent sequences, and romanizes letter by letter: Latin, Greek,
//   Cyrillic, Georgian, Armenian (ե→e), kana (each on its own: きゃ→kiya,
//   っか→kka; iteration marks as written) and Hangul (Revised Romanization,
//   syllable by syllable: 악단광칠→akdangwangchil). Scripts whose reading
//   depends on the surrounding letters keep a normalized surface: CJK
//   ideographs (folded to simplified Chinese), Thai, Lao, the Brahmic
//   scripts, Tibetan, and Arabic, Hebrew and Syriac without their vowel
//   marks and letter variants.
// - Alternate readings are what a searcher might type instead: German
//   ae/oe/ue and ü as i, modern and phonetic Greek, common and Slavic
//   Cyrillic spellings with Ukrainian, Belarusian and Serbian letters,
//   Eastern and Western Armenian as read (Երևան→yerevan), wapuro romaji
//   (きゃ→kya, ティ→ti) and Hepburn with long vowels collapsed (tokyo),
//   Japanese kanji readings from a seed dictionary, Chinese pinyin with a
//   second reading for characters that have two, Korean as pronounced
//   (한국어→hangugeo), in McCune-Reischauer (보천보→pochonbo), and with
//   conventional surname and given-name spellings (이지은→leejieun), Thai RTGS and common
//   spellings, Indic with and without Hindi schwa deletion, Arabic and Hebrew
//   with their vowel letters read in context, and Tibetan Wylie.
//
// The index stores foldSegments(): the primary fold followed by each distinct
// alternate, separated by kSegmentSeparator. A query folds to the primary
// form only. The folding tables are compiled from src/search/fold/tables by
// tools/foldgen.py; see docs/search.md.
//
// Folding preserves spaces and punctuation: the matcher relies on word
// boundaries for its fzf bonuses and on substring semantics.

#include <QChar>
#include <QString>
#include <QVector>
#include <QtTypes>

namespace Search::Fold {

// Revision of the fold pipeline. cacheSignature() mixes it with a digest of
// the compiled tables; the on-disk search cache stores folded norms, so a
// change to either must invalidate it.
inline constexpr quint32 kVersion = 4;

// Separates the alternative folded forms of one field inside a search norm
// (the display text's fold, its alternate readings, then a sort name's).
// Anchored and fuzzy matching score each segment on its own, so `^the
// beatles$` still matches a norm that also carries "beatles, the". The fold
// drops control characters, so no folded needle can contain the separator and
// a plain substring never spans segments.
inline constexpr QChar kSegmentSeparator = QChar(0x1F);

// Folded text plus a map from each folded character back to the index of the
// source character it derived from. Used by highlighting to project match
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

// The primary fold with the source-index map.
FoldResult fold(const QString &src);

// The primary fold without the index map: the query-side hot path. Produces
// text identical to fold(src).text.
QString foldText(const QString &src);

// The primary fold followed by each distinct alternate reading, joined by
// kSegmentSeparator: the index-side form of a field.
QString foldSegments(const QString &src);

// Every distinct fold with its index map, primary first, in the order
// foldSegments() joins them.
QVector<FoldResult> foldAll(const QString &src);

// Identifies the fold's output for cache invalidation.
quint32 cacheSignature();

} // namespace Search::Fold
