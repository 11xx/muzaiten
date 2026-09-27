#include "search/fold/Fold.h"

#include "search/fold/FoldEngine.h"

#include <QVarLengthArray>

#include <algorithm>
#include <array>

namespace Search::Fold {

namespace Engine {

Out::Out(bool withIndex, qsizetype hint)
    : m_withIndex(withIndex)
{
    if (hint > 0) grow(hint);
}

void Out::grow(qsizetype extra)
{
    const qsizetype cap = std::max(m_len + extra, m_cap * 2 + 16);
    std::unique_ptr<int[]> src;
    if (m_withIndex) {
        src = std::make_unique_for_overwrite<int[]>(cap);
        if (m_len > 0) std::copy_n(m_src.get(), m_len, src.get());
    }
    m_text.resize(cap);
    m_data = reinterpret_cast<char16_t *>(m_text.data());
    if (m_withIndex) m_src = std::move(src);
    m_cap = cap;
}

namespace {

inline bool isDroppedAscii(char16_t c)
{
    return (c < 0x20 && (c < 0x09 || c > 0x0D)) || c == 0x7F;
}

inline char16_t lowerAscii(char16_t c)
{
    return (c >= u'A' && c <= u'Z') ? char16_t(c + 0x20) : c;
}

} // namespace

void Out::putAscii(const char16_t *s, qsizetype n, int src, bool sequential)
{
    if (m_len + n > m_cap) grow(n);
    char16_t *d = m_data + m_len;
    bool dropped = false;
    for (qsizetype k = 0; k < n; ++k) dropped |= isDroppedAscii(s[k]);
    if (!dropped) {
        for (qsizetype k = 0; k < n; ++k) {
            d[k] = lowerAscii(s[k]);
            if (m_withIndex) m_src[m_len + k] = sequential ? src + int(k) : src;
        }
        m_len += n;
        return;
    }
    qsizetype written = 0;
    for (qsizetype k = 0; k < n; ++k) {
        if (isDroppedAscii(s[k])) continue;
        d[written] = lowerAscii(s[k]);
        if (m_withIndex) m_src[m_len + written] = sequential ? src + int(k) : src;
        ++written;
    }
    m_len += written;
}

void Out::putCodePoint(char32_t cp, int src)
{
    if (cp < 0x10000) {
        put(char16_t(cp), src);
        return;
    }
    put(QChar::highSurrogate(cp), src);
    put(QChar::lowSurrogate(cp), src);
}

std::pair<QString, QList<int>> Out::take()
{
    QList<int> index;
    if (m_withIndex) index = QList<int>(m_src.get(), m_src.get() + m_len);
    m_text.truncate(m_len);
    m_data = nullptr;
    m_src.reset();
    m_len = m_cap = 0;
    return {std::move(m_text), std::move(index)};
}

int styleCount(Script family)
{
    switch (family) {
    case Script::Han:
        return 5;
    case Script::Brahmic:
    case Script::Arabic:
        return 4;
    case Script::Hebrew:
        return 3;
    case Script::Hangul:
        return 4;
    case Script::Syriac:
    case Script::Thaana:
        return 2;
    case Script::Greek:
    case Script::Cyrillic:
    case Script::Thai:
    case Script::Lao:
    case Script::LatinAlt:
    case Script::Tibetan:
    case Script::Kana:
    case Script::Armenian:
        return 3;
    default:
        return 1;
    }
}

} // namespace Engine

namespace {

using namespace Engine;

struct Decoded {
    char32_t cp;
    int width;
};

inline Decoded decode(const char16_t *s, qsizetype n, qsizetype i)
{
    const char16_t c = s[i];
    if (QChar::isHighSurrogate(c) && i + 1 < n && QChar::isLowSurrogate(s[i + 1])) {
        return {QChar::surrogateToUcs4(c, s[i + 1]), 2};
    }
    return {c, 1};
}

inline char32_t previousCodePoint(const char16_t *s, qsizetype i)
{
    if (i <= 0) return 0;
    const char16_t c = s[i - 1];
    if (QChar::isLowSurrogate(c) && i >= 2 && QChar::isHighSurrogate(s[i - 2])) {
        return QChar::surrogateToUcs4(s[i - 2], c);
    }
    return c;
}

// Printable ASCII and whitespace fold to a lowercase copy, which is `src`
// itself when it has no capitals. Returns false for anything else.
bool foldAscii(const QString &src, QString &folded)
{
    const qsizetype n = src.size();
    const char16_t *s = reinterpret_cast<const char16_t *>(src.utf16());
    bool other = false;
    bool upper = false;
    for (qsizetype i = 0; i < n; ++i) {
        const char16_t c = s[i];
        other |= c >= 0x7F || (c < 0x20 && (c < 0x09 || c > 0x0D));
        upper |= c >= u'A' && c <= u'Z';
    }
    if (other) return false;
    if (!upper) {
        folded = src;
        return true;
    }
    folded = QString(n, Qt::Uninitialized);
    char16_t *d = reinterpret_cast<char16_t *>(folded.data());
    for (qsizetype i = 0; i < n; ++i) d[i] = lowerAscii(s[i]);
    return true;
}

// Canonically equivalent text folds alike: a base with marks that NFC would
// change (decomposed file names, NFD input, marks out of canonical order,
// excluded compositions) is put into NFC first. Composed carries that text
// and, for each of its units, the source index of the sequence it came from.
struct Composed {
    QString text;
    QVector<int> map;
};

bool needsComposition(const QString &src)
{
    const char16_t *s = reinterpret_cast<const char16_t *>(src.utf16());
    const qsizetype n = src.size();
    int previousClass = 0;
    for (qsizetype i = 0; i < n; ++i) {
        const char16_t c = s[i];
        if (c < 0x0300) {
            previousClass = 0;
            continue;
        }
        char32_t cp = c;
        if (QChar::isHighSurrogate(c) && i + 1 < n && QChar::isLowSurrogate(s[i + 1])) {
            cp = QChar::surrogateToUcs4(c, s[++i]);
        }
        if (normalizes(cp)) return true;
        const int cls = QChar::combiningClass(cp);
        if (cls != 0 && previousClass > cls) return true;  // marks out of canonical order
        previousClass = cls;
    }
    return false;
}

Composed compose(const QString &src)
{
    const char16_t *s = reinterpret_cast<const char16_t *>(src.utf16());
    const qsizetype n = src.size();
    Composed out;
    out.text.reserve(n);
    out.map.reserve(n);
    qsizetype i = 0;
    while (i < n) {
        const auto [first, firstWidth] = decode(s, n, i);
        qsizetype j = i + firstWidth;
        bool composable = normalizes(first);
        const int firstClass = QChar::combiningClass(first);
        int previousClass = firstClass != 0 ? firstClass : markLedClass(first);
        // The starter a following starter would compose with; a mark blocks it.
        char32_t starter = previousClass == 0 ? first : 0;
        while (j < n) {
            const auto [next, width] = decode(s, n, j);
            const int cls = QChar::combiningClass(next);
            if (cls != 0) {
                starter = 0;
                composable = composable || normalizes(next) || previousClass > cls;
                previousClass = cls;
            } else if (const int led = markLedClass(next)) {
                // Decomposed, it is marks on the base before it.
                starter = 0;
                composable = true;
                previousClass = led;
            } else {
                starter = composeStarters(starter, next);
                if (starter == 0) break;
                composable = true;
            }
            j += width;
        }
        const QStringView sequence(s + i, j - i);
        if (composable) {
            const QString nfc = sequence.toString().normalized(QString::NormalizationForm_C);
            out.text += nfc;
            out.map.insert(out.map.size(), nfc.size(), int(i));
        } else {
            out.text += sequence;
            for (qsizetype k = i; k < j; ++k) out.map.append(int(k));
        }
        i = j;
    }
    return out;
}

void remap(QVector<int> &index, const QVector<int> &map)
{
    for (int &k : index) k = map.at(k);
}

QVector<int> identityIndex(qsizetype n)
{
    QVector<int> index(n);
    for (qsizetype i = 0; i < n; ++i) index[i] = int(i);
    return index;
}

qsizetype collectRun(const char16_t *s, qsizetype n, qsizetype i, Script family, int srcOverride,
                     std::uint32_t &mask, Units &units)
{
    while (i < n) {
        const auto [cp, width] = decode(s, n, i);
        const std::uint32_t e = entryFor(cp);
        const Script script = scriptOf(e);
        if (familyOf(script) != family && script != Script::Inherited) {
            // Thai and Lao may write a space before the repetition mark, which
            // repeats the word before it (จริง ๆ).
            const bool beforeRepeat = (family == Script::Thai || family == Script::Lao) && cp == u' ' && i + 1 < n
                && (s[i + 1] == 0x0E46 || s[i + 1] == 0x0EC6);
            if (!beforeRepeat) break;
        }
        mask |= maskOf(script);
        const int src = srcOverride >= 0 ? srcOverride : int(i);
        if (hasDecomp(e)) {
            const QStringView form = decompOf(cp);
            for (qsizetype k = 0; k < form.size(); ++k) {
                const auto [dcp, dwidth] = decode(form.utf16(), form.size(), k);
                units.append({dcp, entryFor(dcp), src});
                k += dwidth - 1;
            }
        } else {
            units.append({cp, e, src});
        }
        i += width;
    }
    return i;
}

// The reading style of each script family in one pass (0 everywhere is the
// primary fold).
using Styles = std::array<std::uint8_t, 32>;
constexpr Styles kPrimary{};

void dispatch(Script family, const Units &units, const Styles &styles, const Context &ctx, Out &out)
{
    const int style = styles[unsigned(family)];
    const Unit *u = units.data();
    const qsizetype n = units.size();
    switch (family) {
    case Script::Kana:
        foldKana(u, n, style, out);
        return;
    case Script::Han:
        foldHan(u, n, style, ctx, out);
        return;
    case Script::Hangul:
        foldHangul(u, n, style, out);
        return;
    case Script::Armenian:
        foldArmenian(u, n, style, out);
        return;
    case Script::Greek:
        foldGreek(u, n, style, out);
        return;
    case Script::Cyrillic:
        foldCyrillic(u, n, style, ctx, out);
        return;
    case Script::Thai:
    case Script::Lao:
        if (style > 0) {
            foldThai(u, n, style, out);
            return;
        }
        break;
    case Script::Brahmic:
        if (style > 0) {
            foldBrahmic(u, n, style, out);
            return;
        }
        break;
    case Script::Arabic:
        if (style > 0) {
            foldArabic(u, n, style, out);
            return;
        }
        break;
    case Script::Hebrew:
        if (style > 0) {
            foldHebrew(u, n, style, out);
            return;
        }
        break;
    case Script::Syriac:
        if (style > 0) {
            foldSyriac(u, n, out);
            return;
        }
        break;
    case Script::Thaana:
        if (style > 0) {
            foldThaana(u, n, out);
            return;
        }
        break;
    case Script::Tibetan:
        if (style > 0) {
            foldTibetan(u, n, style, out);
            return;
        }
        break;
    case Script::LatinAlt:
        if (style > 0) {
            foldLatinAlt(u, n, style, out);
            return;
        }
        break;
    default:
        break;
    }
    for (qsizetype k = 0; k < n; ++k) emitPrimary(u[k], out);
}

// Folds s[0, n) into `out` with reading style `pass`. `srcOverride` maps every
// unit to one source index (a compatibility expansion folds in place of the
// character it came from). `mask` collects the scripts seen; `fullMask` is the
// whole string's, which language-dependent readings consult.
void runPass(const char16_t *s, qsizetype n, const Styles &styles, int srcOverride, std::uint32_t &mask,
             std::uint32_t fullMask, Out &out)
{
    qsizetype i = 0;
    while (i < n) {
        if (s[i] < 0x80) {
            qsizetype j = i + 1;
            while (j < n && s[j] < 0x80) ++j;
            out.putAscii(s + i, j - i, srcOverride >= 0 ? srcOverride : int(i), srcOverride < 0);
            i = j;
            continue;
        }
        const auto [cp, width] = decode(s, n, i);
        const std::uint32_t e = entryFor(cp);
        const Script script = scriptOf(e);
        const Kind kind = kindOf(e);
        const Script family = familyOf(script);
        if (kind == Kind::Script || styles[unsigned(family)] > 0) {
            Units units;
            Context ctx;
            ctx.mask = fullMask | mask;
            ctx.before = srcOverride < 0 ? previousCodePoint(s, i) : 0;
            const qsizetype end = collectRun(s, n, i, family, srcOverride, mask, units);
            if (units.isEmpty()) {
                i += width;
                continue;
            }
            dispatch(family, units, styles, ctx, out);
            i = end;
            continue;
        }
        mask |= maskOf(script);
        const int src = srcOverride >= 0 ? srcOverride : int(i);
        switch (kind) {
        case Kind::Keep:
            out.put(s[i], src);
            if (width == 2) out.put(s[i + 1], srcOverride >= 0 ? srcOverride : int(i + 1));
            break;
        case Kind::Char:
            out.put(char16_t(payloadOf(e)), src);
            break;
        case Kind::Pool:
            out.put(QStringView(kPool + payloadOf(e), qsizetype(lengthOf(e))), src);
            break;
        case Kind::Expand:
            runPass(kPool + payloadOf(e), qsizetype(lengthOf(e)), styles, src, mask, fullMask, out);
            break;
        case Kind::Drop:
        case Kind::Script:
            break;
        }
        i += width;
    }
}

// Kana and ideographs choose their styles together: Japanese readings of
// kanji pair with the kana spelling of the same pass.
Script groupOf(Script family)
{
    return family == Script::Kana ? Script::Han : family;
}

void assign(Styles &styles, Script group, int style)
{
    const auto set = [&](Script family) {
        styles[unsigned(family)] = std::uint8_t(style < styleCount(family) ? style : 0);
    };
    set(group);
    if (group == Script::Han) set(Script::Kana);
}

struct Group {
    Script group;
    int count;
};

// The script groups in `mask` that have alternate readings.
QVarLengthArray<Group, 8> readingGroups(std::uint32_t mask)
{
    QVarLengthArray<Group, 8> groups;
    for (unsigned s = 0; s < 32; ++s) {
        if (!(mask & (1u << s))) continue;
        const Script group = groupOf(familyOf(Script(s)));
        const int count = group == Script::Han ? std::max(styleCount(Script::Han), styleCount(Script::Kana))
                                               : styleCount(group);
        if (count < 2 || std::any_of(groups.cbegin(), groups.cend(), [&](const Group &g) { return g.group == group; })) {
            continue;
        }
        groups.append({group, count});
    }
    return groups;
}

struct Folded {
    QString text;
    QVector<int> index;
};

Folded foldPass(const QString &src, const Styles &styles, bool withIndex, std::uint32_t &mask,
                std::uint32_t fullMask)
{
    Out out(withIndex, src.size() + 8);
    runPass(reinterpret_cast<const char16_t *>(src.utf16()), src.size(), styles, -1, mask, fullMask, out);
    auto [text, index] = out.take();
    return {std::move(text), std::move(index)};
}

// Every distinct fold of `src`, primary first.
//
// Each script group reads independently. A group alone contributes each of
// its styles that reads this string differently; with several groups, the
// folds cover every combination of those distinct readings (up to 16), so a
// word mixing scripts matches whichever reading each part is typed in.
// Beyond that, the groups also move together style by style.
QVector<FoldResult> allFolds(const QString &src, bool withIndex)
{
    std::uint32_t mask = 0;
    Folded primary = foldPass(src, kPrimary, withIndex, mask, 0);
    QVector<FoldResult> folds;
    folds.append({std::move(primary.text), std::move(primary.index)});
    const auto fold = [&](const Styles &styles) {
        std::uint32_t seen = 0;
        return foldPass(src, styles, withIndex, seen, mask);
    };
    const auto add = [&](Folded &&alt) {
        const bool duplicate = std::any_of(folds.cbegin(), folds.cend(),
                                           [&](const FoldResult &f) { return f.text == alt.text; });
        if (!duplicate) folds.append({std::move(alt.text), std::move(alt.index)});
    };

    const QVarLengthArray<Group, 8> groups = readingGroups(mask);
    // The styles of each group that read this string differently from its others.
    QVarLengthArray<QVarLengthArray<int, 4>, 8> distinct;
    for (const Group &g : groups) {
        QVarLengthArray<int, 4> styles{0};
        QStringList readings{folds.first().text};
        for (int k = 1; k < g.count; ++k) {
            Styles alone{};
            assign(alone, g.group, k);
            Folded alt = fold(alone);
            if (readings.contains(alt.text)) continue;
            readings.append(alt.text);
            styles.append(k);
            add(std::move(alt));
        }
        distinct.append(styles);
    }
    if (groups.size() < 2) return folds;

    const int secondHanStyle = styleCount(Script::Han) - 1;
    qsizetype hanGroup = -1;
    for (qsizetype g = 0; g < groups.size(); ++g) {
        if (groups[g].group == Script::Han
            && std::find(distinct[g].cbegin(), distinct[g].cend(), secondHanStyle) != distinct[g].cend()) {
            hanGroup = g;
            break;
        }
    }

    if (hanGroup >= 0) {
        QVarLengthArray<QVarLengthArray<int, 4>, 8> establishedStyles;
        QVarLengthArray<QVarLengthArray<int, 4>, 8> secondHanStyles;
        for (qsizetype g = 0; g < groups.size(); ++g) {
            QVarLengthArray<int, 4> established;
            for (const int style : distinct[g]) {
                if (g != hanGroup || style != secondHanStyle) established.append(style);
            }
            establishedStyles.append(established);
            if (g == hanGroup) {
                QVarLengthArray<int, 4> second{secondHanStyle};
                secondHanStyles.append(second);
            } else {
                secondHanStyles.append(established);
            }
        }

        const auto product = [](const auto &choices) {
            int count = 1;
            for (const auto &styles : choices) {
                if (count > 16 / int(styles.size())) return 17;
                count *= int(styles.size());
            }
            return count;
        };
        const auto enumerate = [&](const auto &choices, int combinations, bool fixedHan) {
            if (combinations <= 16) {
                for (int index = 0; index < combinations; ++index) {
                    Styles styles{};
                    int rest = index;
                    int varied = 0;
                    for (qsizetype g = 0; g < groups.size(); ++g) {
                        const auto &options = choices[g];
                        const int style = options[rest % int(options.size())];
                        rest /= int(options.size());
                        assign(styles, groups[g].group, style);
                        varied += style != 0;
                    }
                    if (varied >= 2) add(fold(styles));
                }
                return;
            }

            int widest = 1;
            for (const Group &g : groups) {
                const int count = g.group == Script::Han ? secondHanStyle : g.count;
                widest = std::max(widest, count);
            }
            for (int k = 1; k < widest; ++k) {
                Styles styles{};
                int varied = 0;
                for (const Group &g : groups) {
                    if (fixedHan && g.group == Script::Han) {
                        assign(styles, g.group, secondHanStyle);
                        ++varied;
                        continue;
                    }
                    const int count = g.group == Script::Han ? secondHanStyle : g.count;
                    const int style = k < count ? k : 0;
                    assign(styles, g.group, style);
                    varied += style != 0;
                }
                if (varied >= 2) add(fold(styles));
            }
        };

        // Keep the established reading combinations intact, then combine the
        // second Han reading with those same non-Han alternatives.
        enumerate(establishedStyles, product(establishedStyles), false);
        enumerate(secondHanStyles, product(secondHanStyles), true);
        return folds;
    }

    int combinations = 1;
    for (const auto &styles : distinct) combinations = std::min<int>(combinations * int(styles.size()), 17);
    if (combinations <= 16) {
        for (int index = 1; index < combinations; ++index) {
            Styles styles{};
            int rest = index;
            int varied = 0;
            for (qsizetype g = 0; g < groups.size(); ++g) {
                const int k = distinct[g][rest % distinct[g].size()];
                rest /= int(distinct[g].size());
                assign(styles, groups[g].group, k);
                varied += k != 0;
            }
            if (varied >= 2) add(fold(styles));  // one group alone is already folded
        }
        return folds;
    }
    int widest = 1;
    for (const Group &g : groups) widest = std::max(widest, g.count);
    for (int k = 1; k < widest; ++k) {
        Styles styles{};
        for (const Group &g : groups) assign(styles, g.group, k < g.count ? k : 0);
        add(fold(styles));
    }
    return folds;
}

} // namespace

namespace Engine {

void emitPrimary(const Unit &unit, Out &out)
{
    const std::uint32_t e = unit.entry;
    switch (kindOf(e)) {
    case Kind::Keep:
    case Kind::Script:
        out.putCodePoint(unit.cp, unit.src);
        return;
    case Kind::Char:
        out.put(char16_t(payloadOf(e)), unit.src);
        return;
    case Kind::Pool:
        out.put(QStringView(kPool + payloadOf(e), qsizetype(lengthOf(e))), unit.src);
        return;
    case Kind::Expand: {
        std::uint32_t mask = 0;
        runPass(kPool + payloadOf(e), qsizetype(lengthOf(e)), kPrimary, unit.src, mask, 0, out);
        return;
    }
    case Kind::Drop:
        return;
    }
}

} // namespace Engine

FoldResult fold(const QString &src)
{
    if (QString ascii; foldAscii(src, ascii)) return {std::move(ascii), identityIndex(src.size())};
    std::uint32_t mask = 0;
    if (needsComposition(src)) {
        const Composed composed = compose(src);
        Folded primary = foldPass(composed.text, kPrimary, true, mask, 0);
        remap(primary.index, composed.map);
        return {std::move(primary.text), std::move(primary.index)};
    }
    Folded primary = foldPass(src, kPrimary, true, mask, 0);
    return {std::move(primary.text), std::move(primary.index)};
}

QString foldText(const QString &src)
{
    if (QString ascii; foldAscii(src, ascii)) return ascii;
    std::uint32_t mask = 0;
    return foldPass(needsComposition(src) ? compose(src).text : src, kPrimary, false, mask, 0).text;
}

QString foldSegments(const QString &src)
{
    if (QString ascii; foldAscii(src, ascii)) return ascii;
    const QVector<FoldResult> folds = allFolds(needsComposition(src) ? compose(src).text : src, false);
    if (folds.size() == 1) return folds.first().text;
    QString joined = folds.first().text;
    for (qsizetype k = 1; k < folds.size(); ++k) {
        joined += kSegmentSeparator;
        joined += folds.at(k).text;
    }
    return joined;
}

QVector<FoldResult> foldAll(const QString &src)
{
    if (QString ascii; foldAscii(src, ascii)) return {{std::move(ascii), identityIndex(src.size())}};
    if (!needsComposition(src)) return allFolds(src, true);
    const Composed composed = compose(src);
    QVector<FoldResult> folds = allFolds(composed.text, true);
    for (FoldResult &f : folds) remap(f.srcIndex, composed.map);
    return folds;
}

quint32 cacheSignature()
{
    return (kVersion * 0x9E3779B1u) ^ Engine::kDataDigest;
}

} // namespace Search::Fold
