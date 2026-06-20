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

// Build a scoped query that matches the artist and title as whole PHRASES
// (quoted field values), e.g. artist:"suni clay" title:"my hood". Phrase matching
// is far more precise than AND-ing each word separately, which scatter-matched
// unrelated tracks and produced spurious MultiMatch. The relaxed per-word fallback
// (relaxedQueryString) still provides recall when a phrase is too strict.
// Build a scoped artist+title phrase query. With `stripNoise`, packaging tags are
// removed from the title; without it the FULL title is used so a real version
// discriminator ("(Full Mix)", "(Instrumental)") still distinguishes copies.
// Quote the raw values and let SearchQuery::parse fold them with the SAME Fold the
// index uses — folding both sides identically. (Do NOT pre-run normalizeForMatch:
// it turns punctuation into spaces, so "Static-X" would become "static x" and no
// longer phrase-match the index's "static-x".)
QString scopedQueryString(const ImportEntry &entry, bool stripNoise)
{
    QStringList parts;
    const QString artist = entry.artist.simplified();
    if (!artist.isEmpty()) {
        parts.append(QStringLiteral("artist:%1").arg(Search::quoteFieldValue(artist)));
    }
    const QString title = (stripNoise ? stripTitleNoise(entry.title) : entry.title).simplified();
    if (!title.isEmpty()) {
        parts.append(QStringLiteral("title:%1").arg(Search::quoteFieldValue(title)));
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

// Fraction of a candidate's title+artist length the query plausibly explains.
// Used to reject fuzzy "magnet" candidates whose long fields dwarf a short query.
double coverageRatio(const ScoredResult &hit, const ImportEntry &entry)
{
    const QString querySignal =
        normalizeForMatch(entry.artist + QLatin1Char(' ') + stripTitleNoise(entry.title))
            .remove(QLatin1Char(' '));
    const qsizetype candSignal = hit.rec.normTitle.size() + hit.rec.normArtist.size();
    if (querySignal.isEmpty() || candSignal <= 0) {
        return 1.0;  // nothing to judge → don't penalise
    }
    return std::min(1.0, static_cast<double>(querySignal.size()) / static_cast<double>(candSignal));
}

// True when the candidate title shares a real word with the query title — used to
// reject fuzzy/relaxed hits that matched only via the artist or scattered
// subsequence noise (e.g. "The Only" landing on "Stronger (...) Remix"). A
// significant query-title token (>= 4 chars) must appear in the candidate title.
// Titles with no significant token are not gated (nothing to anchor on).
bool titleWordOverlaps(const ScoredResult &hit, const ImportEntry &entry)
{
    if (hit.rec.normTitle.isEmpty()) {
        return false;
    }
    const QStringList tokens = normalizeForMatch(stripTitleNoise(entry.title))
                                   .split(QLatin1Char(' '), Qt::SkipEmptyParts);
    bool hadSignificant = false;
    for (const QString &token : tokens) {
        if (token.size() >= 4) {
            hadSignificant = true;
            if (hit.rec.normTitle.contains(token)) {
                return true;
            }
        }
    }
    return !hadSignificant;
}

// Heuristic certainty (0-100) of a single chosen hit: the tier `base` plus a
// reward for being uncontested and for album/duration corroboration.
int confidenceFor(const ScoredResult &hit, const ImportEntry &entry, int base, bool uncontested)
{
    int c = base + (uncontested ? kConfidenceUncontested : 0);
    if (!entry.album.isEmpty() && !hit.rec.normAlbum.isEmpty()) {
        const QString wantedAlbum = normalizeForMatch(entry.album);
        if (!wantedAlbum.isEmpty() && hit.rec.normAlbum.contains(wantedAlbum)) {
            c += kConfidenceAlbumMatch;
        }
    }
    if (entry.durationMs > 0 && std::llabs(hit.rec.durationMs - entry.durationMs) <= kDurationToleranceMs) {
        c += kConfidenceDuration;
    }
    return std::clamp(c, 0, 100);
}

// Finalize a single chosen hit: Matched when confident, else Approximate (still
// auto-picked, but flagged with the close set kept for a quick review).
Outcome single(const ScoredResult &hit, const ImportEntry &entry, int base, bool uncontested,
               const QString &queryUsed, const QVector<ScoredResult> &alternatives)
{
    Outcome outcome;
    outcome.best = hit.rec;
    outcome.queryUsed = queryUsed;
    outcome.confidence0To100 = confidenceFor(hit, entry, base, uncontested);
    if (outcome.confidence0To100 >= kMatchedConfidence) {
        outcome.decision = Decision::Matched;
    } else {
        outcome.decision = Decision::Approximate;
        for (const ScoredResult &r : alternatives) {
            outcome.candidatePaths.append(r.rec.path);
        }
    }
    return outcome;
}

// Decide from a scored result list. `base` is the match-tier confidence; `entry`
// supplies album/duration tiebreakers.
Outcome decide(const QVector<ScoredResult> &results, const ImportEntry &entry,
               const QString &queryUsed, int base)
{
    Outcome outcome;
    outcome.queryUsed = queryUsed;

    // Magnet guard on the fuzzy/relaxed tiers: drop candidates whose title+artist
    // dwarf the query (a long field fuzzily "contains" almost any short query).
    // Exact-substring tiers are reliable and keep every hit.
    QVector<ScoredResult> kept;
    if (base <= kConfidenceScopedFuzzy) {
        for (const ScoredResult &r : results) {
            if (coverageRatio(r, entry) >= kMinFuzzyCoverage && titleWordOverlaps(r, entry)) {
                kept.append(r);
            }
        }
    } else {
        kept = results;
    }

    if (kept.isEmpty()) {
        return outcome;  // Pending (nothing survived, or magnets only)
    }
    if (kept.size() == 1) {
        return single(kept.first(), entry, base, /*uncontested=*/true, queryUsed, kept);
    }

    // The "close set": everything scoring near the top hit.
    const int topScore = kept.first().score;
    QVector<ScoredResult> close;
    for (const ScoredResult &r : kept) {
        if (r.score >= static_cast<int>(topScore * kCloseScoreRatio)) {
            close.append(r);
        }
        if (close.size() >= kMaxCandidates) {
            break;
        }
    }
    if (close.size() == 1) {
        return single(close.first(), entry, base, /*uncontested=*/true, queryUsed, close);
    }

    // The full close set is what a MultiMatch presents — tiebreakers below only
    // try to RESOLVE to a single hit; they must not silently drop the other valid
    // copies from the candidate list when they fail to resolve.
    const QVector<ScoredResult> closeFull = close;

    // Tiebreakers, never filters: album text, then duration proximity. If they
    // single out exactly one of the close hits, trust it (corroborated, so not
    // "uncontested" but the album/duration bonus lifts its confidence).
    if (!entry.album.isEmpty()) {
        const QString wantedAlbum = normalizeForMatch(entry.album);
        QVector<ScoredResult> albumHits;
        for (const ScoredResult &r : close) {
            if (!wantedAlbum.isEmpty() && r.rec.normAlbum.contains(wantedAlbum)) {
                albumHits.append(r);
            }
        }
        if (albumHits.size() == 1) {
            return single(albumHits.first(), entry, base, /*uncontested=*/false, queryUsed, close);
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
            return single(durationHits.first(), entry, base, /*uncontested=*/false, queryUsed, close);
        }
        if (!durationHits.isEmpty()) {
            close = durationHits;
        }
    }

    outcome.decision = Decision::MultiMatch;
    for (const ScoredResult &r : closeFull) {
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
            return single(hits.first(), entry, kConfidencePath, true, QString(), hits);
        }
        const QString basename = QFileInfo(entry.directPath).fileName();
        hits = matchByPath(index, basename, true);
        if (hits.size() == 1) {
            return single(hits.first(), entry, kConfidencePath, true, QString(), hits);
        }
    }

    // 2. Field-scoped artist:/title: phrase. Try the FULL title first so a version
    //    discriminator like "(Full Mix)" still picks the right copy; then the
    //    noise-stripped title to shed packaging junk. Exact mode for both before
    //    fuzzy, so a precise hit always beats a loose one.
    const QString full = scopedQueryString(entry, /*stripNoise=*/false);
    const QString stripped = scopedQueryString(entry, /*stripNoise=*/true);
    QStringList scopedQueries{full};
    if (stripped != full) {
        scopedQueries.append(stripped);
    }
    for (const bool fuzzy : {false, true}) {
        for (const QString &scoped : scopedQueries) {
            if (scoped.isEmpty()) {
                continue;
            }
            const QVector<ScoredResult> results = run(index, scoped, fuzzy);
            if (!results.isEmpty()) {
                return decide(results, entry, scoped,
                              fuzzy ? kConfidenceScopedFuzzy : kConfidenceScopedExact);
            }
        }
    }

    // 3. Relaxed free-text fallback (whole haystack, fuzzy) — least certain tier.
    const QString relaxed = relaxedQueryString(entry);
    QVector<ScoredResult> results = run(index, relaxed, /*fuzzy=*/true);
    Outcome outcome = decide(results, entry, relaxed.isEmpty() ? stripped : relaxed, kConfidenceRelaxed);
    if (outcome.decision == Decision::Pending && outcome.queryUsed.isEmpty()) {
        // Keep something re-runnable on the pending item for the edit modal.
        outcome.queryUsed = stripped.isEmpty() ? relaxed : stripped;
    }
    return outcome;
}

} // namespace PlaylistMatcher
