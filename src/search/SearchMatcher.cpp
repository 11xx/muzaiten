#include "search/SearchMatcher.h"

#include "search/FuzzyMatch.h"
#include "search/SearchRecord.h"

#include <algorithm>
#include <climits>

namespace Search {

namespace {

bool compareNumeric(qint64 trackValue, CompareOp op, qint64 queryValue)
{
    switch (op) {
    case CompareOp::Eq: return trackValue == queryValue;
    case CompareOp::Ne: return trackValue != queryValue;
    case CompareOp::Lt: return trackValue < queryValue;
    case CompareOp::Le: return trackValue <= queryValue;
    case CompareOp::Gt: return trackValue > queryValue;
    case CompareOp::Ge: return trackValue >= queryValue;
    }
    return false;
}

int yearFromDate(const QString &date)
{
    if (date.length() >= 4) {
        bool ok = false;
        const int y = date.left(4).toInt(&ok);
        if (ok) {
            return y;
        }
    }
    return 0;
}

bool isBoundaryChar(QChar c)
{
    return c.isSpace() || c == QLatin1Char('/') || c == QLatin1Char('-')
        || c == QLatin1Char('_') || c == QLatin1Char('.') || c == QLatin1Char('(')
        || c == QLatin1Char('[') || c == QLatin1Char(',');
}

int exactFieldScore(const QString &norm, const Term &term, int weight)
{
    const QString &needle = term.text;
    if (norm.isEmpty() || needle.isEmpty()) {
        return INT_MIN;
    }

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
    if (idx < 0) {
        return INT_MIN;
    }

    int s = weight;
    if (idx == 0) {
        s += 25;
    } else if (isBoundaryChar(norm[idx - 1])) {
        s += 18;
    }
    s += std::max(0, 20 - idx);
    return s;
}

bool isSubsequence(const QString &hay, const QString &needle)
{
    int j = 0;
    const int n = static_cast<int>(needle.length());
    const int h = static_cast<int>(hay.length());
    if (n > h) {
        return false;
    }
    for (int i = 0; i < h && j < n; ++i) {
        if (hay[i] == needle[j]) {
            ++j;
        }
    }
    return j == n;
}

int fuzzyFieldScore(const QString &norm, const QString &needle, int weight)
{
    if (!isSubsequence(norm, needle)) {
        return INT_MIN;
    }
    const MatchResult mr = fuzzyMatchV2(norm, needle, /*caseSensitive=*/true, /*withPositions=*/false);
    return mr.matched() ? weight + mr.score : INT_MIN;
}

int fieldWeight(MatchFieldRole role)
{
    switch (role) {
    case MatchFieldRole::Title:       return 400;
    case MatchFieldRole::Artist:      return 300;
    case MatchFieldRole::AlbumArtist: return 300;
    case MatchFieldRole::Album:       return 200;
    case MatchFieldRole::Path:        return 60;
    case MatchFieldRole::Filename:    return 60;
    case MatchFieldRole::Codec:       return 60;
    case MatchFieldRole::Free:        return 100;
    }
    return 100;
}

int scoreTextField(const QString &norm, const Term &term, bool fuzzyMode, int weight)
{
    return term.forceExact || !fuzzyMode ? exactFieldScore(norm, term, weight)
                                         : fuzzyFieldScore(norm, term.text, weight);
}

void consider(int score, int &best)
{
    if (score > best) {
        best = score;
    }
}

bool documentFieldApplies(const MatchField &field, const Term &term)
{
    switch (term.kind) {
    case TermKind::TitleText:
        return field.role == MatchFieldRole::Title || field.role == MatchFieldRole::Free;
    case TermKind::ArtistText:
        return field.role == MatchFieldRole::Artist || field.role == MatchFieldRole::AlbumArtist || field.role == MatchFieldRole::Free;
    case TermKind::AlbumArtistText:
        return field.role == MatchFieldRole::AlbumArtist || field.role == MatchFieldRole::Free;
    case TermKind::AlbumText:
        return field.role == MatchFieldRole::Album || field.role == MatchFieldRole::Free;
    case TermKind::PathText:
        return field.role == MatchFieldRole::Path || field.role == MatchFieldRole::Free;
    case TermKind::FilenameText:
        return field.role == MatchFieldRole::Filename || field.role == MatchFieldRole::Free;
    case TermKind::CodecText:
    case TermKind::Extension:
        return field.role == MatchFieldRole::Codec || field.role == MatchFieldRole::Free;
    case TermKind::FreeText:
        return true;
    default:
        return false;
    }
}

int textTermScore(const MatchDocument &doc, const Term &term, bool fuzzyMode)
{
    int best = INT_MIN;
    for (const MatchField &field : doc.fields) {
        if (!documentFieldApplies(field, term)) {
            continue;
        }
        const int weight = field.weight > 0 ? field.weight : fieldWeight(field.role);
        if (term.kind == TermKind::Extension) {
            if (!field.normText.isEmpty() && field.normText == term.text) {
                consider(weight + 20, best);
            }
            continue;
        }
        consider(scoreTextField(field.normText, term, fuzzyMode, weight), best);
    }
    return best;
}

bool numericTermMatches(const MatchDocument &doc, const Term &term, int *score)
{
    for (const MatchNumeric &numeric : doc.numeric) {
        if (numeric.kind == term.kind && compareNumeric(numeric.value, term.op, term.numericValue)) {
            *score = 60;
            return true;
        }
    }
    return false;
}

bool termMatches(const MatchDocument &doc, const Term &term, bool fuzzyMode, int *score)
{
    *score = 0;
    switch (term.kind) {
    case TermKind::Year:
    case TermKind::Rating:
    case TermKind::DurationMs:
    case TermKind::SampleRateHz:
    case TermKind::BitrateKbps:
    case TermKind::Channels:
        return numericTermMatches(doc, term, score);
    default: {
        const int s = textTermScore(doc, term, fuzzyMode);
        if (s == INT_MIN) {
            return false;
        }
        *score = s;
        return true;
    }
    }
}

int textTermScore(const SearchRecord &rec, const Term &term, bool fuzzyMode)
{
    auto fieldScore = [&](const QString &norm, MatchFieldRole role) {
        return scoreTextField(norm, term, fuzzyMode, fieldWeight(role));
    };

    int best = INT_MIN;
    switch (term.kind) {
    case TermKind::TitleText:
        consider(fieldScore(rec.normTitle, MatchFieldRole::Title), best);
        break;
    case TermKind::ArtistText:
        consider(fieldScore(rec.normArtist, MatchFieldRole::Artist), best);
        consider(fieldScore(rec.normAlbumArtist, MatchFieldRole::AlbumArtist), best);
        break;
    case TermKind::AlbumArtistText:
        consider(fieldScore(rec.normAlbumArtist, MatchFieldRole::AlbumArtist), best);
        break;
    case TermKind::AlbumText:
        consider(fieldScore(rec.normAlbum, MatchFieldRole::Album), best);
        break;
    case TermKind::PathText:
        consider(fieldScore(rec.normPath, MatchFieldRole::Path), best);
        break;
    case TermKind::FilenameText:
        consider(fieldScore(rec.normFilename, MatchFieldRole::Filename), best);
        break;
    case TermKind::FreeText:
    default:
        consider(fieldScore(rec.normTitle, MatchFieldRole::Title), best);
        consider(fieldScore(rec.normArtist, MatchFieldRole::Artist), best);
        consider(fieldScore(rec.normAlbumArtist, MatchFieldRole::AlbumArtist), best);
        consider(fieldScore(rec.normAlbum, MatchFieldRole::Album), best);
        consider(fieldScore(rec.normPath, MatchFieldRole::Path), best);
        break;
    }
    return best;
}

bool termMatches(const SearchRecord &rec, const Term &term, bool fuzzyMode, int *score)
{
    *score = 0;
    switch (term.kind) {
    case TermKind::Year: {
        const int y = yearFromDate(rec.date);
        const bool m = y > 0 && compareNumeric(y, term.op, term.numericValue);
        if (m) {
            *score = 60;
        }
        return m;
    }
    case TermKind::Rating: {
        const bool m = rec.rating0To100 >= 0 && compareNumeric(rec.rating0To100, term.op, term.numericValue);
        if (m) {
            *score = 60;
        }
        return m;
    }
    case TermKind::DurationMs: {
        const bool m = rec.durationMs > 0 && compareNumeric(rec.durationMs, term.op, term.numericValue);
        if (m) {
            *score = 60;
        }
        return m;
    }
    case TermKind::SampleRateHz: {
        const bool m = rec.sampleRateHz > 0 && compareNumeric(rec.sampleRateHz, term.op, term.numericValue);
        if (m) {
            *score = 60;
        }
        return m;
    }
    case TermKind::BitrateKbps: {
        const bool m = rec.bitrateKbps > 0 && compareNumeric(rec.bitrateKbps, term.op, term.numericValue);
        if (m) {
            *score = 60;
        }
        return m;
    }
    case TermKind::Channels: {
        const bool m = rec.channels > 0 && compareNumeric(rec.channels, term.op, term.numericValue);
        if (m) {
            *score = 60;
        }
        return m;
    }
    case TermKind::Extension: {
        const bool m = !rec.codec.isEmpty() && rec.codec == term.text;
        if (m) {
            *score = 80;
        }
        return m;
    }
    case TermKind::CodecText: {
        const bool m = !rec.codec.isEmpty() && rec.codec.contains(term.text);
        if (m) {
            *score = 60;
        }
        return m;
    }
    default: {
        const int s = textTermScore(rec, term, fuzzyMode);
        if (s == INT_MIN) {
            return false;
        }
        *score = s;
        return true;
    }
    }
}

int matchTerms(auto termMatcher, const SearchQuery &query)
{
    if (query.isEmpty()) {
        return INT_MIN;
    }

    int total = 0;
    for (const Term &term : query.terms) {
        int s = 0;
        bool m = termMatcher(term, &s);
        if (term.negate) {
            m = !m;
            s = 0;
        }
        if (!m) {
            return INT_MIN;
        }
        total += s;
    }
    return total;
}

} // namespace

int matchDocument(const MatchDocument &doc, const SearchQuery &query, bool fuzzyMode)
{
    return matchTerms([&](const Term &term, int *score) {
        return termMatches(doc, term, fuzzyMode, score);
    }, query);
}

QVector<PanelMatch> matchDocumentsInDisplayOrder(const QVector<MatchDocument> &docs,
                                                  const SearchQuery &query,
                                                  bool fuzzyMode)
{
    QVector<PanelMatch> matches;
    if (query.isEmpty()) {
        return matches;
    }
    matches.reserve(docs.size());
    for (const MatchDocument &doc : docs) {
        const int score = matchDocument(doc, query, fuzzyMode);
        if (score != INT_MIN) {
            matches.push_back({doc.row, score});
        }
    }
    return matches;
}

int matchSearchRecord(const SearchRecord &rec, const SearchQuery &query, bool fuzzyMode)
{
    return matchTerms([&](const Term &term, int *score) {
        return termMatches(rec, term, fuzzyMode, score);
    }, query);
}

} // namespace Search
