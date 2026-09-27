#include "search/SearchIndex.h"

#include "search/FuzzyMatch.h"
#include "search/SearchMatcher.h"
#include "search/SearchQuery.h"
#include "search/SearchRecord.h"
#include "search/fold/Fold.h"

#include <QThread>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace Search {

namespace {

// ---- highlight helpers (delegate, on-screen rows only) --------------------

bool termAppliesTo(const Term &term, HighlightField field)
{
    if (term.negate) return false;
    if (term.kind == TermKind::FreeText) return true;
    switch (field) {
    case HighlightField::Title:  return term.kind == TermKind::TitleText;
    case HighlightField::Artist: return term.kind == TermKind::ArtistText
                                      || term.kind == TermKind::AlbumArtistText;
    case HighlightField::Album:  return term.kind == TermKind::AlbumText;
    case HighlightField::Path:   return term.kind == TermKind::PathText
                                      || term.kind == TermKind::FilenameText;
    }
    return false;
}

} // namespace

// ---- SearchIndex -----------------------------------------------------------

void SearchIndex::build(QVector<SearchRecord> records)
{
    m_records = std::move(records);
    m_masks.clear();
    m_masks.reserve(static_cast<std::size_t>(m_records.size()));
    for (const SearchRecord &rec : m_records) m_masks.push_back(recordNormMasks(rec));
}

void SearchIndex::append(QVector<SearchRecord> records)
{
    if (m_records.isEmpty()) {
        build(std::move(records));
        return;
    }
    m_records.reserve(m_records.size() + records.size());
    m_masks.reserve(static_cast<std::size_t>(m_records.size() + records.size()));
    for (SearchRecord &rec : records) {
        m_masks.push_back(recordNormMasks(rec));
        m_records.push_back(std::move(rec));
    }
}

void SearchIndex::clear()
{
    m_records.clear();
    m_records.squeeze();
    m_masks.clear();
    m_masks.shrink_to_fit();
}

namespace {

// A result's rank as one integer: higher score first, then lower record
// index, the order results are returned in. Comparing keys compares ranks.
using RankKey = quint64;

RankKey rankKey(int score, int index)
{
    const quint64 biased = static_cast<quint64>(static_cast<qint64>(score) + (qint64{1} << 31));
    return (biased << 32) | (0xFFFFFFFFu - static_cast<quint32>(index));
}

int keyScore(RankKey key)
{
    return static_cast<int>(static_cast<qint64>(key >> 32) - (qint64{1} << 31));
}

int keyIndex(RankKey key)
{
    return static_cast<int>(0xFFFFFFFFu - static_cast<quint32>(key & 0xFFFFFFFFu));
}

} // namespace

QVector<ScoredResult> SearchIndex::match(const SearchQuery &query, bool fuzzyMode,
                                          const ExclusionSet &excludes,
                                          int *totalMatches) const
{
    if (totalMatches) *totalMatches = 0;
    if (query.isEmpty() || m_records.isEmpty()) return {};

    const int n = static_cast<int>(m_records.size());
    const auto termMasks = termCharacterMasks(query);

    // Every worker keeps its own best kMaxResults ranks. Once a worker's heap
    // is full, its worst rank is a floor no shown result can fall below; it is
    // published so every worker skips scoring a record whose best possible rank
    // (its bound) is lower. Every record is still decided, so the total count
    // stays exact. A stale floor only costs extra scoring.
    std::atomic<RankKey> floor{0};

    // Aligned so workers' counters, written once per match, never share a line.
    struct alignas(64) Slice {
        std::vector<RankKey> heap;
        int count = 0;
    };

    const auto scanSlice = [&](int lo, int hi, Slice &out) {
        auto &heap = out.heap;
        heap.reserve(static_cast<std::size_t>(std::min(kMaxResults, hi - lo)));
        const auto keep = [&](RankKey key) {
            if (static_cast<int>(heap.size()) < kMaxResults) {
                heap.push_back(key);
                std::push_heap(heap.begin(), heap.end(), std::greater<>());
            } else if (key > heap.front()) {
                std::pop_heap(heap.begin(), heap.end(), std::greater<>());
                heap.back() = key;
                std::push_heap(heap.begin(), heap.end(), std::greater<>());
            } else {
                return;
            }
            if (static_cast<int>(heap.size()) == kMaxResults) {
                RankKey seen = floor.load(std::memory_order_relaxed);
                while (heap.front() > seen
                       && !floor.compare_exchange_weak(seen, heap.front(), std::memory_order_relaxed)) {
                }
            }
        };
        for (int i = lo; i < hi; ++i) {
            const SearchRecord &rec = m_records[i];

            // Exclusions are an early reject, before any relevance scoring, so
            // they only reduce work and never consume the result cap.
            bool excluded = false;
            for (const ExcludeMatcher &ex : excludes) {
                if (ex.matches(rec)) { excluded = true; break; }
            }
            if (excluded) continue;

            const NormMasks &masks = m_masks[static_cast<std::size_t>(i)];
            bool exact = false;
            const int bound = matchSearchRecordBound(rec, masks, query, termMasks, fuzzyMode, &exact);
            if (bound == INT_MIN) continue;
            ++out.count;
            if (exact) {
                keep(rankKey(bound, i));
            } else if (rankKey(bound, i) > floor.load(std::memory_order_relaxed)) {
                const int score = scoreSearchRecord(rec, masks, query, termMasks, fuzzyMode);
                if (score == INT_MIN) {
                    // The bound admitted what the scorer rejects: never let a
                    // disagreement surface as a counted, unscorable match.
                    --out.count;
                } else {
                    keep(rankKey(score, i));
                }
            }
        }
    };

    // Fan the scan across cores, leaving one for the GUI and audio threads,
    // and stay serial for small indexes where spawning would not pay off.
    int workers = std::clamp(QThread::idealThreadCount() - 1, 1, 12);
    constexpr int kMinPerThread = 2500;
    workers = std::clamp(n / kMinPerThread, 1, workers);

    std::vector<Slice> slices(static_cast<std::size_t>(workers));
    {
        std::vector<std::thread> threads;
        threads.reserve(static_cast<std::size_t>(workers - 1));
        const int chunk = (n + workers - 1) / workers;
        for (int w = 0; w + 1 < workers; ++w) {
            const int lo = std::min(n, w * chunk);
            const int hi = std::min(n, lo + chunk);
            threads.emplace_back(scanSlice, lo, hi, std::ref(slices[static_cast<std::size_t>(w)]));
        }
        scanSlice(std::min(n, (workers - 1) * chunk), n, slices.back());
        for (std::thread &t : threads) t.join();
    }

    // Every shown result survives in some worker's heap: a record outranked
    // there by kMaxResults others is outranked globally too.
    std::vector<RankKey> ranked;
    int total = 0;
    for (const Slice &slice : slices) {
        total += slice.count;
        ranked.insert(ranked.end(), slice.heap.begin(), slice.heap.end());
    }
    if (totalMatches) *totalMatches = total;

    const int retained = std::min(static_cast<int>(ranked.size()), kMaxResults);
    std::partial_sort(ranked.begin(), ranked.begin() + retained, ranked.end(), std::greater<>());

    QVector<ScoredResult> results;
    results.reserve(retained);
    for (int r = 0; r < retained; ++r) {
        const int index = keyIndex(ranked[static_cast<std::size_t>(r)]);
        results.push_back({m_records[index], keyScore(ranked[static_cast<std::size_t>(r)]), index});
    }
    return results;
}

// ---- highlight positions (delegate) ---------------------------------------

namespace {

// Positions in folded text `hay` that `term` matches, or none.
QVector<int> termPositions(const QString &hay, const Term &term, bool fuzzyMode)
{
    const QString &needle = term.text;
    QVector<int> positions;
    const auto span = [&](qsizetype start) {
        for (qsizetype k = 0; k < needle.length(); ++k) positions.append(static_cast<int>(start + k));
    };
    const bool exact = term.forceExact || !fuzzyMode;
    if (!exact) {
        const MatchResult mr = fuzzyMatchV2(hay, needle, /*caseSensitive=*/true, /*withPositions=*/true);
        if (mr.matched()) positions = mr.positions;
        return positions;
    }
    if (term.prefixAnchor && term.suffixAnchor) {
        if (hay == needle) span(0);
    } else if (term.prefixAnchor) {
        if (hay.startsWith(needle)) span(0);
    } else if (term.suffixAnchor) {
        if (hay.endsWith(needle)) span(hay.length() - needle.length());
    } else {
        for (qsizetype idx = hay.indexOf(needle); idx >= 0; idx = hay.indexOf(needle, idx + 1)) span(idx);
    }
    return positions;
}

} // namespace

QVector<int> highlightPositions(const QString &text, const SearchQuery &query,
                                 HighlightField field, bool fuzzyMode)
{
    if (text.isEmpty() || query.isEmpty()) return {};

    // Match in folded space (the needle is already folded by the parser), then
    // project the matched positions back onto the original display string via
    // the fold's source-index map — folding can change length (ß→ss, 三→san,
    // dropped combining marks), so folded indices are not display indices. A
    // term that misses the primary fold is looked up in the alternate readings
    // the index also matched (한국어 found by "hangugeo").
    const QVector<Fold::FoldResult> folds = Fold::foldAll(text);

    QVector<int> positions;
    for (const Term &term : query.terms) {
        if (!termAppliesTo(term, field) || term.text.isEmpty()) continue;
        for (const Fold::FoldResult &folded : folds) {
            const QVector<int> foldedPos = termPositions(folded.text, term, fuzzyMode);
            if (foldedPos.isEmpty()) continue;
            for (const int p : foldedPos) {
                if (p >= 0 && p < folded.srcIndex.size()) positions.append(folded.srcIndex.at(p));
            }
            break;
        }
    }
    std::sort(positions.begin(), positions.end());
    positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
    return positions;
}

} // namespace Search
