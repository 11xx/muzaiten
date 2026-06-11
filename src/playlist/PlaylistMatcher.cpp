#include "playlist/PlaylistMatcher.h"

#include "search/SearchQuery.h"

#include <QFileInfo>
#include <QStringList>

#include <algorithm>

namespace PlaylistMatcher {

namespace {

using PlaylistImport::ImportEntry;
using PlaylistImport::normalizeForMatch;
using PlaylistImport::stripTitleNoise;
using Search::ScoredResult;
using Search::SearchIndex;
using Search::SearchQuery;

// Words too generic to help in the relaxed free-text fallback.
bool isStopWord(const QString &word)
{
    static const QStringList stop = {
        QStringLiteral("the"), QStringLiteral("a"), QStringLiteral("an"),
        QStringLiteral("of"), QStringLiteral("and"), QStringLiteral("to"),
        QStringLiteral("in"), QStringLiteral("de"), QStringLiteral("la"),
    };
    return stop.contains(word);
}

// Build a parse-compatible scoped query string: each word becomes its own
// field-scoped term (terms AND together). Word counts are capped so an
// over-long source line can't over-narrow the query.
QString scopedQueryString(const ImportEntry &entry)
{
    QStringList parts;
    const QStringList artistWords = normalizeForMatch(entry.artist).split(
        QLatin1Char(' '), Qt::SkipEmptyParts);
    for (qsizetype i = 0; i < std::min<qsizetype>(artistWords.size(), 4); ++i) {
        parts.append(QStringLiteral("artist:%1").arg(artistWords.at(i)));
    }
    const QStringList titleWords = normalizeForMatch(stripTitleNoise(entry.title)).split(
        QLatin1Char(' '), Qt::SkipEmptyParts);
    for (qsizetype i = 0; i < std::min<qsizetype>(titleWords.size(), 6); ++i) {
        parts.append(QStringLiteral("title:%1").arg(titleWords.at(i)));
    }
    return parts.join(QLatin1Char(' '));
}

// Relaxed fallback: every word as a free term across the whole haystack.
QString relaxedQueryString(const ImportEntry &entry)
{
    const QString combined = normalizeForMatch(
        entry.artist + QLatin1Char(' ') + stripTitleNoise(entry.title));
    QStringList words = combined.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    words.erase(std::remove_if(words.begin(), words.end(), isStopWord), words.end());
    while (words.size() > 8) {
        words.removeLast();
    }
    return words.join(QLatin1Char(' '));
}

QVector<ScoredResult> run(const SearchIndex &index, const QString &queryString, bool fuzzy)
{
    if (queryString.trimmed().isEmpty()) {
        return {};
    }
    return index.match(SearchQuery::parse(queryString), fuzzy);
}

// Path lookups go through the engine too: a single exact path: term.
QVector<ScoredResult> matchByPath(const SearchIndex &index, const QString &needle,
                                  bool filenameOnly)
{
    if (needle.isEmpty()) {
        return {};
    }
    SearchQuery query;
    Search::Term term;
    term.kind = filenameOnly ? Search::TermKind::FilenameText : Search::TermKind::PathText;
    term.text = needle.toLower();
    term.rawText = needle;
    term.forceExact = true;
    query.terms.append(term);
    return index.match(query, /*fuzzyMode=*/false);
}

Outcome matched(const ScoredResult &hit, const QString &queryUsed)
{
    Outcome outcome;
    outcome.decision = Decision::Matched;
    outcome.best = hit.rec;
    outcome.queryUsed = queryUsed;
    return outcome;
}

// Decide from a scored result list. `entry` supplies album/duration tiebreakers.
Outcome decide(const QVector<ScoredResult> &results, const ImportEntry &entry,
               const QString &queryUsed)
{
    Outcome outcome;
    outcome.queryUsed = queryUsed;
    if (results.isEmpty()) {
        return outcome;  // Pending
    }
    if (results.size() == 1) {
        return matched(results.first(), queryUsed);
    }

    // The "close set": everything scoring near the top hit.
    const int topScore = results.first().score;
    QVector<ScoredResult> close;
    for (const ScoredResult &r : results) {
        if (r.score >= static_cast<int>(topScore * kCloseScoreRatio)) {
            close.append(r);
        }
        if (close.size() >= kMaxCandidates) {
            break;
        }
    }
    if (close.size() == 1) {
        return matched(close.first(), queryUsed);
    }

    // Tiebreakers, never filters: album text, then duration proximity. If they
    // single out exactly one of the close hits, trust it.
    if (!entry.album.isEmpty()) {
        const QString wantedAlbum = normalizeForMatch(entry.album);
        QVector<ScoredResult> albumHits;
        for (const ScoredResult &r : close) {
            if (!wantedAlbum.isEmpty() && r.rec.normAlbum.contains(wantedAlbum)) {
                albumHits.append(r);
            }
        }
        if (albumHits.size() == 1) {
            return matched(albumHits.first(), queryUsed);
        }
        if (!albumHits.isEmpty()) {
            close = albumHits;
        }
    }
    if (entry.durationMs > 0) {
        QVector<ScoredResult> durationHits;
        for (const ScoredResult &r : close) {
            if (std::llabs(r.rec.durationMs - entry.durationMs) <= kDurationToleranceMs) {
                durationHits.append(r);
            }
        }
        if (durationHits.size() == 1) {
            return matched(durationHits.first(), queryUsed);
        }
        if (!durationHits.isEmpty()) {
            close = durationHits;
        }
    }

    outcome.decision = Decision::MultiMatch;
    for (const ScoredResult &r : close) {
        outcome.candidatePaths.append(r.rec.path);
    }
    return outcome;
}

} // namespace

Outcome match(const SearchIndex &index, const ImportEntry &entry)
{
    // 1. Direct path resolution (m3u/csv exports). Exact full path, then
    //    basename — same files on a different machine/layout.
    if (!entry.directPath.isEmpty()) {
        QVector<ScoredResult> hits = matchByPath(index, entry.directPath, false);
        if (hits.size() == 1) {
            return matched(hits.first(), QString());
        }
        const QString basename = QFileInfo(entry.directPath).fileName();
        hits = matchByPath(index, basename, true);
        if (hits.size() == 1) {
            return matched(hits.first(), QString());
        }
    }

    // 2. Field-scoped artist:/title: query — exact mode, then fuzzy.
    const QString scoped = scopedQueryString(entry);
    if (!scoped.isEmpty()) {
        QVector<ScoredResult> results = run(index, scoped, /*fuzzy=*/false);
        if (results.isEmpty()) {
            results = run(index, scoped, /*fuzzy=*/true);
        }
        if (!results.isEmpty()) {
            return decide(results, entry, scoped);
        }
    }

    // 3. Relaxed free-text fallback (whole haystack, fuzzy).
    const QString relaxed = relaxedQueryString(entry);
    QVector<ScoredResult> results = run(index, relaxed, /*fuzzy=*/true);
    Outcome outcome = decide(results, entry, relaxed.isEmpty() ? scoped : relaxed);
    if (outcome.decision == Decision::Pending && outcome.queryUsed.isEmpty()) {
        // Keep something re-runnable on the pending item for the edit modal.
        outcome.queryUsed = scoped.isEmpty() ? relaxed : scoped;
    }
    return outcome;
}

} // namespace PlaylistMatcher
