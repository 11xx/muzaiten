// The compiled fold tables and the lookups over them.

#include "search/fold/FoldEngine.h"

#include <algorithm>

namespace Search::Fold::Engine {

#include "FoldTables.inc"

std::uint32_t astralEntry(char32_t cp)
{
    const AstralEntry *end = kAstral + kAstralCount;
    const AstralEntry *it = std::lower_bound(kAstral, end, cp,
                                             [](const AstralEntry &e, char32_t c) { return e.cp < c; });
    if (it != end && it->cp == cp) return it->entry;
    for (std::size_t k = 0; k < kAstralHanCount; ++k) {
        if (cp >= kAstralHan[k].first && cp <= kAstralHan[k].last) {
            return (std::uint32_t(Kind::Keep)) | (std::uint32_t(Script::Han) << 3);
        }
    }
    return std::uint32_t(Kind::Keep);
}

bool normalizes(char32_t cp)
{
    if (cp < 0x10000) return (kComposableBmp[cp / 32] >> (cp % 32)) & 1u;
    return std::binary_search(kComposableAstral, kComposableAstral + kComposableAstralCount, cp);
}

char32_t composeStarters(char32_t first, char32_t second)
{
    if (!normalizes(second)) return 0;
    // Hangul composes algorithmically: a leading consonant takes a vowel, and
    // a syllable without a final consonant takes one.
    constexpr char32_t kSyllable = 0xAC00;
    if (second >= 0x1161 && second <= 0x1175) {
        return first >= 0x1100 && first <= 0x1112 ? kSyllable + ((first - 0x1100) * 21 + (second - 0x1161)) * 28 : 0;
    }
    if (second >= 0x11A8 && second <= 0x11C2) {
        return first >= kSyllable && first <= 0xD7A3 && (first - kSyllable) % 28 == 0 ? first + (second - 0x11A7) : 0;
    }
    const StarterComposition *end = kStarterCompositions + kStarterCompositionCount;
    const StarterComposition *it = std::lower_bound(kStarterCompositions, end, std::pair(first, second),
                                                    [](const StarterComposition &c, std::pair<char32_t, char32_t> key) {
                                                        return std::pair(c.first, c.second) < key;
                                                    });
    return it != end && it->first == first && it->second == second ? it->composite : 0;
}

int markLedClass(char32_t cp)
{
    const MarkLedStarter *end = kMarkLedStarters + kMarkLedStarterCount;
    const MarkLedStarter *it = std::lower_bound(kMarkLedStarters, end, cp,
                                                [](const MarkLedStarter &m, char32_t c) { return m.cp < c; });
    return it != end && it->cp == cp ? it->lastClass : 0;
}

QStringView decompOf(char32_t cp)
{
    const Decomp *end = kDecomp + kDecompCount;
    const Decomp *it = std::lower_bound(kDecomp, end, cp,
                                        [](const Decomp &d, char32_t c) { return d.cp < c; });
    return it != end && it->cp == cp ? view(it->seq) : QStringView();
}

const Word *findWord(const Word *words, std::size_t count, QStringView key)
{
    const Word *end = words + count;
    const Word *it = std::lower_bound(words, end, key, [](const Word &w, QStringView k) {
        return view(w.key).compare(k) < 0;
    });
    return it != end && view(it->key) == key ? it : nullptr;
}

const AlphaRule *findRule(AlphaRange range, char32_t first, char32_t second)
{
    const AlphaRule *begin = kAlphaRules + range.begin;
    const AlphaRule *end = begin + range.count;
    const AlphaRule *it = std::lower_bound(begin, end, std::pair(first, second),
                                           [](const AlphaRule &r, std::pair<char32_t, char32_t> key) {
                                               return std::pair(r.first, r.second) < key;
                                           });
    return it != end && it->first == first && it->second == second ? it : nullptr;
}

} // namespace Search::Fold::Engine
