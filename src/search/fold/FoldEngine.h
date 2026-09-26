#pragma once

// Internals of the search fold: the compiled tables and the script handlers.
//
// tools/foldgen.py precomputes every code point's primary fold, so the
// runtime never normalizes. An entry is 32 bits:
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

enum class GreekPairKind : std::uint8_t { Plain, Initial, Voicing };
struct GreekPair { char16_t first; char16_t second; Str a; Str b; GreekPairKind kind; };
struct GreekStyle {
    Str letter[25];         // α (U+03B1) through ω (U+03C9)
    GreekPair pair[24];
    std::uint8_t pairCount;
    Str frontGamma;
};

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
extern const Str kPinyin[];
extern const Word kJapaneseWords[];
extern const std::size_t kJapaneseWordCount;
extern const int kJapaneseLongest;
extern const Word kChineseWords[];
extern const std::size_t kChineseWordCount;
extern const int kChineseLongest;
extern const GreekStyle kGreekStyles[];
extern const AlphaRule kAlphaRules[];
extern const CyrillicStyle kCyrillicStyles[];
extern const AlphaRange kArmenianEastern;
extern const AlphaRange kArmenianWestern;
extern const AlphaRange kGerman;
extern const char16_t kPool[];
extern const std::size_t kPoolSize;

inline QStringView view(Str s) { return QStringView(kPool + s.off, qsizetype(s.len)); }

std::uint32_t astralEntry(char32_t cp);
inline std::uint32_t entryFor(char32_t cp) { return cp < 0x10000 ? kBmp[cp] : astralEntry(cp); }

// The code points a handler reads for `cp`, when its entry has the decomp bit.
QStringView decompOf(char32_t cp);

// The rule for `first` followed by `second` (0 for a single letter) in `range`.
const AlphaRule *findRule(AlphaRange range, char32_t first, char32_t second);

// ---- output --------------------------------------------------------------------

// Folded text under construction, with the source index of every unit when
// the caller wants highlight positions.
class Out {
public:
    explicit Out(bool withIndex, qsizetype hint = 0);

    void put(char16_t c, int src)
    {
        if (m_len == m_cap) grow(1);
        m_data[m_len++] = c;
        if (m_withIndex) m_src.append(src);
    }
    void put(QStringView s, int src)
    {
        const qsizetype n = s.size();
        if (m_len + n > m_cap) grow(n);
        for (qsizetype k = 0; k < n; ++k) m_data[m_len + k] = s.utf16()[k];
        m_len += n;
        if (m_withIndex) m_src.insert(m_src.size(), n, src);
    }
    void put(Str s, int src) { put(view(s), src); }
    void putCodePoint(char32_t cp, int src);
    // ASCII s[0, n): lowercased, controls other than whitespace dropped. Unit k
    // maps to src + k, or to src throughout when `sequential` is false.
    void putAscii(const char16_t *s, qsizetype n, int src, bool sequential);

    qsizetype size() const { return m_len; }
    QStringView text() const { return QStringView(m_data, m_len); }
    char16_t last() const { return m_len > 0 ? m_data[m_len - 1] : char16_t(0); }

    QString takeText();
    QList<int> takeIndex() { return std::move(m_src); }

private:
    void grow(qsizetype extra);

    QString m_text;
    char16_t *m_data = nullptr;
    qsizetype m_len = 0;
    qsizetype m_cap = 0;
    QList<int> m_src;
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
void foldGerman(const Unit *units, qsizetype n, Out &out);

} // namespace Search::Fold::Engine
