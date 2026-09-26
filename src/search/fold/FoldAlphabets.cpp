// Alternate readings of alphabets whose primary fold is letter by letter:
// Greek, Cyrillic, Armenian, and Latin letters with other common spellings.

#include "search/fold/FoldEngine.h"

namespace Search::Fold::Engine {

namespace {

constexpr char32_t kGreekFirst = 0x03B1;  // α
constexpr char32_t kGreekLast = 0x03C9;   // ω

bool isBasicGreek(char32_t cp)
{
    return cp >= kGreekFirst && cp <= kGreekLast;
}

bool isGreekVowel(char32_t cp)
{
    return cp == U'α' || cp == U'ε' || cp == U'η' || cp == U'ι' || cp == U'ο' || cp == U'υ' || cp == U'ω';
}

// Sounds that make αυ, ευ, ηυ read af, ef, if.
bool isVoicelessGreek(char32_t cp)
{
    return cp == U'θ' || cp == U'κ' || cp == U'ξ' || cp == U'π' || cp == U'σ' || cp == U'ς' || cp == U'τ'
        || cp == U'φ' || cp == U'χ' || cp == U'ψ';
}

struct GreekLetter {
    Unit unit;
    bool diaeresis = false;
    bool accent = false;
};

const GreekPair *findPair(const GreekStyle &st, char32_t a, char32_t b)
{
    for (int k = 0; k < st.pairCount; ++k) {
        if (st.pair[k].first == a && st.pair[k].second == b) return &st.pair[k];
    }
    return nullptr;
}

} // namespace

void foldGreek(const Unit *u, qsizetype n, int style, Out &out)
{
    const GreekStyle &st = kGreekStyles[style - 1];
    QVarLengthArray<GreekLetter, 32> letters;
    for (qsizetype k = 0; k < n; ++k) {
        const char32_t cp = u[k].cp;
        if (scriptOf(u[k].entry) == Script::Inherited) {
            if (!letters.isEmpty()) {
                if (cp == 0x0308) letters.back().diaeresis = true;
                else if (cp == 0x0301 || cp == 0x0300 || cp == 0x0342) letters.back().accent = true;
            }
            continue;
        }
        letters.append({u[k]});
    }
    const qsizetype count = letters.size();
    const auto cpAt = [&](qsizetype i) { return i < count ? letters[i].unit.cp : char32_t(0); };
    // An e or i sound starts at `i` (ε, η, ι, υ, or the diphthongs αι, ει, οι).
    const auto frontVowelAt = [&](qsizetype i) {
        const char32_t c = cpAt(i);
        if (c == U'ε' || c == U'η' || c == U'ι' || c == U'υ') return true;
        return (c == U'α' || c == U'ο') && cpAt(i + 1) == U'ι' && !letters[i + 1].diaeresis;
    };

    for (qsizetype i = 0; i < count; ++i) {
        const GreekLetter &a = letters[i];
        const char32_t cp = a.unit.cp;
        if (!isBasicGreek(cp)) {
            emitPrimary(a.unit, out);
            continue;
        }
        if (i + 1 < count && isBasicGreek(cpAt(i + 1))) {
            const GreekLetter &b = letters[i + 1];
            const GreekPair *pair = findPair(st, char16_t(cp), char16_t(b.unit.cp));
            const bool vowelPair = isGreekVowel(cp) && isGreekVowel(b.unit.cp);
            if (pair && !(vowelPair && (b.diaeresis || a.accent))) {
                QStringView text = view(pair->a);
                if (pair->kind == GreekPairKind::Initial && i > 0) text = view(pair->b);
                if (pair->kind == GreekPairKind::Voicing && (i + 2 >= count || isVoicelessGreek(cpAt(i + 2)))) {
                    text = view(pair->b);
                }
                out.put(text, a.unit.src);
                ++i;
                continue;
            }
        }
        if (cp == U'γ' && st.frontGamma.len != 0 && frontVowelAt(i + 1)) {
            out.put(st.frontGamma, a.unit.src);
            continue;
        }
        out.put(st.letter[cp - kGreekFirst], a.unit.src);
    }
}

void foldCyrillic(const Unit *u, qsizetype n, int style, const Context &ctx, Out &out)
{
    const CyrillicStyle &st = kCyrillicStyles[style - 1];
    AlphaRange language{0, 0};
    if (ctx.mask & maskOf(Script::CyrillicBe)) language = st.be;
    else if (ctx.mask & maskOf(Script::CyrillicSr)) language = st.sr;
    else if (ctx.mask & maskOf(Script::CyrillicUk)) language = st.uk;

    bool atStart = true;
    for (qsizetype k = 0; k < n; ++k) {
        if (scriptOf(u[k].entry) == Script::Inherited) {
            emitPrimary(u[k], out);
            continue;
        }
        const AlphaRule *rule = findRule(language, u[k].cp, 0);
        if (!rule) rule = findRule(st.map, u[k].cp, 0);
        if (rule) out.put(atStart ? rule->initial : rule->medial, u[k].src);
        else emitPrimary(u[k], out);
        atStart = false;
    }
}

// Style 0, the primary fold, reads letter by letter (ե e, ու ov), so an
// Armenian query finds any text containing it; style 1 is Eastern Armenian
// as read, with word-initial forms and ու as u (Երևան yerevan); style 2 is
// Western Armenian.
void foldArmenian(const Unit *u, qsizetype n, int style, Out &out)
{
    bool atStart = style != 0;
    for (qsizetype k = 0; k < n; ++k) {
        if (scriptOf(u[k].entry) == Script::Inherited) continue;
        const char32_t cp = u[k].cp;
        const AlphaRule *rule = style != 0 && k + 1 < n ? findRule(kArmenianEastern, cp, u[k + 1].cp) : nullptr;
        if (rule) {
            out.put(atStart ? rule->initial : rule->medial, u[k].src);
            ++k;
            atStart = false;
            continue;
        }
        if (style == 2) rule = findRule(kArmenianWestern, cp, 0);
        if (!rule) rule = findRule(kArmenianEastern, cp, 0);
        if (rule) out.put(atStart ? rule->initial : rule->medial, u[k].src);
        else emitPrimary(u[k], out);
        atStart = false;
    }
}

// Style 1 spells umlauts as digraphs (mueller); style 2 spells ü as i (miller).
void foldLatinAlt(const Unit *u, qsizetype n, int style, Out &out)
{
    const AlphaRange table = style == 1 ? kGerman : kUmlautAsI;
    for (qsizetype k = 0; k < n; ++k) {
        if (const AlphaRule *rule = findRule(table, u[k].cp, 0)) out.put(rule->medial, u[k].src);
        else emitPrimary(u[k], out);
    }
}

} // namespace Search::Fold::Engine
