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
#include <limits>
#include <thread>
#include <vector>

namespace Search {

namespace {

std::atomic<quint64> nextGeneration{0};

template <typename Size>
Size appendCapacity(Size current, Size required)
{
    if (required <= current) return current;
    const Size increment = std::max<Size>(Size{1}, current / 2);
    if (current > std::numeric_limits<Size>::max() - increment) return required;
    return std::max(required, current + increment);
}

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
    m_folders.clear();
    m_folderIds.clear();
    m_pathSegments.clear();
    m_pathOffsets.clear();
    m_pathOffsets.push_back(0);
    m_masks.reserve(static_cast<std::size_t>(m_records.size()));
    for (const SearchRecord &rec : m_records) {
        m_masks.push_back(recordNormMasks(rec));
        indexPath(rec.normPath);
    }
    m_generation = nextGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
}

void SearchIndex::append(QVector<SearchRecord> records)
{
    if (m_records.isEmpty()) {
        build(std::move(records));
        return;
    }
    const qsizetype requiredRecords = m_records.size() + records.size();
    if (requiredRecords > m_records.capacity()) {
        m_records.reserve(appendCapacity(m_records.capacity(), requiredRecords));
    }
    const std::size_t requiredMasks = m_masks.size() + static_cast<std::size_t>(records.size());
    if (requiredMasks > m_masks.capacity()) {
        m_masks.reserve(appendCapacity(m_masks.capacity(), requiredMasks));
    }
    for (SearchRecord &rec : records) {
        m_masks.push_back(recordNormMasks(rec));
        indexPath(rec.normPath);
        m_records.push_back(std::move(rec));
    }
    m_generation = nextGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
}

void SearchIndex::clear()
{
    m_records = QVector<SearchRecord>{};
    m_masks.clear();
    m_masks.shrink_to_fit();
    m_folders.clear();
    m_folders.shrink_to_fit();
    m_folderIds.clear();
    m_pathSegments.clear();
    m_pathSegments.shrink_to_fit();
    m_pathOffsets.clear();
    m_pathOffsets.shrink_to_fit();
    m_pathOffsets.push_back(0);
    m_generation = nextGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
}

void SearchIndex::indexPath(const QString &normPath)
{
    const QStringView path(normPath);
    for (qsizetype segmentStart = 0; segmentStart <= path.size();) {
        qsizetype segmentEnd = path.indexOf(Fold::kSegmentSeparator, segmentStart);
        if (segmentEnd < 0) segmentEnd = path.size();
        int parent = -1;
        qsizetype componentStart = segmentStart;
        qsizetype slash = path.indexOf(QLatin1Char('/'), componentStart);
        while (slash >= 0 && slash < segmentEnd) {
            if (slash > componentStart) {
                const QString component = path.mid(componentStart, slash - componentStart).toString();
                const QPair<int, QString> key{parent, component};
                const auto it = m_folderIds.constFind(key);
                if (it != m_folderIds.cend()) {
                    parent = *it;
                } else {
                    const int id = static_cast<int>(m_folders.size());
                    m_folders.push_back({parent, component, normCharacterMask(component)});
                    m_folderIds.insert(key, id);
                    parent = id;
                }
            }
            componentStart = slash + 1;
            slash = path.indexOf(QLatin1Char('/'), componentStart);
        }
        m_pathSegments.push_back({parent, static_cast<int>(componentStart), static_cast<int>(segmentEnd),
                                  normCharacterMask(path.mid(componentStart, segmentEnd - componentStart))});
        if (segmentEnd == path.size()) break;
        segmentStart = segmentEnd + 1;
    }
    m_pathOffsets.push_back(m_pathSegments.size());
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
                                          int *totalMatches, QVector<int> *matchingIndices,
                                          const QVector<int> *candidateIndices,
                                          const std::function<bool()> &cancelled,
                                          int *hiddenMatches) const
{
    Q_ASSERT(!matchingIndices || matchingIndices != candidateIndices);
    if (totalMatches) *totalMatches = 0;
    if (hiddenMatches) *hiddenMatches = 0;
    if (matchingIndices) matchingIndices->clear();
    if (cancelled && cancelled()) return {};
    if (query.isEmpty() || m_records.isEmpty()) return {};

    const int n = candidateIndices ? static_cast<int>(candidateIndices->size())
                                   : static_cast<int>(m_records.size());
    const auto termMasks = termCharacterMasks(query);

    // Each text term that uses the folder table decides every folder once:
    // it holds the term itself or inherits its parent's answer (folders are
    // numbered parents first). Component scores are computed only for the
    // few records that get scored.
    const auto subsequence = [](QStringView hay, QStringView needle) {
        qsizetype pos = 0;
        for (QChar ch : needle) {
            pos = hay.indexOf(ch, pos);
            if (pos < 0) return false;
            ++pos;
        }
        return true;
    };
    struct FolderTerm {
        bool fuzzy = false;
        std::vector<quint8> holds;
    };
    std::vector<FolderTerm> folderTerms(static_cast<std::size_t>(query.terms.size()));
    for (qsizetype t = 0; t < query.terms.size(); ++t) {
        const Term &term = query.terms[t];
        if (!usesPathTree(term, fuzzyMode)) continue;
        FolderTerm &state = folderTerms[static_cast<std::size_t>(t)];
        state.fuzzy = fuzzyMode && !term.forceExact;
        state.holds.reserve(m_folders.size());
        for (const FolderNode &folder : m_folders) {
            const bool own = (folder.mask & termMasks[t]) == termMasks[t]
                && (state.fuzzy ? subsequence(folder.component, term.text)
                                : folder.component.contains(term.text));
            state.holds.push_back(own || (folder.parent >= 0
                                          && state.holds[static_cast<std::size_t>(folder.parent)]));
        }
    }

    // Every worker keeps its own best kMaxResults ranks. Once a worker's heap
    // is full, its worst rank is a floor no shown result can fall below; it is
    // published so every worker skips scoring a record whose best possible rank
    // (its bound) is lower. Every record is still decided, so the total count
    // stays exact. A stale floor only costs extra scoring.
    std::atomic<RankKey> floor{0};

    // Aligned so workers' counters, written once per match, never share a line.
    struct alignas(64) Slice {
        std::vector<RankKey> heap;
        QVector<int> indices;
        int count = 0;
        int hidden = 0;
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
        for (int pos = lo; pos < hi; ++pos) {
            if ((pos & 2047) == 0 && cancelled && cancelled()) return;
            const int i = candidateIndices ? candidateIndices->at(pos) : pos;
            Q_ASSERT(i >= 0 && i < m_records.size());
            const SearchRecord &rec = m_records[i];

            // Exclusions stay out of ranking, but query matches among them are
            // counted for the GUI status when requested.
            bool excluded = false;
            for (const ExcludeMatcher &ex : excludes) {
                if (ex.matches(rec)) { excluded = true; break; }
            }
            if (excluded && !hiddenMatches && !matchingIndices) continue;

            const NormMasks &masks = m_masks[static_cast<std::size_t>(i)];
            const auto lookup = [&](qsizetype t, bool score) {
                const FolderTerm &state = folderTerms[static_cast<std::size_t>(t)];
                const QStringView needle(query.terms[t].text);
                const QStringView path(rec.normPath);
                int best = INT_MIN;
                const auto consider = [&](QStringView component) {
                    const auto result = fuzzyMatchV2(component, needle, true, false);
                    if (result.matched()) best = std::max(best, result.score);
                };
                for (std::size_t s = m_pathOffsets[static_cast<std::size_t>(i)];
                     s < m_pathOffsets[static_cast<std::size_t>(i) + 1]; ++s) {
                    const PathSegment &segment = m_pathSegments[s];
                    const bool filenameFits = (segment.filenameMask & termMasks[t]) == termMasks[t];
                    const QStringView filename = path.mid(segment.filenameStart,
                                                          segment.filenameEnd - segment.filenameStart);
                    if (!score) {
                        if (segment.leaf >= 0 && state.holds[static_cast<std::size_t>(segment.leaf)]) return 0;
                        if (filenameFits && (state.fuzzy ? subsequence(filename, needle)
                                                         : filename.contains(needle))) return 0;
                        continue;
                    }
                    if (filenameFits) consider(filename);
                    for (int f = segment.leaf; f >= 0 && state.holds[static_cast<std::size_t>(f)];
                         f = m_folders[static_cast<std::size_t>(f)].parent) {
                        consider(m_folders[static_cast<std::size_t>(f)].component);
                    }
                }
                return best;
            };
            const PathTermLookup pathLookup(lookup);
            bool exact = false;
            const int bound = matchSearchRecordBound(rec, masks, query, termMasks,
                                                     fuzzyMode, &exact, &pathLookup);
            if (bound == INT_MIN) continue;
            if (excluded) {
                // Counts use the same bound as visible records below the heap floor.
                ++out.hidden;
                if (matchingIndices) out.indices.append(i);
                continue;
            }
            ++out.count;
            bool matched = true;
            if (exact) {
                keep(rankKey(bound, i));
            } else if (rankKey(bound, i) > floor.load(std::memory_order_relaxed)) {
                const int score = scoreSearchRecord(rec, masks, query, termMasks,
                                                    fuzzyMode, &pathLookup);
                if (score == INT_MIN) {
                    // The bound admitted what the scorer rejects: never let a
                    // disagreement surface as a counted, unscorable match.
                    --out.count;
                    matched = false;
                } else {
                    keep(rankKey(score, i));
                }
            }
            if (matched && matchingIndices) out.indices.append(i);
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

    if (cancelled && cancelled()) return {};

    // Every shown result survives in some worker's heap: a record outranked
    // there by kMaxResults others is outranked globally too.
    std::vector<RankKey> ranked;
    int total = 0;
    int hidden = 0;
    for (const Slice &slice : slices) {
        total += slice.count;
        hidden += slice.hidden;
        ranked.insert(ranked.end(), slice.heap.begin(), slice.heap.end());
        if (matchingIndices) *matchingIndices += slice.indices;
    }
    if (totalMatches) *totalMatches = total;
    if (hiddenMatches) *hiddenMatches = hidden;

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
        if (field == HighlightField::Path && term.kind == TermKind::FreeText
            && fuzzyMode && !term.forceExact && usesPathTree(term, fuzzyMode)) {
            int bestScore = INT_MIN;
            QVector<int> bestPositions;
            for (const Fold::FoldResult &folded : folds) {
                const QStringView path(folded.text);
                for (qsizetype start = 0; start <= path.size();) {
                    qsizetype end = path.indexOf(QLatin1Char('/'), start);
                    if (end < 0) end = path.size();
                    const auto result = fuzzyMatchV2(path.mid(start, end - start),
                                                     QStringView(term.text), true, true);
                    if (result.matched() && result.score > bestScore) {
                        bestScore = result.score;
                        bestPositions.clear();
                        for (int p : result.positions) {
                            const qsizetype foldedIndex = start + p;
                            if (foldedIndex >= 0 && foldedIndex < folded.srcIndex.size())
                                bestPositions.append(folded.srcIndex.at(foldedIndex));
                        }
                    }
                    if (end == path.size()) break;
                    start = end + 1;
                }
            }
            positions += bestPositions;
            continue;
        }
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
