#include "search/SearchMatcher.h"

#include "search/FuzzyMatch.h"
#include "search/SearchRecord.h"

#include <algorithm>
#include <climits>
#include <span>

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
        || c == QLatin1Char('[') || c == QLatin1Char(',') || c == Fold::kSegmentSeparator;
}

int exactFieldScore(QStringView norm, const Term &term, int weight)
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

bool isSubsequence(QStringView hay, QStringView needle)
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

int fuzzyFieldScore(QStringView norm, QStringView needle, int weight)
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
    case MatchFieldRole::Extension:   return 60;
    case MatchFieldRole::Free:        return 100;
    }
    return 100;
}

void consider(int score, int &best)
{
    if (score > best) {
        best = score;
    }
}

// Anchors and fuzzy alignment answer per segment of the norm; an unanchored
// substring cannot span segments, so it scans the whole norm at once.
// `segmented` is 1 or 0 when the caller knows whether the norm holds a
// separator, -1 to look.
int scoreTextField(const QString &norm, const Term &term, bool fuzzyMode, int weight,
                   int segmented = -1)
{
    const bool exact = term.forceExact || !fuzzyMode;
    const auto scoreSegment = [&](QStringView segment) {
        return exact ? exactFieldScore(segment, term, weight)
                     : fuzzyFieldScore(segment, term.text, weight);
    };
    if ((exact && !term.prefixAnchor && !term.suffixAnchor) || segmented == 0
        || (segmented < 0 && !norm.contains(Fold::kSegmentSeparator))) {
        return scoreSegment(QStringView(norm));
    }
    int best = INT_MIN;
    const QStringView view(norm);
    for (qsizetype start = 0; start <= view.size();) {
        qsizetype end = view.indexOf(Fold::kSegmentSeparator, start);
        if (end < 0) end = view.size();
        consider(scoreSegment(view.mid(start, end - start)), best);
        if (end == view.size()) break;
        start = end + 1;
    }
    return best;
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
        return field.role == MatchFieldRole::Codec || field.role == MatchFieldRole::Free;
    case TermKind::Extension:
        return field.role == MatchFieldRole::Extension;
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
        // The file's own extension, not the codec column: an `.oga` may hold
        // Vorbis and an `.m4a` may hold ALAC, so the two answer different
        // questions and `ext:` is the one about the name on disk.
        const QString name = rec.filename.isEmpty() ? rec.path : rec.filename;
        const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
        const QString extension = dot >= 0 ? name.mid(dot + 1).toLower() : QString();
        const bool m = !extension.isEmpty() && extension == term.text;
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
        total = static_cast<int>(std::min<qint64>(INT_MAX - 1, static_cast<qint64>(total) + s));
    }
    return total;
}

// ---- index-side prefilter --------------------------------------------------

struct NormField {
    const QString SearchRecord::*norm;
    NormSlot slot;
    MatchFieldRole role;
};

// The fields a text term searches, in the order textTermScore() scores them:
// descending weight, so the first field that matches carries the highest one.
constexpr NormField kFreeFields[] = {
    {&SearchRecord::normTitle, NormTitle, MatchFieldRole::Title},
    {&SearchRecord::normArtist, NormArtist, MatchFieldRole::Artist},
    {&SearchRecord::normAlbumArtist, NormAlbumArtist, MatchFieldRole::AlbumArtist},
    {&SearchRecord::normAlbum, NormAlbum, MatchFieldRole::Album},
    {&SearchRecord::normPath, NormPath, MatchFieldRole::Path},
};
constexpr NormField kArtistFields[] = {
    {&SearchRecord::normArtist, NormArtist, MatchFieldRole::Artist},
    {&SearchRecord::normAlbumArtist, NormAlbumArtist, MatchFieldRole::AlbumArtist},
};
constexpr NormField kAlbumArtistFields[] = {
    {&SearchRecord::normAlbumArtist, NormAlbumArtist, MatchFieldRole::AlbumArtist}};
constexpr NormField kAlbumFields[] = {{&SearchRecord::normAlbum, NormAlbum, MatchFieldRole::Album}};
constexpr NormField kTitleFields[] = {{&SearchRecord::normTitle, NormTitle, MatchFieldRole::Title}};
constexpr NormField kPathFields[] = {{&SearchRecord::normPath, NormPath, MatchFieldRole::Path}};
constexpr NormField kFilenameFields[] = {
    {&SearchRecord::normFilename, NormFilename, MatchFieldRole::Filename}};

std::span<const NormField> textFields(TermKind kind)
{
    switch (kind) {
    case TermKind::FreeText:        return kFreeFields;
    case TermKind::ArtistText:      return kArtistFields;
    case TermKind::AlbumArtistText: return kAlbumArtistFields;
    case TermKind::AlbumText:       return kAlbumFields;
    case TermKind::TitleText:       return kTitleFields;
    case TermKind::PathText:        return kPathFields;
    case TermKind::FilenameText:    return kFilenameFields;
    default:                        return {};
    }
}

bool isTextKind(TermKind kind)
{
    return kind <= TermKind::FilenameText;
}

// The most a term's text can add to its field weight: exact scores top out at
// a start-of-field match (25 + 20) or an anchored bonus, fuzzy ones at every
// character consecutive on a word start (fzf: 26 per character, +10 for the
// first character's doubled bonus).
int maxTextScore(const Term &term, bool fuzzyMode)
{
    if (term.forceExact || !fuzzyMode) {
        if (term.prefixAnchor && term.suffixAnchor) return 40;
        if (term.prefixAnchor) return 30;
        if (term.suffixAnchor) return 12;
        return 45;
    }
    const qsizetype m = term.text.size();
    if (m > (INT_MAX - 11) / 26) return INT_MAX - 1;
    return static_cast<int>(26 * m + 10);
}

// The top mask bit marks a norm with more than one segment; term text never
// holds the separator, so a term mask never requires it.
constexpr quint64 kSegmentedBit = quint64{1} << 63;

int segmentedFlag(quint64 fieldMask)
{
    return (fieldMask & kSegmentedBit) ? 1 : 0;
}

bool fitsMask(quint64 fieldMask, quint64 termMask)
{
    return (fieldMask & termMask) == termMask;
}

// Whether some segment of the norm holds the needle as a subsequence: fuzzy
// terms align inside one segment, never across the separator.
bool segmentSubsequence(QStringView norm, QStringView needle, bool segmented = true)
{
    const qsizetype m = needle.size();
    if (m == 0) return true;
    // Step from one needle character to the next with the vectorized
    // single-character search, restarting the needle at each segment end.
    qsizetype pos = 0;
    qsizetype segmentEnd = segmented ? norm.indexOf(Fold::kSegmentSeparator) : -1;
    if (segmentEnd < 0) segmentEnd = norm.size();
    qsizetype j = 0;
    while (true) {
        const qsizetype hit = norm.indexOf(needle[j], pos);
        if (hit < 0) return false;
        if (hit >= segmentEnd) {
            pos = segmentEnd + 1;
            segmentEnd = norm.indexOf(Fold::kSegmentSeparator, pos);
            if (segmentEnd < 0) segmentEnd = norm.size();
            j = 0;
            continue;
        }
        if (++j == m) return true;
        pos = hit + 1;
    }
}

// A single folded ASCII letter or digit has a mask bit of its own, so the mask
// alone answers whether a field contains it.
bool maskDecides(const Term &term)
{
    if (term.text.size() != 1) return false;
    const char16_t c = term.text.front().unicode();
    return (c >= u'a' && c <= u'z') || (c >= u'0' && c <= u'9');
}

bool maskedFieldHolds(QStringView norm, quint64 mask, const Term &term, quint64 termMask,
                      bool fuzzyMode)
{
    if (!fitsMask(mask, termMask)) return false;
    return term.forceExact || !fuzzyMode ? norm.contains(term.text)
                                         : segmentSubsequence(norm, term.text, segmentedFlag(mask));
}

int textTermScoreMasked(const SearchRecord &rec, const NormMasks &masks, const Term &term,
                        quint64 termMask, bool fuzzyMode)
{
    const int headroom = maxTextScore(term, fuzzyMode);
    int best = INT_MIN;
    for (const NormField &field : textFields(term.kind)) {
        if (!fitsMask(masks[field.slot], termMask)) continue;
        const int weight = fieldWeight(field.role);
        if (best != INT_MIN && qint64{best} >= qint64{weight} + headroom) continue;
        consider(scoreTextField(rec.*field.norm, term, fuzzyMode, weight,
                                segmentedFlag(masks[field.slot])),
                 best);
    }
    return best;
}

} // namespace

quint64 normCharacterMask(QStringView text)
{
    quint64 mask = 0;
    for (const QChar ch : text) {
        const char16_t c = ch.unicode();
        if (c >= u'a' && c <= u'z') {
            mask |= quint64{1} << (c - u'a');
        } else if (c >= u'0' && c <= u'9') {
            mask |= quint64{1} << (26 + c - u'0');
        } else if (ch == Fold::kSegmentSeparator) {
            mask |= kSegmentedBit;
        } else if (c < 0x80) {
            mask |= quint64{1} << 36;
        } else {
            mask |= quint64{1} << (37 + (static_cast<quint32>(c) * 2654435761u >> 16) % 26);
        }
    }
    return mask;
}

NormMasks recordNormMasks(const SearchRecord &rec)
{
    return {normCharacterMask(rec.normTitle), normCharacterMask(rec.normArtist),
            normCharacterMask(rec.normAlbumArtist), normCharacterMask(rec.normAlbum),
            normCharacterMask(rec.normFilename), normCharacterMask(rec.normPath)};
}

QVector<quint64> termCharacterMasks(const SearchQuery &query)
{
    QVector<quint64> masks;
    masks.reserve(query.terms.size());
    for (const Term &term : query.terms) {
        masks.push_back(isTextKind(term.kind) ? normCharacterMask(term.text) : 0);
    }
    return masks;
}

int matchSearchRecordBound(const SearchRecord &rec, const NormMasks &masks,
                           const SearchQuery &query, const QVector<quint64> &termMasks,
                           bool fuzzyMode, bool *exact)
{
    *exact = true;
    if (query.isEmpty()) return INT_MIN;
    qint64 total = 0;
    for (qsizetype t = 0; t < query.terms.size(); ++t) {
        const Term &term = query.terms[t];
        const quint64 termMask = termMasks[t];
        bool matched = false;
        int score = 0;
        const bool fuzzyTerm = fuzzyMode && !term.forceExact;
        if (!isTextKind(term.kind) || term.text.isEmpty()) {
            matched = termMatches(rec, term, fuzzyMode, &score);
        } else if (fuzzyTerm || (maskDecides(term) && !term.prefixAnchor && !term.suffixAnchor)) {
            // Membership only; the first field that holds the term has the
            // highest weight among those that do.
            const bool byMask = maskDecides(term);
            for (const NormField &field : textFields(term.kind)) {
                if (!fitsMask(masks[field.slot], termMask)) continue;
                if (byMask || maskedFieldHolds(rec.*field.norm, masks[field.slot], term,
                                               termMask, fuzzyMode)) {
                    matched = true;
                    score = static_cast<int>(std::min<qint64>(INT_MAX - 1,
                        static_cast<qint64>(fieldWeight(field.role)) + maxTextScore(term, fuzzyMode)));
                    break;
                }
            }
            if (matched && !term.negate) *exact = false;
        } else {
            score = textTermScoreMasked(rec, masks, term, termMask, fuzzyMode);
            matched = score != INT_MIN;
        }
        if (term.negate) {
            matched = !matched;
            score = 0;
        }
        if (!matched) return INT_MIN;
        total = std::min<qint64>(INT_MAX - 1, total + score);
    }
    return static_cast<int>(total);
}

int scoreSearchRecord(const SearchRecord &rec, const NormMasks &masks,
                      const SearchQuery &query, const QVector<quint64> &termMasks,
                      bool fuzzyMode)
{
    if (query.isEmpty()) return INT_MIN;
    qint64 total = 0;
    for (qsizetype t = 0; t < query.terms.size(); ++t) {
        const Term &term = query.terms[t];
        int score = 0;
        bool matched;
        if (isTextKind(term.kind) && !term.text.isEmpty()) {
            score = textTermScoreMasked(rec, masks, term, termMasks[t], fuzzyMode);
            matched = score != INT_MIN;
        } else {
            matched = termMatches(rec, term, fuzzyMode, &score);
        }
        if (term.negate) {
            matched = !matched;
            score = 0;
        }
        if (!matched) return INT_MIN;
        total = std::min<qint64>(INT_MAX - 1, total + score);
    }
    return static_cast<int>(total);
}

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
