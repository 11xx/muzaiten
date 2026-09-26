// The compiled fold tables and the lookups over them.

#include "search/fold/FoldEngine.h"

#include <algorithm>

namespace Search::Fold::Engine {

#include "FoldTables.inc"

std::uint32_t astralEntry(char32_t cp)
{
    for (std::size_t k = 0; k < kAstralHanCount; ++k) {
        if (cp >= kAstralHan[k].first && cp <= kAstralHan[k].last) {
            return (std::uint32_t(Kind::Keep)) | (std::uint32_t(Script::Han) << 3);
        }
    }
    const AstralEntry *end = kAstral + kAstralCount;
    const AstralEntry *it = std::lower_bound(kAstral, end, cp,
                                             [](const AstralEntry &e, char32_t c) { return e.cp < c; });
    return it != end && it->cp == cp ? it->entry : std::uint32_t(Kind::Keep);
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
