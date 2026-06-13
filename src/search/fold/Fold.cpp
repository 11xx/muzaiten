#include "search/fold/Fold.h"

#include <QChar>
#include <QHash>

#include <algorithm>

namespace Search::Fold {

namespace {

// ---- step 3: combining diacritical marks ----------------------------------
// The same ranges the reference slug helper uses. After NFD decomposition many
// accented letters become base + combining mark(s); dropping the marks yields
// the plain base (é→e, ã→a, ş→s, ğ→g, ö→o, й→и, ά→α).
bool isCombiningMark(char16_t u)
{
    return (u >= 0x0300 && u <= 0x036F)   // Combining Diacritical Marks
        || (u >= 0x1AB0 && u <= 0x1AFF)   // ... Extended
        || (u >= 0x1DC0 && u <= 0x1DFF)   // ... Supplement
        || (u >= 0x20D0 && u <= 0x20FF)   // ... for Symbols
        || (u >= 0xFE20 && u <= 0xFE2F);  // Combining Half Marks
}

// ---- step 4: single code-point transliteration ----------------------------
// Keyed by the *lowercase, post-NFD-strip* code point, so only lowercase forms
// are listed. Entries are limited to letters NFD alone does not reduce to ASCII
// (decomposable accents like é/ş/ö are already handled by steps 2–3). An empty
// mapping deletes the character (Cyrillic hard/soft signs).
//
// One section per script — extend by adding rows here.
const QHash<char16_t, QString> &transliterationTable()
{
    static const QHash<char16_t, QString> table = [] {
        QHash<char16_t, QString> t;

        // Latin special letters / ligatures (incl. Turkish dotless ı).
        t[u'ß'] = QStringLiteral("ss");
        t[u'æ'] = QStringLiteral("ae");
        t[u'œ'] = QStringLiteral("oe");
        t[u'ø'] = QStringLiteral("o");
        t[u'å'] = QStringLiteral("a");
        t[u'þ'] = QStringLiteral("th");
        t[u'ð'] = QStringLiteral("d");
        t[u'đ'] = QStringLiteral("d");
        t[u'ł'] = QStringLiteral("l");
        t[u'ħ'] = QStringLiteral("h");
        t[u'ŧ'] = QStringLiteral("t");
        t[u'ŋ'] = QStringLiteral("ng");
        t[u'ĳ'] = QStringLiteral("ij");
        t[u'ŉ'] = QStringLiteral("n");
        t[u'ſ'] = QStringLiteral("s");
        t[u'ƒ'] = QStringLiteral("f");
        t[u'ĸ'] = QStringLiteral("k");
        t[u'ı'] = QStringLiteral("i");   // Turkish dotless i (İ→i+dot is handled by NFD)
        t[u'ə'] = QStringLiteral("e");

        // Greek lowercase.
        t[u'α'] = QStringLiteral("a");  t[u'β'] = QStringLiteral("b");
        t[u'γ'] = QStringLiteral("g");  t[u'δ'] = QStringLiteral("d");
        t[u'ε'] = QStringLiteral("e");  t[u'ζ'] = QStringLiteral("z");
        t[u'η'] = QStringLiteral("e");  t[u'θ'] = QStringLiteral("th");
        t[u'ι'] = QStringLiteral("i");  t[u'κ'] = QStringLiteral("k");
        t[u'λ'] = QStringLiteral("l");  t[u'μ'] = QStringLiteral("m");
        t[u'ν'] = QStringLiteral("n");  t[u'ξ'] = QStringLiteral("x");
        t[u'ο'] = QStringLiteral("o");  t[u'π'] = QStringLiteral("p");
        t[u'ρ'] = QStringLiteral("r");  t[u'σ'] = QStringLiteral("s");
        t[u'ς'] = QStringLiteral("s");  t[u'τ'] = QStringLiteral("t");
        t[u'υ'] = QStringLiteral("y");  t[u'φ'] = QStringLiteral("ph");
        t[u'χ'] = QStringLiteral("ch"); t[u'ψ'] = QStringLiteral("ps");
        t[u'ω'] = QStringLiteral("o");

        // Cyrillic lowercase.
        t[u'а'] = QStringLiteral("a");   t[u'б'] = QStringLiteral("b");
        t[u'в'] = QStringLiteral("v");   t[u'г'] = QStringLiteral("g");
        t[u'д'] = QStringLiteral("d");   t[u'е'] = QStringLiteral("e");
        t[u'ё'] = QStringLiteral("e");   t[u'ж'] = QStringLiteral("zh");
        t[u'з'] = QStringLiteral("z");   t[u'и'] = QStringLiteral("i");
        t[u'й'] = QStringLiteral("i");   t[u'к'] = QStringLiteral("k");
        t[u'л'] = QStringLiteral("l");   t[u'м'] = QStringLiteral("m");
        t[u'н'] = QStringLiteral("n");   t[u'о'] = QStringLiteral("o");
        t[u'п'] = QStringLiteral("p");   t[u'р'] = QStringLiteral("r");
        t[u'с'] = QStringLiteral("s");   t[u'т'] = QStringLiteral("t");
        t[u'у'] = QStringLiteral("u");   t[u'ф'] = QStringLiteral("f");
        t[u'х'] = QStringLiteral("kh");  t[u'ц'] = QStringLiteral("ts");
        t[u'ч'] = QStringLiteral("ch");  t[u'ш'] = QStringLiteral("sh");
        t[u'щ'] = QStringLiteral("shch");
        t[u'ъ'] = QString();             t[u'ь'] = QString();
        t[u'ы'] = QStringLiteral("y");   t[u'э'] = QStringLiteral("e");
        t[u'ю'] = QStringLiteral("yu");  t[u'я'] = QStringLiteral("ya");

        return t;
    }();
    return table;
}

// ---- step 5: kana → romaji -------------------------------------------------
// Katakana is mapped onto hiragana code points first, so only hiragana is
// tabulated here. Yōon (きゃ→kya) and sokuon (っか→kka) are handled by the
// driver with one character of look-ahead.

constexpr char16_t kHiraSokuon   = 0x3063; // っ
constexpr char16_t kHiraSmallYa  = 0x3083; // ゃ
constexpr char16_t kHiraSmallYu  = 0x3085; // ゅ
constexpr char16_t kHiraSmallYo  = 0x3087; // ょ
constexpr char16_t kHiraLow      = 0x3041;
constexpr char16_t kHiraHigh     = 0x3096;
constexpr char16_t kKataLow      = 0x30A1;
constexpr char16_t kKataHigh     = 0x30F6;
constexpr char16_t kProlonged    = 0x30FC; // ー (shared by both kana)

bool isSmallYa(char16_t u) { return u == kHiraSmallYa || u == kHiraSmallYu || u == kHiraSmallYo; }

const QHash<char16_t, QString> &hiraganaTable()
{
    static const QHash<char16_t, QString> t = [] {
        QHash<char16_t, QString> m;
        auto add = [&](char16_t base, std::initializer_list<const char *> romaji) {
            char16_t c = base;
            for (const char *r : romaji) m[c++] = QString::fromLatin1(r);
        };
        // Small standalone vowels ぁぃぅぇぉ then あいうえお share readings.
        add(0x3041, {"a", "a", "i", "i", "u", "u", "e", "e", "o", "o"});
        add(0x304B, {"ka", "ga", "ki", "gi", "ku", "gu", "ke", "ge", "ko", "go"});
        add(0x3055, {"sa", "za", "shi", "ji", "su", "zu", "se", "ze", "so", "zo"});
        add(0x305F, {"ta", "da", "chi", "ji", "tsu", "tsu", "zu", "te", "de", "to", "do"});
        add(0x306A, {"na", "ni", "nu", "ne", "no"});
        add(0x306F, {"ha", "ba", "pa", "hi", "bi", "pi", "fu", "bu", "pu",
                     "he", "be", "pe", "ho", "bo", "po"});
        add(0x307E, {"ma", "mi", "mu", "me", "mo"});
        // や-row is non-contiguous (small kana interleave) — set explicitly.
        m[0x3083] = QStringLiteral("ya");  m[0x3084] = QStringLiteral("ya"); // ゃ や
        m[0x3085] = QStringLiteral("yu");  m[0x3086] = QStringLiteral("yu"); // ゅ ゆ
        m[0x3087] = QStringLiteral("yo");  m[0x3088] = QStringLiteral("yo"); // ょ よ
        add(0x3089, {"ra", "ri", "ru", "re", "ro"});
        m[0x308E] = QStringLiteral("wa");           // ゎ
        m[0x308F] = QStringLiteral("wa");           // わ
        m[0x3092] = QStringLiteral("wo");           // を
        m[0x3093] = QStringLiteral("n");            // ん
        m[0x3094] = QStringLiteral("vu");           // ゔ
        return m;
    }();
    return t;
}

// Consonant prefix used to build yōon (palatalized) syllables; combined with the
// small vowel's a/u/o. Hepburn digraphs: し→sh, ち→ch, じ/ぢ→j.
QString yoonPrefix(char16_t c)
{
    switch (c) {
    case 0x304D: return QStringLiteral("ky");  // き
    case 0x304E: return QStringLiteral("gy");  // ぎ
    case 0x3057: return QStringLiteral("sh");  // し
    case 0x3058: return QStringLiteral("j");   // じ
    case 0x3061: return QStringLiteral("ch");  // ち
    case 0x3062: return QStringLiteral("j");   // ぢ
    case 0x306B: return QStringLiteral("ny");  // に
    case 0x3072: return QStringLiteral("hy");  // ひ
    case 0x3073: return QStringLiteral("by");  // び
    case 0x3074: return QStringLiteral("py");  // ぴ
    case 0x307F: return QStringLiteral("my");  // み
    case 0x308A: return QStringLiteral("ry");  // り
    default:     return QString();
    }
}

QString smallVowel(char16_t u)
{
    switch (u) {
    case kHiraSmallYa: return QStringLiteral("a");
    case kHiraSmallYu: return QStringLiteral("u");
    case kHiraSmallYo: return QStringLiteral("o");
    default:           return QString();
    }
}

// Apply sokuon gemination to a freshly produced syllable romaji.
QString geminate(const QString &romaji)
{
    if (romaji.isEmpty()) return romaji;
    if (romaji.startsWith(QLatin1String("ch"))) return QLatin1Char('t') + romaji; // っち→tchi
    return romaji.at(0) + romaji;                                                  // っか→kka
}

// ---- step 5b: kanji/word reading dictionary --------------------------------
// Kanji readings are word-level, not derivable per character (三線 reads
// "sanshin", a fixed word reading), so they need a lookup. This is the bundled,
// hand-tuned starter table: surface (kanji/mixed) → hiragana reading. The
// dictionary pass runs *before* kana→romaji, so readings here are written in
// kana and romanized by the existing stage. Longest surface wins, so multi-kanji
// words override single-kanji fallbacks (三線 beats 三+線).
//
// This is a seed, expected to grow over time. Single-kanji entries pick the most
// common standalone reading — a heuristic that favors recall; a longer word
// entry always takes precedence. Per-track reading tags (a later slice) cover
// what the table misses. To extend: add rows below.
bool isKanji(char16_t u)
{
    return (u >= 0x3400 && u <= 0x9FFF)    // CJK Unified Ideographs (+ Ext A)
        || (u >= 0xF900 && u <= 0xFAFF);   // CJK Compatibility Ideographs
}

struct JapaneseDict {
    QHash<QString, QString> map;
    int maxKeyLen = 0;
};

const JapaneseDict &japaneseDict()
{
    static const JapaneseDict dict = [] {
        JapaneseDict d;
        auto add = [&](const char *surface, const char *reading) {
            const QString s = QString::fromUtf8(surface);
            d.map.insert(s, QString::fromUtf8(reading));
            d.maxKeyLen = std::max(d.maxKeyLen, static_cast<int>(s.size()));
        };
        // Multi-character words (checked first via longest-match).
        add("三線", "さんしん");  add("言葉", "ことば");  add("世界", "せかい");
        add("未来", "みらい");    add("物語", "ものがたり"); add("東京", "とうきょう");
        add("京都", "きょうと");  add("音楽", "おんがく");  add("時間", "じかん");
        add("約束", "やくそく");  add("季節", "きせつ");    add("記憶", "きおく");
        // Common single kanji — most-common standalone reading.
        add("花", "はな");  add("名", "な");    add("君", "きみ");  add("海", "うみ");
        add("空", "そら");  add("心", "こころ"); add("恋", "こい");  add("愛", "あい");
        add("夢", "ゆめ");  add("道", "みち");  add("風", "かぜ");  add("月", "つき");
        add("雨", "あめ");  add("桜", "さくら"); add("涙", "なみだ"); add("声", "こえ");
        add("光", "ひかり"); add("時", "とき");  add("歌", "うた");  add("星", "ほし");
        add("雪", "ゆき");  add("夜", "よる");  add("朝", "あさ");  add("色", "いろ");
        return d;
    }();
    return dict;
}

// ---- driver ----------------------------------------------------------------

struct Builder {
    QString text;
    QVector<int> srcIndex;
    bool withIndex = false;

    void push(const QString &s, int src)
    {
        text += s;
        if (withIndex) {
            for (qsizetype k = 0; k < s.size(); ++k) srcIndex.append(src);
        }
    }
    void push(QChar c, int src)
    {
        text += c;
        if (withIndex) srcIndex.append(src);
    }
};

// Stage A: NFD-decompose + lowercase + strip combining marks, per source code
// point, recording the originating source index for each surviving char.
void decomposeFold(const QString &src, QString &base, QVector<int> &baseSrc)
{
    const int n = static_cast<int>(src.size());
    base.reserve(n);
    baseSrc.reserve(n);
    for (int i = 0; i < n; ++i) {
        const QChar ch = src.at(i);
        const char16_t u = ch.unicode();

        if (u < 0x80) {                              // ASCII fast path
            base += (u >= 'A' && u <= 'Z') ? QChar(u + 0x20) : ch;
            baseSrc.append(i);
            continue;
        }
        if (ch.isHighSurrogate() && i + 1 < n && src.at(i + 1).isLowSurrogate()) {
            // Astral code point (e.g. CJK Ext-B kanji): pass both UTF-16 units
            // through, each mapped to its own source index so a match flags the
            // whole surrogate pair — otherwise the delegate bolds half a glyph.
            base += ch;            baseSrc.append(i);
            base += src.at(i + 1); baseSrc.append(i + 1);
            ++i;
            continue;
        }
        // Skip NFD for the kana blocks: it would split voiced kana (が→か+゙)
        // and the combining voiced mark is meaningful here, not a strippable
        // diacritic. Kana are romanized whole in the transliteration stage.
        if (u >= 0x3040 && u <= 0x30FF) {
            base += ch;
            baseSrc.append(i);
            continue;
        }
        const QString dec = QString(ch).normalized(QString::NormalizationForm_D).toLower();
        for (const QChar d : dec) {
            if (isCombiningMark(d.unicode())) continue;
            base += d;
            baseSrc.append(i);
        }
    }
}

// Stage A.5: replace dictionary surface forms (kanji words) with their kana
// reading, longest-match-first. Only a kanji start triggers a lookup, so non-CJK
// text is copied through untouched. Reading characters are spread evenly across
// the surface's source span so a romaji match highlights the underlying kanji
// (e.g. matching "sanshin" lights 三線, "san" lights 三).
void applyDictionary(const QString &base, const QVector<int> &baseSrc,
                     QString &out, QVector<int> &outSrc)
{
    const JapaneseDict &dict = japaneseDict();
    const int n = static_cast<int>(base.size());
    out.reserve(n);
    outSrc.reserve(n);
    for (int j = 0; j < n;) {
        if (isKanji(base.at(j).unicode())) {
            const int maxL = std::min(dict.maxKeyLen, n - j);
            QString reading;
            int matchedLen = 0;
            for (int L = maxL; L >= 1; --L) {
                const auto it = dict.map.constFind(base.mid(j, L));
                if (it != dict.map.constEnd()) { reading = it.value(); matchedLen = L; break; }
            }
            if (matchedLen > 0) {
                const int K = static_cast<int>(reading.size());
                for (int k = 0; k < K; ++k) {
                    out += reading.at(k);
                    outSrc.append(baseSrc.at(j + (k * matchedLen) / K));
                }
                j += matchedLen;
                continue;
            }
        }
        out += base.at(j);
        outSrc.append(baseSrc.at(j));
        ++j;
    }
}

} // namespace

static FoldResult foldImpl(const QString &src, bool withIndex)
{
    QString base;
    QVector<int> baseSrc;
    decomposeFold(src, base, baseSrc);

    // Substitute kanji/word readings before romanizing the resulting kana.
    {
        QString db;
        QVector<int> dbSrc;
        applyDictionary(base, baseSrc, db, dbSrc);
        base = std::move(db);
        baseSrc = std::move(dbSrc);
    }

    Builder out;
    out.withIndex = withIndex;
    out.text.reserve(base.size());

    const QHash<char16_t, QString> &table = transliterationTable();
    const QHash<char16_t, QString> &kana = hiraganaTable();

    bool pendingGemination = false;
    const int n = static_cast<int>(base.size());
    for (int j = 0; j < n; ++j) {
        char16_t c = base.at(j).unicode();
        const int srcIdx = baseSrc.at(j);

        // Normalize katakana onto hiragana code points (ア→あ, ヴ→ゔ).
        if (c >= kKataLow && c <= kKataHigh) {
            c -= 0x60;
        } else if (c == kProlonged) {
            continue; // long-vowel mark: drop
        }

        const bool isKana = c >= kHiraLow && c <= kHiraHigh;
        if (isKana) {
            if (c == kHiraSokuon) {            // っ — geminate the next syllable
                pendingGemination = true;
                continue;
            }
            QString romaji;
            // Yōon: -i syllable followed by a small ya/yu/yo.
            if (j + 1 < n) {
                const char16_t next0 = base.at(j + 1).unicode();
                const char16_t next = (next0 >= kKataLow && next0 <= kKataHigh) ? char16_t(next0 - 0x60) : next0;
                if (isSmallYa(next)) {
                    const QString prefix = yoonPrefix(c);
                    if (!prefix.isEmpty()) {
                        romaji = prefix + smallVowel(next);
                        ++j; // consume the small kana
                    }
                }
            }
            if (romaji.isEmpty()) {
                const auto it = kana.constFind(c);
                romaji = (it != kana.constEnd()) ? it.value() : QString(QChar(c));
            }
            if (pendingGemination) {
                romaji = geminate(romaji);
                pendingGemination = false;
            }
            out.push(romaji, srcIdx);
            continue;
        }

        // A stray sokuon with no following syllable: emit nothing meaningful.
        pendingGemination = false;

        const auto it = table.constFind(c);
        if (it != table.constEnd()) {
            out.push(it.value(), srcIdx);
        } else {
            out.push(base.at(j), srcIdx);
        }
    }

    return {std::move(out.text), std::move(out.srcIndex)};
}

FoldResult fold(const QString &src)
{
    return foldImpl(src, /*withIndex=*/true);
}

QString foldText(const QString &src)
{
    return foldImpl(src, /*withIndex=*/false).text;
}

} // namespace Search::Fold
