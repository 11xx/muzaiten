#include "search/SearchIndex.h"

#include "search/FuzzyMatch.h"
#include "search/SearchMatcher.h"
#include "search/SearchQuery.h"
#include "search/SearchRecord.h"

#include <algorithm>
#include <climits>

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
    QVector<Candidate> candidates;
    candidates.reserve(1024);

    const int n = static_cast<int>(m_records.size());
    for (int i = 0; i < n; ++i) {
        const SearchRecord &rec = m_records[i];

        // Exclusions are an early reject, before any relevance scoring, so they
        // only reduce work and never consume the result cap.
        bool excluded = false;
        for (const ExcludeMatcher &ex : excludes) {
            if (ex.matches(rec)) { excluded = true; break; }
        }
        if (excluded) continue;

        const int score = matchSearchRecord(rec, query, fuzzyMode);
        if (score != INT_MIN) candidates.push_back({i, score});
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
    QVector<int> positions;
    if (text.isEmpty() || query.isEmpty()) return positions;

    const QString lower = text.toLower();

    for (const Term &term : query.terms) {
        if (!termAppliesTo(term, field)) continue;
        const QString &needle = term.text;
        if (needle.isEmpty()) continue;

        const bool exact = term.forceExact || !fuzzyMode;
        if (exact) {
            if (term.prefixAnchor && term.suffixAnchor) {
                if (lower == needle) {
                    for (int k = 0; k < needle.length(); ++k) positions.append(k);
                }
                continue;
            }
            if (term.prefixAnchor) {
                if (lower.startsWith(needle)) {
                    for (int k = 0; k < needle.length(); ++k) positions.append(k);
                }
                continue;
            }
            if (term.suffixAnchor) {
                if (lower.endsWith(needle)) {
                    const int start = static_cast<int>(lower.length() - needle.length());
                    for (int k = 0; k < needle.length(); ++k) positions.append(start + k);
                }
                continue;
            }
            // Highlight every occurrence.
            int idx = static_cast<int>(lower.indexOf(needle));
            while (idx >= 0) {
                for (int k = 0; k < needle.length(); ++k) positions.append(idx + k);
                idx = static_cast<int>(lower.indexOf(needle, idx + 1));
            }
        } else {
            const MatchResult mr = fuzzyMatchV2(lower, needle, /*caseSensitive=*/true, /*withPositions=*/true);
            if (mr.matched()) positions += mr.positions;
        }
    }
    return positions;
}

} // namespace Search
