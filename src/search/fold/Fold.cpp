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
    if (withIndex) m_src.reserve(hint);
}

void Out::grow(qsizetype extra)
{
    const qsizetype cap = std::max(m_len + extra, m_cap * 2 + 16);
    m_text.resize(cap);
    m_data = reinterpret_cast<char16_t *>(m_text.data());
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
        for (qsizetype k = 0; k < n; ++k) d[k] = lowerAscii(s[k]);
        m_len += n;
        if (m_withIndex) {
            for (qsizetype k = 0; k < n; ++k) m_src.append(sequential ? src + int(k) : src);
        }
        return;
    }
    qsizetype written = 0;
    for (qsizetype k = 0; k < n; ++k) {
        if (isDroppedAscii(s[k])) continue;
        d[written++] = lowerAscii(s[k]);
        if (m_withIndex) m_src.append(sequential ? src + int(k) : src);
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

QString Out::takeText()
{
    m_text.truncate(m_len);
    m_data = nullptr;
    m_len = m_cap = 0;
    return std::move(m_text);
}

int styleCount(Script family)
{
    switch (family) {
    case Script::Han:
    case Script::Brahmic:
    case Script::Arabic:
        return 4;
    case Script::Hebrew:
        return 3;
    case Script::Syriac:
    case Script::Thaana:
        return 2;
    case Script::Greek:
    case Script::Cyrillic:
    case Script::Hangul:
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

// Canonically equivalent text folds alike: a base followed by marks that
// compose with it (decomposed file names, NFD input) is composed first.
// Composed carries the composed text and, for each of its units, the
// source index of the sequence it came from.
struct Composed {
    QString text;
    QVector<int> map;
};

bool needsComposition(const QString &src)
{
    const char16_t *s = reinterpret_cast<const char16_t *>(src.utf16());
    const qsizetype n = src.size();
    for (qsizetype i = 1; i < n; ++i) {
        const char16_t c = s[i];
        if (c < 0x0300) continue;
        if (QChar::isLowSurrogate(c) && QChar::isHighSurrogate(s[i - 1])) {
            if (composesWithPrevious(QChar::surrogateToUcs4(s[i - 1], c))) return true;
            continue;
        }
        if (composesWithPrevious(c)) return true;
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
        qsizetype j = i + decode(s, n, i).width;
        bool composable = false;
        while (j < n) {
            const auto [next, width] = decode(s, n, j);
            const bool composes = composesWithPrevious(next);
            if (!composes && QChar::combiningClass(next) == 0) break;
            composable = composable || composes;
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

// The alternate passes for a string whose scripts are `mask`. Each script
// group reads independently: with several groups, the passes cover every
// combination of their styles (up to 16 passes), so a word mixing scripts
// matches whichever reading each part is typed in. Beyond that, each group
// varies alone besides the passes that move every group together.
QVarLengthArray<Styles, 16> alternateStyles(std::uint32_t mask)
{
    struct Group {
        Script group;
        int count;
    };
    QVarLengthArray<Group, 8> groups;
    int widest = 1;
    for (unsigned s = 0; s < 32; ++s) {
        if (!(mask & (1u << s))) continue;
        const Script group = groupOf(familyOf(Script(s)));
        const int count = group == Script::Han ? std::max(styleCount(Script::Han), styleCount(Script::Kana))
                                               : styleCount(group);
        if (count < 2 || std::any_of(groups.cbegin(), groups.cend(), [&](const Group &g) { return g.group == group; })) {
            continue;
        }
        groups.append({group, count});
        widest = std::max(widest, count);
    }
    QVarLengthArray<Styles, 16> passes;
    const auto add = [&](const Styles &styles) {
        if (styles != kPrimary && std::find(passes.cbegin(), passes.cend(), styles) == passes.cend()) {
            passes.append(styles);
        }
    };
    for (int k = 1; k < widest; ++k) {
        Styles styles{};
        for (const Group &g : groups) assign(styles, g.group, k < g.count ? k : 0);
        add(styles);
    }
    if (groups.size() < 2) return passes;
    int combinations = 1;
    for (const Group &g : groups) combinations = std::min(combinations * g.count, 17);
    if (combinations <= 16) {
        for (int index = 1; index < combinations; ++index) {
            Styles styles{};
            int rest = index;
            for (const Group &g : groups) {
                assign(styles, g.group, rest % g.count);
                rest /= g.count;
            }
            add(styles);
        }
        return passes;
    }
    for (const Group &g : groups) {
        for (int k = 1; k < g.count; ++k) {
            Styles styles{};
            assign(styles, g.group, k);
            add(styles);
        }
    }
    return passes;
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
    QVector<int> index = out.takeIndex();
    return {out.takeText(), std::move(index)};
}

// Every distinct fold of `src`, primary first.
QVector<FoldResult> allFolds(const QString &src, bool withIndex)
{
    std::uint32_t mask = 0;
    Folded primary = foldPass(src, kPrimary, withIndex, mask, 0);
    QVector<FoldResult> folds;
    folds.append({std::move(primary.text), std::move(primary.index)});
    for (const Styles &styles : alternateStyles(mask)) {
        std::uint32_t seen = 0;
        Folded alt = foldPass(src, styles, withIndex, seen, mask);
        const bool duplicate = std::any_of(folds.cbegin(), folds.cend(),
                                           [&](const FoldResult &f) { return f.text == alt.text; });
        if (!duplicate) folds.append({std::move(alt.text), std::move(alt.index)});
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
