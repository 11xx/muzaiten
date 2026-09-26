#include "search/fold/Fold.h"

#include "search/fold/FoldEngine.h"

#include <QVarLengthArray>

#include <algorithm>

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
        return 4;
    case Script::Greek:
    case Script::Cyrillic:
    case Script::Hangul:
    case Script::Thai:
    case Script::Lao:
        return 3;
    case Script::LatinAlt:
    case Script::Armenian:
    case Script::Kana:
        return 2;
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
        if (familyOf(script) != family && script != Script::Inherited) break;
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

void dispatch(Script family, const Units &units, int pass, const Context &ctx, Out &out)
{
    const int style = pass < styleCount(family) ? pass : 0;
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
    case Script::LatinAlt:
        if (style == 1) {
            foldGerman(u, n, out);
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
void runPass(const char16_t *s, qsizetype n, int pass, int srcOverride, std::uint32_t &mask,
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
        if (kind == Kind::Script || (pass > 0 && pass < styleCount(family))) {
            Units units;
            Context ctx;
            ctx.mask = fullMask | mask;
            ctx.before = srcOverride < 0 ? previousCodePoint(s, i) : 0;
            const qsizetype end = collectRun(s, n, i, family, srcOverride, mask, units);
            if (units.isEmpty()) {
                i += width;
                continue;
            }
            dispatch(family, units, pass, ctx, out);
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
            runPass(kPool + payloadOf(e), qsizetype(lengthOf(e)), pass, src, mask, fullMask, out);
            break;
        case Kind::Drop:
        case Kind::Script:
            break;
        }
        i += width;
    }
}

int passCount(std::uint32_t mask)
{
    int count = 1;
    for (unsigned s = 0; s < 32; ++s) {
        if (mask & (1u << s)) count = std::max(count, styleCount(familyOf(Script(s))));
    }
    return count;
}

struct Folded {
    QString text;
    QVector<int> index;
};

Folded foldPass(const QString &src, int pass, bool withIndex, std::uint32_t &mask, std::uint32_t fullMask)
{
    Out out(withIndex, src.size() + 8);
    runPass(reinterpret_cast<const char16_t *>(src.utf16()), src.size(), pass, -1, mask, fullMask, out);
    QVector<int> index = out.takeIndex();
    return {out.takeText(), std::move(index)};
}

// Every distinct fold of `src`, primary first.
QVector<FoldResult> allFolds(const QString &src, bool withIndex)
{
    std::uint32_t mask = 0;
    Folded primary = foldPass(src, 0, withIndex, mask, 0);
    QVector<FoldResult> folds;
    folds.append({std::move(primary.text), std::move(primary.index)});
    const int passes = passCount(mask);
    for (int pass = 1; pass < passes; ++pass) {
        std::uint32_t seen = 0;
        Folded alt = foldPass(src, pass, withIndex, seen, mask);
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
        runPass(kPool + payloadOf(e), qsizetype(lengthOf(e)), 0, unit.src, mask, 0, out);
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
    Folded primary = foldPass(src, 0, true, mask, 0);
    return {std::move(primary.text), std::move(primary.index)};
}

QString foldText(const QString &src)
{
    if (QString ascii; foldAscii(src, ascii)) return ascii;
    std::uint32_t mask = 0;
    return foldPass(src, 0, false, mask, 0).text;
}

QString foldSegments(const QString &src)
{
    if (QString ascii; foldAscii(src, ascii)) return ascii;
    const QVector<FoldResult> folds = allFolds(src, false);
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
    return allFolds(src, true);
}

quint32 cacheSignature()
{
    return (kVersion * 0x9E3779B1u) ^ Engine::kDataDigest;
}

} // namespace Search::Fold
