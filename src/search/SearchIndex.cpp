#include "search/SearchIndex.h"

#include "search/FuzzyMatch.h"
#include "search/SearchMatcher.h"
#include "search/SearchQuery.h"
#include "search/SearchRecord.h"
#include "search/fold/Fold.h"

#include <QThread>

#include <algorithm>
#include <climits>
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
}

void SearchIndex::append(QVector<SearchRecord> records)
{
    if (m_records.isEmpty()) {
        m_records = std::move(records);
        return;
    }
    m_records.reserve(m_records.size() + records.size());
    for (SearchRecord &rec : records) {
        m_records.push_back(std::move(rec));
    }
}

void SearchIndex::clear()
{
    m_records.clear();
    m_records.squeeze();
}

QVector<ScoredResult> SearchIndex::match(const SearchQuery &query, bool fuzzyMode,
                                          const ExclusionSet &excludes,
                                          int *totalMatches) const
{
    if (totalMatches) *totalMatches = 0;
    if (query.isEmpty() || m_records.isEmpty()) return {};

    // Phase 1: cheap boolean filter + lightweight score. No positions, no
    // per-record allocations — only record indices are collected.
    struct Candidate { int index; int score; };
    const int n = static_cast<int>(m_records.size());

    // Score the half-open slice [lo, hi) into `out`. Records are scored
    // independently, so slices run on separate threads with no shared mutable
    // state — fuzzyMatchV2's scratch is thread_local, one slab per thread.
    const auto scoreRange = [&](int lo, int hi, QVector<Candidate> &out) {
        for (int i = lo; i < hi; ++i) {
            const SearchRecord &rec = m_records[i];

            // Exclusions are an early reject, before any relevance scoring, so
            // they only reduce work and never consume the result cap.
            bool excluded = false;
            for (const ExcludeMatcher &ex : excludes) {
                if (ex.matches(rec)) { excluded = true; break; }
            }
            if (excluded) continue;

            const int score = matchSearchRecord(rec, query, fuzzyMode);
            if (score != INT_MIN) out.push_back({i, score});
        }
    };

    // Fan the scan across cores. Fuzzy scoring is an order of magnitude heavier
    // than exact, and a 90k-track library is the dominant per-keystroke cost, so
    // this scan is the one place worth the thread spawn. Leave a core for the
    // GUI/audio threads, and stay serial for small indexes where the spawn and
    // join wouldn't pay off.
    int workers = std::clamp(QThread::idealThreadCount() - 1, 1, 12);
    constexpr int kMinPerThread = 2500;
    workers = std::clamp(n / kMinPerThread, 1, workers);

    QVector<Candidate> candidates;
    if (workers <= 1) {
        candidates.reserve(1024);
        scoreRange(0, n, candidates);
    } else {
        QVector<QVector<Candidate>> partials(workers);
        std::vector<std::thread> threads;
        threads.reserve(static_cast<std::size_t>(workers - 1));
        const int chunk = (n + workers - 1) / workers;
        for (int w = 0; w + 1 < workers; ++w) {
            const int lo = w * chunk;
            const int hi = std::min(n, lo + chunk);
            threads.emplace_back(scoreRange, lo, hi, std::ref(partials[w]));
        }
        scoreRange((workers - 1) * chunk, n, partials[workers - 1]); // last slice here

        for (std::thread &t : threads) {
            t.join();
        }

        // Concatenate in slice order: chunks are ascending contiguous ranges, so
        // the merged list stays index-ordered and ties break exactly as before.
        int total = 0;
        for (const QVector<Candidate> &p : partials) {
            total += static_cast<int>(p.size());
        }
        candidates.reserve(total);
        for (const QVector<Candidate> &p : partials) {
            candidates += p;
        }
    }

    if (totalMatches) *totalMatches = static_cast<int>(candidates.size());

    // Phase 2: keep only the top-scoring slice, ordered by score desc.
    const int keep = std::min(static_cast<int>(candidates.size()), kMaxResults);
    const auto cmp = [](const Candidate &a, const Candidate &b) {
        return a.score != b.score ? a.score > b.score : a.index < b.index;
    };
    if (keep < static_cast<int>(candidates.size())) {
        std::partial_sort(candidates.begin(), candidates.begin() + keep, candidates.end(), cmp);
        candidates.resize(keep);
    } else {
        std::sort(candidates.begin(), candidates.end(), cmp);
    }

    // Phase 3: materialize record copies only for the kept slice.
    QVector<ScoredResult> results;
    results.reserve(keep);
    for (const Candidate &c : candidates) {
        results.push_back({m_records[c.index], c.score});
    }
    return results;
}

// ---- highlight positions (delegate) ---------------------------------------

QVector<int> highlightPositions(const QString &text, const SearchQuery &query,
                                 HighlightField field, bool fuzzyMode)
{
    if (text.isEmpty() || query.isEmpty()) return {};

    // Match in folded space (the needle is already folded by the parser), then
    // project the matched positions back onto the original display string via
    // the fold's source-index map — folding can change length (ß→ss, 三→san,
    // dropped combining marks), so folded indices are not display indices.
    const Fold::FoldResult folded = Fold::fold(text);
    const QString &hay = folded.text;

    QVector<int> foldedPos;
    for (const Term &term : query.terms) {
        if (!termAppliesTo(term, field)) continue;
        const QString &needle = term.text;
        if (needle.isEmpty()) continue;

        const bool exact = term.forceExact || !fuzzyMode;
        if (exact) {
            if (term.prefixAnchor && term.suffixAnchor) {
                if (hay == needle) {
                    for (int k = 0; k < needle.length(); ++k) foldedPos.append(k);
                }
                continue;
            }
            if (term.prefixAnchor) {
                if (hay.startsWith(needle)) {
                    for (int k = 0; k < needle.length(); ++k) foldedPos.append(k);
                }
                continue;
            }
            if (term.suffixAnchor) {
                if (hay.endsWith(needle)) {
                    const int start = static_cast<int>(hay.length() - needle.length());
                    for (int k = 0; k < needle.length(); ++k) foldedPos.append(start + k);
                }
                continue;
            }
            // Highlight every occurrence.
            int idx = static_cast<int>(hay.indexOf(needle));
            while (idx >= 0) {
                for (int k = 0; k < needle.length(); ++k) foldedPos.append(idx + k);
                idx = static_cast<int>(hay.indexOf(needle, idx + 1));
            }
        } else {
            const MatchResult mr = fuzzyMatchV2(hay, needle, /*caseSensitive=*/true, /*withPositions=*/true);
            if (mr.matched()) foldedPos += mr.positions;
        }
    }

    QVector<int> positions;
    positions.reserve(foldedPos.size());
    const QVector<int> &src = folded.srcIndex;
    for (const int p : foldedPos) {
        if (p >= 0 && p < src.size()) positions.append(src.at(p));
    }
    std::sort(positions.begin(), positions.end());
    positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
    return positions;
}

} // namespace Search
