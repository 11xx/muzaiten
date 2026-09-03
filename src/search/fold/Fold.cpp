#include "search/fold/Fold.h"

#include <QByteArray>
#include <QChar>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QString>

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

using CharTable = QHash<char16_t, QString>;

struct JapaneseDict {
    QHash<QString, QString> map;
    int maxKeyLen = 0;
};

struct FoldTables {
    CharTable transliteration;
    CharTable kana;
    CharTable yoonPrefix;
    CharTable smallVowel;
    JapaneseDict dictionary;
};

CharTable charTableFromJson(const QJsonValue &value)
{
    CharTable table;
    const QJsonObject object = value.toObject();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!it.value().isString() || it.key().size() != 1) continue;
        table.insert(static_cast<char16_t>(it.key().at(0).unicode()), it.value().toString());
    }
    return table;
}

// The tables travel inside this translation unit, so every binary that
// compiles Fold.cpp carries them and nothing has to register a resource.
constexpr unsigned char kFoldTablesJson[] = {
#embed "fold_tables.json"
};

FoldTables loadFoldTables()
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromRawData(reinterpret_cast<const char *>(kFoldTablesJson), sizeof kFoldTablesJson),
        &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        qFatal("fold_tables.json is not a JSON object: %s", qPrintable(parseError.errorString()));
    }

    FoldTables tables;
    const QJsonObject root = document.object();
    tables.transliteration = charTableFromJson(root.value(QStringLiteral("transliteration")));
    tables.kana = charTableFromJson(root.value(QStringLiteral("kana")));
    tables.yoonPrefix = charTableFromJson(root.value(QStringLiteral("yoon_prefix")));
    tables.smallVowel = charTableFromJson(root.value(QStringLiteral("small_vowel")));

    const QJsonObject dictionary = root.value(QStringLiteral("kanji")).toObject();
    for (auto it = dictionary.constBegin(); it != dictionary.constEnd(); ++it) {
        if (it.key().isEmpty() || !it.value().isString()) continue;
        tables.dictionary.map.insert(it.key(), it.value().toString());
        tables.dictionary.maxKeyLen = std::max(
            tables.dictionary.maxKeyLen, static_cast<int>(it.key().size()));
    }
    return tables;
}

const FoldTables &foldTables()
{
    static const FoldTables tables = loadFoldTables();
    return tables;
}

const CharTable &transliterationTable()
{
    return foldTables().transliteration;
}

// ---- step 5: kana → romaji -------------------------------------------------
// Katakana is mapped onto hiragana code points first, so only hiragana is
// tabulated here. Yōon (きゃ→kya) and sokuon (っか→kka) are handled by the
// driver with one character of look-ahead.

constexpr char16_t kHiraSokuon   = 0x3063; // っ
constexpr char16_t kHiraLow      = 0x3041;
constexpr char16_t kHiraHigh     = 0x3096;
constexpr char16_t kKataLow      = 0x30A1;
constexpr char16_t kKataHigh     = 0x30F6;
constexpr char16_t kProlonged    = 0x30FC; // ー (shared by both kana)
constexpr char16_t kKanjiIteration       = 0x3005; // 々
constexpr char16_t kHiraIteration        = 0x309D; // ゝ
constexpr char16_t kHiraVoicedIteration  = 0x309E; // ゞ
constexpr char16_t kKataIteration        = 0x30FD; // ヽ
constexpr char16_t kKataVoicedIteration  = 0x30FE; // ヾ

bool isSmallYa(char16_t u) { return foldTables().smallVowel.contains(u); }

bool isKanaCharacter(char16_t u)
{
    return (u >= kHiraLow && u <= kHiraHigh)
        || (u >= kKataLow && u <= kKataHigh);
}

QString voicedKana(char16_t c)
{
    const QString composed = (QString(QChar(c)) + QChar(0x3099))
                                 .normalized(QString::NormalizationForm_C);
    return composed.size() == 1 && isKanaCharacter(composed.at(0).unicode())
        ? composed
        : QString(QChar(c));
}

const CharTable &hiraganaTable()
{
    return foldTables().kana;
}

// Consonant prefix used to build yōon (palatalized) syllables; combined with the
// small vowel's a/u/o. Hepburn digraphs: し→sh, ち→ch, じ/ぢ→j.
QString yoonPrefix(char16_t c)
{
    const auto it = foldTables().yoonPrefix.constFind(c);
    return it == foldTables().yoonPrefix.constEnd() ? QString() : it.value();
}

QString smallVowel(char16_t u)
{
    const auto it = foldTables().smallVowel.constFind(u);
    return it == foldTables().smallVowel.constEnd() ? QString() : it.value();
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
// what the table misses. Rows are kept in fold_tables.json.
bool isKanji(char16_t u)
{
    return (u >= 0x3400 && u <= 0x9FFF)    // CJK Unified Ideographs (+ Ext A)
        || (u >= 0xF900 && u <= 0xFAFF);   // CJK Compatibility Ideographs
}

// Expand Japanese iteration marks before dictionary lookup and kana
// romanization. A repeated unit inherits the mark's source position so a
// match in the expansion highlights the mark that supplied it.
void expandIterationMarks(const QString &base, const QVector<int> &baseSrc,
                          QString &out, QVector<int> &outSrc)
{
    const int n = static_cast<int>(base.size());
    out.reserve(n);
    outSrc.reserve(n);
    for (int j = 0; j < n; ++j) {
        const char16_t c = base.at(j).unicode();
        const int srcIdx = baseSrc.at(j);
        if (c == kKanjiIteration && j > 0 && isKanji(base.at(j - 1).unicode())) {
            out += base.at(j - 1);
            outSrc.append(srcIdx);
            continue;
        }

        const bool voiced = c == kHiraVoicedIteration || c == kKataVoicedIteration;
        const bool plain = c == kHiraIteration || c == kKataIteration;
        if ((plain || voiced) && j > 0 && isKanaCharacter(base.at(j - 1).unicode())) {
            const QString repeated = voiced ? voicedKana(base.at(j - 1).unicode())
                                            : QString(base.at(j - 1));
            out += repeated;
            for (qsizetype k = 0; k < repeated.size(); ++k) outSrc.append(srcIdx);
            continue;
        }

        out += base.at(j);
        outSrc.append(srcIdx);
    }
}

const JapaneseDict &japaneseDict()
{
    return foldTables().dictionary;
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

bool isKanaBlock(char16_t u)
{
    return u >= 0x3040 && u <= 0x30FF;
}

bool isHalfWidthKanaBase(char16_t u)
{
    return u >= 0xFF61 && u <= 0xFF9D;
}

bool isHalfWidthVoicedMark(char16_t u)
{
    return u == 0xFF9E || u == 0xFF9F;
}

void appendPostNfkc(const QString &normalized, int srcIndex,
                    QString &base, QVector<int> &baseSrc)
{
    for (const QChar ch : normalized) {
        if (isKanaBlock(ch.unicode())) {
            base += ch;
            baseSrc.append(srcIndex);
            continue;
        }
        const QString dec = QString(ch).normalized(QString::NormalizationForm_D).toLower();
        for (const QChar d : dec) {
            if (isCombiningMark(d.unicode())) continue;
            base += d;
            baseSrc.append(srcIndex);
        }
    }
}

void appendNfkcUnit(const QString &unit, int srcIndex,
                   QString &base, QVector<int> &baseSrc)
{
    appendPostNfkc(unit.normalized(QString::NormalizationForm_KC).toLower(),
                   srcIndex, base, baseSrc);
}

// Stage A: NFKC compatibility normalization, then NFD decomposition, lowercase,
// and combining-mark stripping, per source unit. Every surviving char records
// the source index of the unit that produced it.
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
            // Astral code point (e.g. CJK Ext-B kanji): preserve the UTF-16
            // mapping so a match flags the whole surrogate pair.
            const QString unit = src.mid(i, 2);
            const QString normalized = unit.normalized(QString::NormalizationForm_KC).toLower();
            if (normalized == unit) {
                base += normalized.at(0); baseSrc.append(i);
                base += normalized.at(1); baseSrc.append(i + 1);
            } else {
                appendPostNfkc(normalized, i, base, baseSrc);
            }
            ++i;
            continue;
        }

        // U+FF9E/U+FF9F compose with the preceding half-width kana only when
        // normalized as one unit. Consuming the pair prevents a bare voiced
        // mark from being mistaken for a separate kana.
        if (isHalfWidthKanaBase(u) && i + 1 < n
            && isHalfWidthVoicedMark(src.at(i + 1).unicode())) {
            appendNfkcUnit(src.mid(i, 2), i, base, baseSrc);
            ++i;
            continue;
        }
        appendNfkcUnit(QString(ch), i, base, baseSrc);
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

TableStats tableStats()
{
    const FoldTables &tables = foldTables();
    int longestKeyLen = 0;
    for (auto it = tables.dictionary.map.constBegin();
         it != tables.dictionary.map.constEnd(); ++it) {
        longestKeyLen = std::max(longestKeyLen, static_cast<int>(it.key().size()));
    }
    return {
        static_cast<int>(tables.transliteration.size()),
        static_cast<int>(tables.kana.size()),
        static_cast<int>(tables.yoonPrefix.size()),
        static_cast<int>(tables.smallVowel.size()),
        static_cast<int>(tables.dictionary.map.size()),
        tables.dictionary.maxKeyLen,
        longestKeyLen,
    };
}

static FoldResult foldImpl(const QString &src, bool withIndex, bool romanizeCjk)
{
    QString base;
    QVector<int> baseSrc;
    decomposeFold(src, base, baseSrc);

    // Substitute kanji/word readings before romanizing the resulting kana. Both
    // are skipped for the cheap "basic" fold (kana/kanji then pass through).
    if (romanizeCjk) {
        QString expanded;
        QVector<int> expandedSrc;
        expandIterationMarks(base, baseSrc, expanded, expandedSrc);
        base = std::move(expanded);
        baseSrc = std::move(expandedSrc);

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
        if (romanizeCjk && c >= kKataLow && c <= kKataHigh) {
            c -= 0x60;
        } else if (romanizeCjk && c == kProlonged) {
            continue; // long-vowel mark: drop
        }

        const bool isKana = romanizeCjk && c >= kHiraLow && c <= kHiraHigh;
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

FoldResult fold(const QString &src, bool romanizeCjk)
{
    return foldImpl(src, /*withIndex=*/true, romanizeCjk);
}

QString foldText(const QString &src, bool romanizeCjk)
{
    return foldImpl(src, /*withIndex=*/false, romanizeCjk).text;
}

} // namespace Search::Fold
