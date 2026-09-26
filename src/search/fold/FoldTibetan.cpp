// Tibetan read as Wylie transliteration.
//
// A syllable, delimited by the tsheg ་, is a row of stacks: optional prefix
// letters, the root stack (a base letter with superscript and subscript
// letters written under it), and optional suffixes. The inherent vowel a
// follows the root unless a vowel sign is written. The root is the stack
// that carries a vowel sign or subjoined letters; otherwise the syllable's
// spelling decides it: a prefix letter (ག ད བ མ འ) precedes only certain
// roots, and only ག ང ད ན བ མ འ ར ལ ས end a syllable (དགའ dga', གངས gangs).
// Where a spelling reads either way the prefix reading wins (དམས dmas),
// except in the two words convention reads root first (མངས mangs). A stack
// before the root that is not a lone prefix reads with its own a, as
// Sanskrit words do (པདྨེ padme). Syllables join without a separator
// (སྨོན་ལམ smonlam).
//
// Style 1 is Wylie, with the silent འ as an apostrophe (བཀའ bka'); style 2
// drops the apostrophe (bka).

#include "search/fold/FoldEngine.h"

#include <algorithm>
#include <initializer_list>

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

// The root letters a prefix may precede.
bool prefixes(char32_t prefix, char32_t root)
{
    const auto in = [root](std::initializer_list<char32_t> roots) {
        return std::find(roots.begin(), roots.end(), root) != roots.end();
    };
    switch (prefix) {
    case 0x0F42:  // ག: ཅ ཉ ཏ ད ན ཙ ཞ ཟ ཡ ཤ ས
        return in({0x0F45, 0x0F49, 0x0F4F, 0x0F51, 0x0F53, 0x0F59, 0x0F5E, 0x0F5F, 0x0F61, 0x0F64, 0x0F66});
    case 0x0F51:  // ད: ཀ ག ང པ བ མ
        return in({0x0F40, 0x0F42, 0x0F44, 0x0F54, 0x0F56, 0x0F58});
    case 0x0F56:  // བ: ཀ ག ཅ ཏ ད ཙ ཞ ཟ ཤ ས
        return in({0x0F40, 0x0F42, 0x0F45, 0x0F4F, 0x0F51, 0x0F59, 0x0F5E, 0x0F5F, 0x0F64, 0x0F66});
    case 0x0F58:  // མ: ཁ ག ང ཆ ཇ ཉ ཐ ད ན ཚ ཛ
        return in({0x0F41, 0x0F42, 0x0F44, 0x0F46, 0x0F47, 0x0F49, 0x0F50, 0x0F51, 0x0F53, 0x0F5A, 0x0F5B});
    case 0x0F60:  // འ: ཁ ག ཆ ཇ ཐ ད ཕ བ ཚ ཛ
        return in({0x0F41, 0x0F42, 0x0F46, 0x0F47, 0x0F50, 0x0F51, 0x0F55, 0x0F56, 0x0F5A, 0x0F5B});
    default:
        return false;
    }
}

bool isSuffixLetter(char32_t cp)  // ག ང ད ན བ མ འ ར ལ ས
{
    return cp == 0x0F42 || cp == 0x0F44 || cp == 0x0F51 || cp == 0x0F53 || cp == 0x0F56 || cp == 0x0F58
        || cp == 0x0F60 || cp == 0x0F62 || cp == 0x0F63 || cp == 0x0F66;
}

// The three-letter spellings a prefix could open that read as root, suffix
// and second suffix ས instead: མངས mangs, བགས bags. The other spellings that
// read either way (དགས དངས དབས དམས མགས འགས འབས) keep the prefix reading.
bool readsRootFirst(char32_t first, char32_t second, char32_t third)
{
    return third == 0x0F66 && ((first == 0x0F58 && second == 0x0F44) || (first == 0x0F56 && second == 0x0F42));
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
    const auto plain = [&](qsizetype k) { return !stacks[k].subjoined; };
    if (stacks.size() >= 4) return 1;
    if (stacks.size() == 3) {
        const bool prefixed = plain(0) && prefixes(stacks[0].base, stacks[1].base) && isSuffixLetter(stacks[2].base);
        return prefixed && !readsRootFirst(stacks[0].base, stacks[1].base, stacks[2].base) ? 1 : 0;
    }
    if (stacks.size() == 2 && !isSuffixLetter(stacks[1].base) && prefixes(stacks[0].base, stacks[1].base)) return 1;
    return 0;
}

void foldSyllable(QVarLengthArray<Stack, 8> &stacks, QVarLengthArray<Str, 2> &coda, int codaSrc, int style,
                  Out &out)
{
    if (stacks.isEmpty()) return;
    const qsizetype root = rootOf(stacks);
    for (qsizetype k = 0; k < stacks.size(); ++k) {
        const Stack &s = stacks[k];
        for (const Str letter : s.letters) {
            if (style == 2 && view(letter) == u"'") continue;
            out.put(letter, s.src);
        }
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

void foldTibetan(const Unit *u, qsizetype n, int style, Out &out)
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
            foldSyllable(stacks, coda, codaSrc, style, out);
            break;
        case TibetanRole::None:
            break;
        }
    }
    foldSyllable(stacks, coda, codaSrc, style, out);
}

} // namespace Search::Fold::Engine
