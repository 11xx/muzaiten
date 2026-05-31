#include "search/SearchIndex.h"

#include "search/FuzzyMatch.h"
#include "search/SearchQuery.h"
#include "search/SearchRecord.h"

#include <algorithm>

namespace Search {

// ---- numeric comparison helper --------------------------------------------

static bool compareNumeric(qint64 trackValue, CompareOp op, qint64 queryValue)
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

// Extract the 4-digit year from a date string (first 4 chars if numeric).
static int yearFromDate(const QString &date)
{
    if (date.length() >= 4) {
        bool ok;
        const int y = date.left(4).toInt(&ok);
        if (ok) return y;
    }
    return 0;
}

// Match a single text term against one field string (already lowercased).
// Returns a MatchResult; !matched() means no match.
static MatchResult matchTextField(const QString &normField,
                                   const QString &origField,
                                   const Term    &term,
                                   bool           fuzzyMode,
                                   bool           withPositions)
{
    Q_UNUSED(origField);
    if (normField.isEmpty()) return {};

    // Anchored matching — always exact substring
    if (term.prefixAnchor && term.suffixAnchor) {
        // Full equality
        if (normField != term.text) return {};
        MatchResult r{0, static_cast<int>(normField.length()), scoreMatch * static_cast<int>(term.text.length()), {}};
        if (withPositions) {
            r.positions.reserve(term.text.length());
            for (int i = 0; i < term.text.length(); ++i) r.positions.append(i);
        }
        return r;
    }
    if (term.prefixAnchor) {
        if (!normField.startsWith(term.text)) return {};
        return exactMatchNaive(normField, term.text, true, withPositions);
    }
    if (term.suffixAnchor) {
        if (!normField.endsWith(term.text)) return {};
        // Score from the suffix position
        return exactMatchNaive(normField, term.text, true, withPositions);
    }

    const bool exactMode = term.forceExact || !fuzzyMode;
    return matchTerm(normField, term.text, true /*already lowercased*/, !exactMode, withPositions);
}

// Field weight for scoring (higher = more important).
static int fieldWeight(TermKind kind)
{
    switch (kind) {
    case TermKind::TitleText:       return 400;
    case TermKind::ArtistText:
    case TermKind::AlbumArtistText: return 300;
    case TermKind::AlbumText:       return 200;
    case TermKind::FilenameText:    return 100;
    case TermKind::PathText:        return  50;
    default:                        return 200; // free text gets album weight
    }
}

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

QVector<ScoredResult> SearchIndex::match(const SearchQuery &query, bool fuzzyMode) const
{
    if (query.isEmpty() || m_records.isEmpty()) return {};

    QVector<ScoredResult> results;
    results.reserve(std::min(static_cast<int>(m_records.size()), 2000));

    for (int i = 0; i < static_cast<int>(m_records.size()); ++i) {
        const SearchRecord &rec = m_records[i];

        ScoredResult sr;
        bool allMatch = true;
        int totalScore = 0;

        for (const Term &term : query.terms) {
            bool termMatched = false;

            // ---- numeric tokens -------------------------------------------
            switch (term.kind) {
            case TermKind::Year: {
                const int y = yearFromDate(rec.date);
                termMatched = y > 0 && compareNumeric(y, term.op, term.numericValue);
                if (termMatched) totalScore += 100;
                goto nextTerm;
            }
            case TermKind::Rating:
                termMatched = rec.rating0To100 >= 0 &&
                              compareNumeric(rec.rating0To100, term.op, term.numericValue);
                if (termMatched) totalScore += 100;
                goto nextTerm;
            case TermKind::DurationMs:
                termMatched = rec.durationMs > 0 &&
                              compareNumeric(rec.durationMs, term.op, term.numericValue);
                if (termMatched) totalScore += 100;
                goto nextTerm;
            case TermKind::SampleRateHz:
                termMatched = rec.sampleRateHz > 0 &&
                              compareNumeric(rec.sampleRateHz, term.op, term.numericValue);
                if (termMatched) totalScore += 100;
                goto nextTerm;
            case TermKind::BitrateKbps:
                termMatched = rec.bitrateKbps > 0 &&
                              compareNumeric(rec.bitrateKbps, term.op, term.numericValue);
                if (termMatched) totalScore += 100;
                goto nextTerm;
            case TermKind::Channels:
                termMatched = rec.channels > 0 &&
                              compareNumeric(rec.channels, term.op, term.numericValue);
                if (termMatched) totalScore += 100;
                goto nextTerm;
            case TermKind::Extension:
                // ext: is an exact match on the codec/extension field
                termMatched = !rec.codec.isEmpty() && rec.codec == term.text;
                if (termMatched) totalScore += 150;
                goto nextTerm;
            case TermKind::CodecText: {
                const MatchResult mr = exactMatchNaive(rec.codec, term.text, true, false);
                termMatched = mr.matched();
                if (termMatched) totalScore += 100 + mr.score;
                goto nextTerm;
            }
            default:
                break; // handled below
            }

            // ---- text tokens ---------------------------------------------
            {
                const bool withPos = true; // always collect positions for highlighting

                // Append (union) match positions so EVERY matching term contributes
                // its highlight ranges to the field — not just the last one.
                auto tryField = [&](const QString &norm, const QString &orig,
                                    QVector<int> *outPositions,
                                    int weight) -> bool {
                    MatchResult mr = matchTextField(norm, orig, term, fuzzyMode, withPos);
                    if (mr.matched()) {
                        totalScore += mr.score + weight;
                        if (outPositions) *outPositions += mr.positions;
                        return true;
                    }
                    return false;
                };

                switch (term.kind) {
                case TermKind::TitleText:
                    termMatched = tryField(rec.normTitle, rec.title,
                                           &sr.ranges.titlePositions, fieldWeight(TermKind::TitleText));
                    break;
                case TermKind::ArtistText: {
                    const bool a = tryField(rec.normArtist, rec.artistName,
                                            &sr.ranges.artistPositions, fieldWeight(TermKind::ArtistText));
                    const bool b = tryField(rec.normAlbumArtist, rec.albumArtistName,
                                            &sr.ranges.albumArtistPositions, fieldWeight(TermKind::ArtistText));
                    termMatched = a || b;
                    break;
                }
                case TermKind::AlbumArtistText:
                    termMatched = tryField(rec.normAlbumArtist, rec.albumArtistName,
                                           &sr.ranges.albumArtistPositions, fieldWeight(TermKind::AlbumArtistText));
                    break;
                case TermKind::AlbumText:
                    termMatched = tryField(rec.normAlbum, rec.albumTitle,
                                           &sr.ranges.albumPositions, fieldWeight(TermKind::AlbumText));
                    break;
                case TermKind::PathText:
                    termMatched = tryField(rec.normPath, rec.path,
                                           &sr.ranges.pathPositions, fieldWeight(TermKind::PathText));
                    break;
                case TermKind::FilenameText:
                    termMatched = tryField(rec.normFilename, rec.filename,
                                           &sr.ranges.filenamePositions, fieldWeight(TermKind::FilenameText));
                    break;
                case TermKind::FreeText:
                default: {
                    // A free term matches if it matches ANY field; highlight every
                    // field it matches (each tryField appends its own positions).
                    bool matched = false;
                    matched |= tryField(rec.normTitle, rec.title,
                                        &sr.ranges.titlePositions, fieldWeight(TermKind::TitleText));
                    matched |= tryField(rec.normArtist, rec.artistName,
                                        &sr.ranges.artistPositions, fieldWeight(TermKind::ArtistText));
                    matched |= tryField(rec.normAlbumArtist, rec.albumArtistName,
                                        &sr.ranges.albumArtistPositions, fieldWeight(TermKind::AlbumArtistText));
                    matched |= tryField(rec.normAlbum, rec.albumTitle,
                                        &sr.ranges.albumPositions, fieldWeight(TermKind::AlbumText));
                    matched |= tryField(rec.normPath, rec.path,
                                        &sr.ranges.pathPositions, fieldWeight(TermKind::PathText));
                    termMatched = matched;
                    break;
                }
                } // switch term.kind
            }

            nextTerm:
            if (term.negate) termMatched = !termMatched;
            if (!termMatched) {
                allMatch = false;
                break;
            }
        } // for terms

        if (allMatch) {
            sr.rec   = rec;  // copy the record so it's self-contained (no cross-thread pointer)
            sr.score = totalScore;
            results.push_back(std::move(sr));
        }
    } // for records

    // Sort: descending score
    std::stable_sort(results.begin(), results.end(),
                     [](const ScoredResult &a, const ScoredResult &b) {
                         return a.score > b.score;
                     });

    return results;
}

} // namespace Search
