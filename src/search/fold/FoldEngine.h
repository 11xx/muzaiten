#pragma once

// Internals of the search fold: the compiled tables and the script handlers.
//
// tools/foldgen.py precomputes every code point's primary fold, so the
// runtime applies no per-character normalization; it only puts text into
// NFC first when NFC would change it (decomposed input, marks out of order). An entry is 32 bits:
//
//   bits  0-2   kind      Keep, Char, Pool, Drop, Script or Expand
//   bits  3-7   script    which handler reads the code point in an alternate pass
//   bit   8     decomp    a handler reads kDecomp's code points instead of this one
//   bits  9-14  length    of the pooled string (Pool, Expand)
//   bits 15-31  payload   the replacement character (Char), the pool offset
//                         (Pool, Expand) or handler data (Script)
//
// Keep emits the code point unchanged, Char and Pool emit the replacement,
// Drop emits nothing, and Expand folds the pooled compatibility expansion in
// place of the code point. Script code points are always read by their
// script's handler (kana, Hangul, Armenian and the kanji iteration mark need
// their neighbours even for the primary fold). In an alternate pass, a code
// point whose script has that many reading styles goes to the handler too.

#include <QList>
#include <QString>
#include <QStringView>
#include <QVarLengthArray>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace Search::Fold::Engine {

enum class Kind : std::uint8_t { Keep, Char, Pool, Drop, Script, Expand };

// Must match SCRIPTS in tools/foldgen.py.
enum class Script : std::uint8_t {
    None, Inherited, Latin, LatinAlt, Greek, Cyrillic, CyrillicUk, CyrillicBe, CyrillicSr,
    Armenian, Georgian, Hebrew, Arabic, Syriac, Thaana, Brahmic, Thai, Lao, Tibetan, Kana, Han,
    Hangul,
};

inline constexpr Kind kindOf(std::uint32_t e) { return Kind(e & 7u); }
inline constexpr Script scriptOf(std::uint32_t e) { return Script((e >> 3) & 31u); }
inline constexpr bool hasDecomp(std::uint32_t e) { return (e >> 8) & 1u; }
inline constexpr std::uint32_t lengthOf(std::uint32_t e) { return (e >> 9) & 63u; }
inline constexpr std::uint32_t payloadOf(std::uint32_t e) { return e >> 15; }

// Scripts whose language variants share one handler.
inline constexpr Script familyOf(Script s)
{
    switch (s) {
    case Script::CyrillicUk:
    case Script::CyrillicBe:
    case Script::CyrillicSr:
        return Script::Cyrillic;
    default:
        return s;
    }
}

inline constexpr std::uint32_t maskOf(Script s) { return 1u << unsigned(s); }

// ---- compiled tables (FoldData.cpp) -----------------------------------------

struct Str { std::uint32_t off; std::uint32_t len; };
struct CodeRange { char32_t first; char32_t last; };
struct AstralEntry { char32_t cp; std::uint32_t entry; };
struct Decomp { char32_t cp; Str seq; };
struct Word { Str key; Str value; };
struct StarterComposition { char32_t first; char32_t second; char32_t composite; };
struct MarkLedStarter { char32_t cp; int lastClass; };

enum class KanaKind : std::uint8_t {
    None, Syllable, Moraic, Small, Sokuon, Long, Iteration, VoicedIteration, Dakuten, Handakuten,
};

struct KanaInfo {
    KanaKind kind;
    Str wapuro;
    Str hepburn;
    Str consonant;          // the reading without its vowel (k, sh, ts)
    Str combine;            // consonant kept before a small vowel after an -u syllable (う w)
    char16_t vowel;         // final vowel of the reading, 0 when it has none
    std::uint16_t voiced;   // kana id with a dakuten, 0 when there is none
    std::uint16_t semivoiced;
    char16_t hira;          // hiragana code point, 0 for katakana-only kana
};

struct HangulStyle {
    Str initial[19];
    Str voicedInitial[19];  // McCune-Reischauer between voiced sounds
    Str vowel[21];
    Str coda[7];            // k n t l m p ng
    Str lateral;            // ㄹ read as l after l or n
};

struct HanAstralPinyin { char32_t cp; std::uint16_t pinyin; };
struct HanSecondPinyin { char32_t cp; std::uint16_t pinyin; };

enum class GreekPairKind : std::uint8_t { Plain, Initial, Voicing };
struct GreekPair { char16_t first; char16_t second; Str a; Str b; GreekPairKind kind; };
struct GreekStyle {
    Str letter[25];         // α (U+03B1) through ω (U+03C9)
    GreekPair pair[24];
    std::uint8_t pairCount;
    Str frontGamma;
};

// Space is the space Thai and Lao write before the repetition mark ๆ; the
// tables never assign it.
enum class ThaiRole : std::uint8_t { None, Consonant, Pre, Above, Post, Tone, Silencer, Repeat, Letter, Cluster, Space };
enum class ThaiVowel : std::uint8_t {
    A, I, II, UE, UEE, U, UU, E, AE, O, OO, IA, UEA, UA, OE, OEI, AI, AO, AM, ImplicitA, ImplicitO, RUE, LUE, RI,
    Count,
};
// A Thai or Lao character: its role in a syllable, the Thai letter it
// behaves like, and its [style 1, style 2] spellings.
struct ThaiChar { ThaiRole role; ThaiVowel vowel; char16_t thai; Str initial[2]; Str final[2]; };

enum class TibetanRole : std::uint8_t { None, Base, Subjoined, Vowel, Coda, Tsheg };
struct TibetanChar { TibetanRole role; Str roman; };

enum class AbjadRole : std::uint8_t {
    None, Consonant, Alef, Ain, Waw, Yeh, Maksura, Marbuta, Hamza, VowelLetter, He, Shin,
    Vowel, Shadda, Sukun, Dagesh, ShinDot, SinDot, Rafe, Geresh, Sheva,
};
// An Arabic, Hebrew, Syriac or Thaana code point: its role and its spelling in
// reading styles 1-3, with a Hebrew letter's spelling without a hardening dot
// and after a geresh.
struct AbjadChar { AbjadRole role; Str value[3]; Str soft; Str geresh; };
struct AbjadBlock { char32_t first; char32_t last; std::uint32_t offset; };

enum class BrahmicRole : std::uint8_t {
    None, Consonant, Vowel, Sign, Virama, Anusvara, Candrabindu, Visarga, Nukta, Medial, Final, Geminate,
};
// A Brahmic code point: its role, a consonant's inherent vowel, its spelling
// (a consonant without its vowel, or a vowel), an alternate spelling (a
// consonant in the ee/oo reading, a medial in the full one), and a
// consonant's spelling with a nukta.
struct BrahmicChar { BrahmicRole role; char16_t inherent; Str roman; Str alt; Str nukta; };
// How a script treats the inherent vowel and the anusvara (see brahmic.json).
enum class BrahmicReading : std::uint8_t { Plain, North, Bengali, South, Khmer };
struct BrahmicBlock { char32_t first; char32_t last; std::uint32_t offset; BrahmicReading reading; };
struct BrahmicPair { char32_t first; char32_t second; Str roman; };

struct AlphaRule { char32_t first; char32_t second; Str initial; Str medial; };
struct AlphaRange { std::uint32_t begin; std::uint32_t count; };
struct CyrillicStyle { AlphaRange map; AlphaRange uk; AlphaRange be; AlphaRange sr; };

extern const std::uint32_t kDataDigest;
extern const std::uint32_t kBmp[0x10000];
extern const AstralEntry kAstral[];
extern const std::size_t kAstralCount;
extern const CodeRange kAstralHan[];
extern const std::size_t kAstralHanCount;
extern const Decomp kDecomp[];
extern const std::size_t kDecompCount;
extern const KanaInfo kKana[];
extern const HangulStyle kHangulRr;
extern const HangulStyle kHangulMr;
extern const std::uint16_t kHanPinyinBmp[0xA000 - 0x3400];
extern const HanAstralPinyin kHanPinyinAstral[];
extern const std::size_t kHanPinyinAstralCount;
// Sparse readings for authored ideographs with a second distinct pinyin.
extern const HanSecondPinyin kHanSecondPinyin[];
extern const std::size_t kHanSecondPinyinCount;
extern const Str kPinyin[];
extern const Word kJapaneseWords[];
extern const std::size_t kJapaneseWordCount;
extern const int kJapaneseLongest;
extern const Word kChineseWords[];
extern const std::size_t kChineseWordCount;
extern const int kChineseLongest;
extern const GreekStyle kGreekStyles[];
extern const TibetanChar kTibetanChars[0x100];
extern const std::uint32_t kComposableBmp[0x10000 / 32];
extern const char32_t kComposableAstral[];
extern const std::size_t kComposableAstralCount;
extern const StarterComposition kStarterCompositions[];
extern const std::size_t kStarterCompositionCount;
extern const MarkLedStarter kMarkLedStarters[];
extern const std::size_t kMarkLedStarterCount;
extern const AbjadBlock kAbjadBlocks[];
extern const std::size_t kAbjadBlockCount;
extern const AbjadChar kAbjadChars[];
extern const Str kArabicArticle[3];
extern const Str kArabicWaw[3];
extern const Str kArabicYeh[3];
extern const Str kArabicMarbuta[3];
extern const Str kHebrewFinalHe[2];
extern const BrahmicBlock kBrahmicBlocks[];
extern const std::size_t kBrahmicBlockCount;
extern const BrahmicChar kBrahmicChars[];
extern const BrahmicPair kBrahmicMerges[];
extern const std::size_t kBrahmicMergeCount;
extern const BrahmicPair kBrahmicClusters[];
extern const std::size_t kBrahmicClusterCount;
extern const ThaiChar kThaiChars[128];
extern const ThaiChar kLaoChars[128];
extern const Str kThaiVowels[][2];
extern const Word kThaiWords[];
extern const std::size_t kThaiWordCount;
extern const int kThaiLongest;
extern const AlphaRule kAlphaRules[];
extern const CyrillicStyle kCyrillicStyles[];
extern const AlphaRange kArmenianEastern;
extern const AlphaRange kArmenianWestern;
extern const AlphaRange kGerman;
extern const AlphaRange kUmlautAsI;
extern const char16_t kPool[];
extern const std::size_t kPoolSize;

inline QStringView view(Str s) { return QStringView(kPool + s.off, qsizetype(s.len)); }

std::uint32_t astralEntry(char32_t cp);
inline std::uint32_t entryFor(char32_t cp) { return cp < 0x10000 ? kBmp[cp] : astralEntry(cp); }

// The code points a handler reads for `cp`, when its entry has the decomp bit.
QStringView decompOf(char32_t cp);

// NFC may change text containing `cp`: it composes canonically with the code
// point before it (a combining accent, a kana voicing mark, a Hangul vowel or
// final, the second half of an Indic two-part vowel), or NFC replaces it (an
// excluded composition such as U+0344, a singleton).
bool normalizes(char32_t cp);

// The character NFC composes from the starters `first` and `second` (combining
// class 0; Hangul jamo, the halves of a two-part vowel), or 0 when they stay
// apart.
char32_t composeStarters(char32_t first, char32_t second);

// For a starter whose decomposition is marks on the base before it (U+0F73 =
// U+0F71 U+0F72), the combining class of the mark it ends in; otherwise 0.
int markLedClass(char32_t cp);

// The rule for `first` followed by `second` (0 for a single letter) in `range`.
const AlphaRule *findRule(AlphaRange range, char32_t first, char32_t second);

// The word spelled `key` in a table sorted by key, or nullptr.
const Word *findWord(const Word *words, std::size_t count, QStringView key);

// ---- output --------------------------------------------------------------------

// Folded text under construction, with the source index of every unit when
// the caller wants highlight positions.
class Out {
public:
    explicit Out(bool withIndex, qsizetype hint = 0);

    void put(char16_t c, int src)
    {
        if (m_len == m_cap) grow(1);
        m_data[m_len] = c;
        if (m_withIndex) m_src[m_len] = src;
        ++m_len;
    }
    void put(QStringView s, int src)
    {
        const qsizetype n = s.size();
        if (m_len + n > m_cap) grow(n);
        if (m_withIndex) {
            for (qsizetype k = 0; k < n; ++k) {
                m_data[m_len + k] = s.utf16()[k];
                m_src[m_len + k] = src;
            }
        } else {
            for (qsizetype k = 0; k < n; ++k) m_data[m_len + k] = s.utf16()[k];
        }
        m_len += n;
    }
    void put(Str s, int src) { put(view(s), src); }
    void putCodePoint(char32_t cp, int src);
    // ASCII s[0, n): lowercased, controls other than whitespace dropped. Unit k
    // maps to src + k, or to src throughout when `sequential` is false.
    void putAscii(const char16_t *s, qsizetype n, int src, bool sequential);

    qsizetype size() const { return m_len; }
    QStringView text() const { return QStringView(m_data, m_len); }
    char16_t last() const { return m_len > 0 ? m_data[m_len - 1] : char16_t(0); }

    // The folded text and, when built with an index, its source indices;
    // leaves the builder empty.
    std::pair<QString, QList<int>> take();

private:
    void grow(qsizetype extra);

    QString m_text;
    char16_t *m_data = nullptr;
    qsizetype m_len = 0;
    qsizetype m_cap = 0;
    std::unique_ptr<int[]> m_src;
    bool m_withIndex;
};

// ---- handlers --------------------------------------------------------------------

struct Unit {
    char32_t cp;
    std::uint32_t entry;
    int src;
};
using Units = QVarLengthArray<Unit, 64>;

// What a handler knows about the string beyond its run.
struct Context {
    std::uint32_t mask = 0;   // every script in the string (Script bits)
    char32_t before = 0;      // the code point just before the run, 0 at the start
};

// Emits a unit's primary fold (for a handler falling back to it).
void emitPrimary(const Unit &unit, Out &out);

// Number of reading styles of a script family: the primary fold plus its
// alternates. Pass k reads a family with style k when k is below its count.
int styleCount(Script family);

void foldKana(const Unit *units, qsizetype n, int style, Out &out);
void foldHan(const Unit *units, qsizetype n, int style, const Context &ctx, Out &out);
void foldHangul(const Unit *units, qsizetype n, int style, Out &out);
void foldGreek(const Unit *units, qsizetype n, int style, Out &out);
void foldCyrillic(const Unit *units, qsizetype n, int style, const Context &ctx, Out &out);
void foldArmenian(const Unit *units, qsizetype n, int style, Out &out);
void foldLatinAlt(const Unit *units, qsizetype n, int style, Out &out);
void foldThai(const Unit *units, qsizetype n, int style, Out &out);
void foldBrahmic(const Unit *units, qsizetype n, int style, Out &out);
void foldArabic(const Unit *units, qsizetype n, int style, Out &out);
void foldHebrew(const Unit *units, qsizetype n, int style, Out &out);
void foldSyriac(const Unit *units, qsizetype n, Out &out);
void foldThaana(const Unit *units, qsizetype n, Out &out);
void foldTibetan(const Unit *units, qsizetype n, int style, Out &out);

} // namespace Search::Fold::Engine
