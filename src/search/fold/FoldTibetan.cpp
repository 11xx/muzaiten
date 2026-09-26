// Tibetan read as Wylie transliteration.
//
// A syllable, delimited by the tsheg ་, is a row of stacks: optional prefix
// letters, the root stack (a base letter with superscript and subscript
// letters written under it), and optional suffixes. The inherent vowel a
// follows the root unless a vowel sign is written. The root is the stack
// that carries a vowel sign or subjoined letters; otherwise it is the first
// stack, or the second when three or more stacks start with a prefix letter
// (ག ད བ མ འ). A stack before the root that is not a lone prefix reads with
// its own a, as Sanskrit words do (པདྨེ padme). Syllables join without a
// separator (སྨོན་ལམ smonlam).

#include "search/fold/FoldEngine.h"

namespace Search::Fold::Engine {

namespace {

struct Stack {
    char32_t base = 0;
    bool subjoined = false;
    int src = 0;
    QVarLengthArray<Str, 4> letters;
    QVarLengthArray<Str, 2> vowels;
};

bool isPrefixLetter(char32_t cp)
{
    return cp == 0x0F42 || cp == 0x0F51 || cp == 0x0F56 || cp == 0x0F58 || cp == 0x0F60;  // ག ད བ མ འ
}

const TibetanChar &tibetanOf(char32_t cp)
{
    static const TibetanChar none{TibetanRole::None, {0, 0}};
    return cp >= 0x0F00 && cp <= 0x0FFF ? kTibetanChars[cp - 0x0F00] : none;
}

qsizetype rootOf(const QVarLengthArray<Stack, 8> &stacks)
{
    for (qsizetype k = 0; k < stacks.size(); ++k) {
        if (!stacks[k].vowels.isEmpty()) return k;
    }
    for (qsizetype k = 0; k < stacks.size(); ++k) {
        if (stacks[k].subjoined) return k;
    }
    if (stacks.size() >= 4) return 1;
    if (stacks.size() == 3 && isPrefixLetter(stacks[0].base)) return 1;
    return 0;
}

void foldSyllable(QVarLengthArray<Stack, 8> &stacks, QVarLengthArray<Str, 2> &coda, int codaSrc, Out &out)
{
    if (stacks.isEmpty()) return;
    const qsizetype root = rootOf(stacks);
    for (qsizetype k = 0; k < stacks.size(); ++k) {
        const Stack &s = stacks[k];
        for (const Str letter : s.letters) out.put(letter, s.src);
        // Only one prefix letter precedes a root; any other stack there is a
        // syllable of a Sanskrit word with its own a (པདྨེ padme).
        if (k < root && (k + 1 < root || !isPrefixLetter(s.base) || s.subjoined)) out.put(u'a', s.src);
        if (k != root) continue;
        if (s.vowels.isEmpty()) out.put(u'a', s.src);
        for (const Str vowel : s.vowels) out.put(vowel, s.src);
    }
    for (const Str c : coda) out.put(c, codaSrc);
    stacks.clear();
    coda.clear();
}

} // namespace

void foldTibetan(const Unit *u, qsizetype n, Out &out)
{
    QVarLengthArray<Stack, 8> stacks;
    QVarLengthArray<Str, 2> coda;
    int codaSrc = 0;
    for (qsizetype k = 0; k < n; ++k) {
        const TibetanChar &c = tibetanOf(u[k].cp);
        switch (c.role) {
        case TibetanRole::Base: {
            Stack s;
            s.base = u[k].cp;
            s.src = u[k].src;
            s.letters.append(c.roman);
            stacks.append(std::move(s));
            break;
        }
        case TibetanRole::Subjoined:
            if (stacks.isEmpty()) break;
            stacks.back().letters.append(c.roman);
            stacks.back().subjoined = true;
            break;
        case TibetanRole::Vowel:
            if (!stacks.isEmpty()) stacks.back().vowels.append(c.roman);
            break;
        case TibetanRole::Coda:
            coda.append(c.roman);
            codaSrc = u[k].src;
            break;
        case TibetanRole::Tsheg:
            foldSyllable(stacks, coda, codaSrc, out);
            break;
        case TibetanRole::None:
            break;
        }
    }
    foldSyllable(stacks, coda, codaSrc, out);
}

} // namespace Search::Fold::Engine
