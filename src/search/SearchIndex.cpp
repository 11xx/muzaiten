#include "search/SearchIndex.h"

#include "search/FuzzyMatch.h"
#include "search/SearchQuery.h"
#include "search/SearchRecord.h"

#include <algorithm>
#include <climits>

namespace Search {

namespace {

// ---- numeric comparison helper --------------------------------------------

bool compareNumeric(qint64 trackValue, CompareOp op, qint64 queryValue)
{
    switch (op) {
    case CompareOp::Eq: return trackValue == queryValue;
    case CompareOp::Ne: return trackValue != queryValue;
    case CompareOp::Lt: return trackValue <  queryValue;
    case CompareOp::Le: return trackValue <= queryValue;
    case CompareOp::Gt: return trackValue >  queryValue;
    case CompareOp::Ge: return trackValue >= queryValue;
    }
    return false;
}

int yearFromDate(const QString &date)
{
    if (date.length() >= 4) {
        bool ok;
        const int y = date.left(4).toInt(&ok);
        if (ok) return y;
    }
    return 0;
}

bool isBoundaryChar(QChar c)
{
    return c.isSpace() || c == QLatin1Char('/') || c == QLatin1Char('-')
        || c == QLatin1Char('_') || c == QLatin1Char('.') || c == QLatin1Char('(')
        || c == QLatin1Char('[') || c == QLatin1Char(',');
}

// Cheap exact-substring score for one (already-lowercased) field.
// Returns INT_MIN when the field does not match the term.
int exactFieldScore(const QString &norm, const Term &term, int weight)
{
    const QString &needle = term.text;
    if (norm.isEmpty() || needle.isEmpty()) return INT_MIN;

    if (term.prefixAnchor && term.suffixAnchor) {
        return norm == needle ? weight + 40 : INT_MIN;
    }
    if (term.prefixAnchor) {
        return norm.startsWith(needle) ? weight + 30 : INT_MIN;
    }
    if (term.suffixAnchor) {
        return norm.endsWith(needle) ? weight + 12 : INT_MIN;
    }

    const int idx = static_cast<int>(norm.indexOf(needle));
    if (idx < 0) return INT_MIN;

    int s = weight;
    if (idx == 0) {
        s += 25;
    } else if (isBoundaryChar(norm[idx - 1])) {
        s += 18;
    }
    s += std::max(0, 20 - idx);  // earlier matches rank slightly higher
    return s;
}

// Two-pointer subsequence test (both args already lowercased) — a cheap reject
// before paying for the fuzzy DP.
bool isSubsequence(const QString &hay, const QString &needle)
{
    int j = 0;
    const int n = static_cast<int>(needle.length());
    const int h = static_cast<int>(hay.length());
    if (n > h) return false;
    for (int i = 0; i < h && j < n; ++i) {
        if (hay[i] == needle[j]) ++j;
    }
    return j == n;
}

int fuzzyFieldScore(const QString &norm, const QString &needle, int weight)
{
    if (!isSubsequence(norm, needle)) return INT_MIN;
    // Both strings are lowercase already, so case-sensitive matching is correct
    // and avoids per-character case folding. No positions in the hot path.
    const MatchResult mr = fuzzyMatchV2(norm, needle, /*caseSensitive=*/true, /*withPositions=*/false);
    return mr.matched() ? weight + mr.score : INT_MIN;
}

int fieldWeight(HighlightField f)
{
    switch (f) {
    case HighlightField::Title:  return 400;
    case HighlightField::Artist: return 300;
    case HighlightField::Album:  return 200;
    case HighlightField::Path:   return 60;
    }
    return 100;
}

// Best text score for a term across the fields it applies to. Returns INT_MIN
// if no field matched.
int textTermScore(const SearchRecord &rec, const Term &term, bool fuzzyMode)
{
    const bool exact = term.forceExact || !fuzzyMode;
    auto fieldScore = [&](const QString &norm, int weight) {
        return exact ? exactFieldScore(norm, term, weight)
                     : fuzzyFieldScore(norm, term.text, weight);
    };

    int best = INT_MIN;
    auto consider = [&](int s) { if (s > best) best = s; };

    switch (term.kind) {
    case TermKind::TitleText:
        consider(fieldScore(rec.normTitle, fieldWeight(HighlightField::Title)));
        break;
    case TermKind::ArtistText:
        consider(fieldScore(rec.normArtist, fieldWeight(HighlightField::Artist)));
        consider(fieldScore(rec.normAlbumArtist, fieldWeight(HighlightField::Artist)));
        break;
    case TermKind::AlbumArtistText:
        consider(fieldScore(rec.normAlbumArtist, fieldWeight(HighlightField::Artist)));
        break;
    case TermKind::AlbumText:
        consider(fieldScore(rec.normAlbum, fieldWeight(HighlightField::Album)));
        break;
    case TermKind::PathText:
        consider(fieldScore(rec.normPath, fieldWeight(HighlightField::Path)));
        break;
    case TermKind::FilenameText:
        consider(fieldScore(rec.normFilename, fieldWeight(HighlightField::Path)));
        break;
    case TermKind::FreeText:
    default:
        consider(fieldScore(rec.normTitle, fieldWeight(HighlightField::Title)));
        consider(fieldScore(rec.normArtist, fieldWeight(HighlightField::Artist)));
        consider(fieldScore(rec.normAlbumArtist, fieldWeight(HighlightField::Artist)));
        consider(fieldScore(rec.normAlbum, fieldWeight(HighlightField::Album)));
        consider(fieldScore(rec.normPath, fieldWeight(HighlightField::Path)));
        break;
    }
    return best;
}

// Returns true if the term matches the record; on match, *score holds its
// contribution. Numeric/codec tokens are direct comparisons.
bool termMatches(const SearchRecord &rec, const Term &term, bool fuzzyMode, int *score)
{
    *score = 0;
    switch (term.kind) {
    case TermKind::Year: {
        const int y = yearFromDate(rec.date);
        const bool m = y > 0 && compareNumeric(y, term.op, term.numericValue);
        if (m) *score = 60;
        return m;
    }
    case TermKind::Rating: {
        const bool m = rec.rating0To100 >= 0 && compareNumeric(rec.rating0To100, term.op, term.numericValue);
        if (m) *score = 60;
        return m;
    }
    case TermKind::DurationMs: {
        const bool m = rec.durationMs > 0 && compareNumeric(rec.durationMs, term.op, term.numericValue);
        if (m) *score = 60;
        return m;
    }
    case TermKind::SampleRateHz: {
        const bool m = rec.sampleRateHz > 0 && compareNumeric(rec.sampleRateHz, term.op, term.numericValue);
        if (m) *score = 60;
        return m;
    }
    case TermKind::BitrateKbps: {
        const bool m = rec.bitrateKbps > 0 && compareNumeric(rec.bitrateKbps, term.op, term.numericValue);
        if (m) *score = 60;
        return m;
    }
    case TermKind::Channels: {
        const bool m = rec.channels > 0 && compareNumeric(rec.channels, term.op, term.numericValue);
        if (m) *score = 60;
        return m;
    }
    case TermKind::Extension: {
        const bool m = !rec.codec.isEmpty() && rec.codec == term.text;
        if (m) *score = 80;
        return m;
    }
    case TermKind::CodecText: {
        const bool m = !rec.codec.isEmpty() && rec.codec.contains(term.text);
        if (m) *score = 60;
        return m;
    }
    default: {
        const int s = textTermScore(rec, term, fuzzyMode);
        if (s == INT_MIN) return false;
        *score = s;
        return true;
    }
    }
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

        int total = 0;
        bool ok = true;
        for (const Term &term : query.terms) {
            int s = 0;
            bool m = termMatches(rec, term, fuzzyMode, &s);
            if (term.negate) { m = !m; s = 0; }
            if (!m) { ok = false; break; }
            total += s;
        }
        if (ok) candidates.push_back({i, total});
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
