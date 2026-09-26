// Kana and CJK ideographs.
//
// Kana romanize in wapuro style (the primary fold: とうきょう → toukyou) or in
// Hepburn with long vowels collapsed (tokyo). A syllable followed by a small
// vowel reads as one mora the way loanwords spell it (ティ ti, ファ fa, シェ
// she, きゃ kya), a sokuon doubles the next consonant, ー is dropped, and the
// iteration marks repeat the previous kana.
//
// Ideographs keep their surface form in the primary fold (traditional forms
// folded to simplified, compiled into the entry). Alternate readings give the
// Japanese seed dictionary's readings, romanized as kana, and toneless pinyin
// with word readings for polyphones (音乐 yinyue, 快乐 kuaile).

#include "search/fold/FoldEngine.h"

#include <algorithm>

namespace Search::Fold::Engine {

namespace {

using Reading = QVarLengthArray<char16_t, 16>;

void append(Reading &r, QStringView s)
{
    r.append(s.utf16(), s.size());
}

bool isVowel(char16_t c)
{
    return c == u'a' || c == u'i' || c == u'u' || c == u'e' || c == u'o';
}

bool isKanaUnit(const Unit &u)
{
    return kindOf(u.entry) == Kind::Script && scriptOf(u.entry) == Script::Kana;
}

const KanaInfo &kanaOf(const Unit &u)
{
    return kKana[payloadOf(u.entry)];
}

// The mora a syllable and a following small kana read as together.
void combine(const KanaInfo &syllable, const KanaInfo &small, Reading &r)
{
    const QStringView sm = view(small.wapuro);
    const QStringView c = view(syllable.consonant);
    const char16_t v = syllable.vowel;
    if (sm.size() == 1 && sm[0] == v) {  // a small vowel repeating the syllable's lengthens it
        append(r, view(syllable.wapuro));
        append(r, sm);
        return;
    }
    const bool palatal = c == u"sh" || c == u"ch" || c == u"j";
    if (sm.startsWith(u'y')) {           // きゃ kya, しゃ sha, ふゅ fyu, てゅ tyu
        append(r, c);
        append(r, palatal ? sm.mid(1) : sm);
        return;
    }
    if (sm == u"wa") {                   // くゎ kwa
        append(r, c);
        append(r, sm);
        return;
    }
    if (v == u'i') {                     // しぇ she, きぇ kye, いぇ ye
        append(r, c);
        if (!palatal) r.append(u'y');
        append(r, sm);
        return;
    }
    if (v == u'u') {                     // ファ fa, ウィ wi, クァ kwa, ツァ tsa
        append(r, view(syllable.combine));
        append(r, sm);
        return;
    }
    append(r, c);                        // ティ ti, トゥ tu
    append(r, sm);
}

void geminate(Reading &r)
{
    if (r.isEmpty() || isVowel(r[0]) || r[0] == u'n') return;
    r.insert(r.begin(), r[0] == u'c' ? u't' : r[0]);
}

} // namespace

void foldKana(const Unit *u, qsizetype n, int style, Out &out)
{
    const bool hepburn = style == 1;
    bool pendingGemination = false;
    const KanaInfo *last = nullptr;
    char16_t lastVowel = 0;
    for (qsizetype k = 0; k < n; ++k) {
        if (!isKanaUnit(u[k])) {
            emitPrimary(u[k], out);
            lastVowel = 0;
            continue;
        }
        const KanaInfo *info = &kanaOf(u[k]);
        const int src = u[k].src;
        switch (info->kind) {
        case KanaKind::None:
        case KanaKind::Dakuten:
        case KanaKind::Handakuten:
        case KanaKind::Long:
            continue;
        case KanaKind::Sokuon:
            pendingGemination = true;
            continue;
        case KanaKind::Iteration:
            if (!last) continue;
            info = last;
            break;
        case KanaKind::VoicedIteration:
            if (!last) continue;
            info = last->voiced ? &kKana[last->voiced] : last;
            break;
        default:
            break;
        }
        if (k + 1 < n && isKanaUnit(u[k + 1])) {
            const KanaKind mark = kanaOf(u[k + 1]).kind;
            if (mark == KanaKind::Dakuten || mark == KanaKind::Handakuten) {
                const std::uint16_t id = mark == KanaKind::Dakuten ? info->voiced : info->semivoiced;
                if (id) info = &kKana[id];
                ++k;
            }
        }
        Reading reading;
        bool combined = false;
        if (info->kind == KanaKind::Syllable && info->vowel && k + 1 < n && isKanaUnit(u[k + 1])
            && kanaOf(u[k + 1]).kind == KanaKind::Small) {
            combine(*info, kanaOf(u[k + 1]), reading);
            combined = true;
            ++k;
        } else {
            append(reading, view(hepburn ? info->hepburn : info->wapuro));
        }
        if (pendingGemination) {
            geminate(reading);
            pendingGemination = false;
        }
        last = info;
        if (hepburn && !combined && info->kind == KanaKind::Syllable && info->consonant.len == 0
            && reading.size() == 1 && lastVowel != 0) {
            const char16_t v = reading[0];
            if ((v == lastVowel && v != u'i') || (lastVowel == u'o' && v == u'u')) continue;
        }
        out.put(QStringView(reading.data(), reading.size()), src);
        lastVowel = !reading.isEmpty() && isVowel(reading.back()) ? reading.back() : char16_t(0);
    }
}

namespace {

bool isIterationMark(char32_t cp)
{
    return cp == 0x3005 || cp == 0x303B;
}

bool isHanEntry(std::uint32_t e)
{
    return scriptOf(e) == Script::Han && !(kindOf(e) == Kind::Script);
}

const Word *findWord(const Word *words, std::size_t count, QStringView key)
{
    const Word *end = words + count;
    const Word *it = std::lower_bound(words, end, key, [](const Word &w, QStringView k) {
        return view(w.key).compare(k) < 0;
    });
    return it != end && view(it->key) == key ? it : nullptr;
}

std::uint16_t pinyinOf(char32_t cp)
{
    if (cp >= 0x3400 && cp < 0xA000) return kHanPinyinBmp[cp - 0x3400];
    if (cp < 0x10000) return 0;
    const HanAstralPinyin *end = kHanPinyinAstral + kHanPinyinAstralCount;
    const HanAstralPinyin *it = std::lower_bound(kHanPinyinAstral, end, cp,
                                                 [](const HanAstralPinyin &h, char32_t c) { return h.cp < c; });
    return it != end && it->cp == cp ? it->pinyin : std::uint16_t(0);
}

void emitSurface(char32_t cp, int src, Out &out)
{
    emitPrimary({cp, entryFor(cp), src}, out);
}

// Longest dictionary word starting at `j`, or nullptr. `keys` holds the run's
// code units with astral ideographs replaced by a non-character, so offsets
// in it are unit offsets.
const Word *longestWord(const Word *words, std::size_t count, int longest, QStringView keys, qsizetype j,
                        qsizetype &length)
{
    for (qsizetype len = std::min<qsizetype>(longest, keys.size() - j); len >= 1; --len) {
        if (const Word *w = findWord(words, count, keys.mid(j, len))) {
            length = len;
            return w;
        }
    }
    return nullptr;
}

} // namespace

void foldHan(const Unit *u, qsizetype n, int style, const Context &ctx, Out &out)
{
    // Iteration marks repeat the ideograph before them.
    QVarLengthArray<char32_t, 32> cps(n);
    QVarLengthArray<char16_t, 32> keys(n);
    for (qsizetype k = 0; k < n; ++k) {
        char32_t cp = u[k].cp;
        if (isIterationMark(cp)) {
            if (k > 0) cp = cps[k - 1];
            else if (ctx.before != 0 && isHanEntry(entryFor(ctx.before))) cp = ctx.before;
        }
        cps[k] = cp;
        keys[k] = cp < 0x10000 ? char16_t(cp) : char16_t(0xFFFF);
    }
    const QStringView keyView(keys.data(), n);

    if (style == 0) {
        for (qsizetype k = 0; k < n; ++k) emitSurface(cps[k], u[k].src, out);
        return;
    }
    if (style == 2) {
        for (qsizetype j = 0; j < n;) {
            qsizetype len = 0;
            if (const Word *w = longestWord(kChineseWords, kChineseWordCount, kChineseLongest, keyView, j, len)) {
                const QStringView syllables = view(w->value);
                qsizetype m = 0;
                for (const QStringView syllable : syllables.tokenize(u' ')) {
                    out.put(syllable, u[j + std::min(m, len - 1)].src);
                    ++m;
                }
                j += len;
                continue;
            }
            if (const std::uint16_t id = pinyinOf(cps[j])) out.put(kPinyin[id], u[j].src);
            else emitSurface(cps[j], u[j].src, out);
            ++j;
        }
        return;
    }
    // Styles 1 and 3: Japanese readings, romanized in Hepburn (1) or wapuro (3).
    const int kanaStyle = style == 1 ? 1 : 0;
    for (qsizetype j = 0; j < n;) {
        qsizetype len = 0;
        if (const Word *w = longestWord(kJapaneseWords, kJapaneseWordCount, kJapaneseLongest, keyView, j, len)) {
            const QStringView reading = view(w->value);
            const qsizetype count = reading.size();
            Units kana;
            for (qsizetype r = 0; r < count; ++r) {
                const char32_t cp = reading[r].unicode();
                kana.append({cp, entryFor(cp), u[j + ((2 * r + 1) * len) / (2 * count)].src});
            }
            foldKana(kana.data(), kana.size(), kanaStyle, out);
            j += len;
            continue;
        }
        emitSurface(cps[j], u[j].src, out);
        ++j;
    }
}

} // namespace Search::Fold::Engine
