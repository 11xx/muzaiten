#include "reco/TrackScorer.h"

#include <algorithm>
#include <cmath>

namespace {

// v1 tuning defaults. The weights set the relative pull of each signal; genre is
// the loudest voice (it defines the mood), rating and history are the taste
// priors, and the two penalties (recency, skips) are strong enough to override a
// good genre match for a track the user just heard or keeps skipping. Novelty is
// a gentle nudge, scaled by the exploration knob so a conservative session leans
// on the familiar and an exploratory one tolerates fresh, less-similar picks.
// These are the knobs to turn when the radio "feels" wrong — nothing else here
// encodes taste.
constexpr double kGenreWeight = 3.0;      // + up to this for a full genre overlap
// The genre component is an ABSOLUTE sum of shared-genre IDF weights, not a
// ratio normalized by the seed's own genre count — normalizing by the seed
// would let a single-genre seed cancel its own weighting (idf/idf == 1.0
// regardless of how broad or junky that one genre is), which is exactly the
// "Other" bug this scale replaces. kGenreIdfSaturation is the IDF-sum at which
// the component reaches 1.0: one shared genre covering ~2% of the tagged
// library (idf ~= 3.9) roughly saturates it on its own; a broad genre like
// "rock" (idf ~= 1.8) contributes ~0.45 alone — real, but weak. Stoplisted
// placeholder genres ("Other", "Unknown", ...) never appear in the sum because
// they are filtered out of the rolling context upstream (GenreTags::informative,
// applied in AppCore::startRadio and RadioSession).
constexpr double kGenreIdfSaturation = 4.0;
constexpr double kEraWeight = 1.0;        // + up to this for a same-year candidate
constexpr double kEraSpanYears = 30.0;    // beyond this year gap, era stops mattering
constexpr double kRatingWeight = 1.5;     // + up to this at a 100/100 effective rating
constexpr double kUserRatingBoost = 1.25; // extra multiplier when the rating is the user's own
constexpr double kHistoryWeight = 1.0;    // + up to this for a well-heard track
constexpr double kHistorySaturation = 50.0; // listens/plays at which history saturates
constexpr double kNoveltyWeight = 0.8;    // + up to this for an unheard track
constexpr double kNoveltyZeroAt = 10.0;   // total plays at which novelty decays to 0
constexpr double kRecencyPenalty = -2.0;  // - up to this for a just-played track
constexpr double kRecencyHalfLifeDays = 14.0; // exp decay constant of the recency penalty
constexpr double kSkipPenalty = -2.5;     // - up to this for an always-skipped track
constexpr double kSameArtistPenalty = -0.6; // - for a recently-heard artist (soft term)

void pushIfNonZero(TrackScorer::Scored &scored, const QString &name, double value)
{
    if (value != 0.0) {
        scored.components.push_back({name, value});
        scored.score += value;
    }
}

} // namespace

namespace TrackScorer {

Scored score(const Candidate &candidate, const Affinity &affinity, const SeedContext &seed)
{
    Scored scored;
    scored.path = candidate.path;

    const double exploration = std::clamp(seed.exploration0To100, 0, 100);

    // genre: sum of IDF weights of the genres shared with the rolling context,
    // on an absolute scale (see kGenreIdfSaturation) rather than a ratio of the
    // seed's own genre count. Exploration softens the pull toward similarity
    // (conservative leans similar, exploratory drifts). A genre missing from
    // seed.genreIdf (including an entirely empty map) contributes IDF 0.
    if (!seed.genresFolded.isEmpty() && !candidate.genresFolded.isEmpty()) {
        const QSet<QString> seedGenres(seed.genresFolded.cbegin(), seed.genresFolded.cend());
        double idfSum = 0.0;
        for (const QString &genre : candidate.genresFolded) {
            if (seedGenres.contains(genre)) {
                idfSum += seed.genreIdf.value(genre, 0.0);
            }
        }
        const double genreScore = std::min(1.0, idfSum / kGenreIdfSaturation);
        const double explorationScale = 1.25 - exploration / 200.0;
        pushIfNonZero(scored, QStringLiteral("genre"), kGenreWeight * genreScore * explorationScale);
    }

    // era: linear proximity in years, both years known.
    if (candidate.year > 0 && seed.year > 0) {
        const double delta = std::min(static_cast<double>(std::abs(candidate.year - seed.year)), kEraSpanYears);
        pushIfNonZero(scored, QStringLiteral("era"), kEraWeight * (1.0 - delta / kEraSpanYears));
    }

    // rating: effective rating, with a boost when it is the user's own rating.
    if (candidate.effectiveRating0To100 >= 0) {
        const double base = kRatingWeight * (candidate.effectiveRating0To100 / 100.0);
        pushIfNonZero(scored, QStringLiteral("rating"), candidate.hasUserRating ? base * kUserRatingBoost : base);
    }

    // history: how much this track has been heard, saturating.
    const int heard = affinity.listenCount + affinity.baselineMax + affinity.finished;
    if (heard > 0) {
        const double ratio = std::min(1.0, std::log1p(heard) / std::log1p(kHistorySaturation));
        pushIfNonZero(scored, QStringLiteral("history"), kHistoryWeight * ratio);
    }

    // novelty: reward the unheard, decaying to nothing by kNoveltyZeroAt total
    // plays, and scaled up with exploration.
    const int totalPlays = affinity.playEvents + affinity.listenCount + affinity.baselineMax;
    const double noveltyRatio = std::max(0.0, 1.0 - totalPlays / kNoveltyZeroAt);
    if (noveltyRatio > 0.0) {
        const double explorationScale = 0.5 + exploration / 100.0;
        pushIfNonZero(scored, QStringLiteral("novelty"), kNoveltyWeight * noveltyRatio * explorationScale);
    }

    // recency: penalize a track played recently, exponentially fading with time.
    if (affinity.lastPlayedAtSecs > 0 && seed.nowSecs > 0) {
        const double days = static_cast<double>(seed.nowSecs - affinity.lastPlayedAtSecs) / 86400.0;
        if (days >= 0.0) {
            pushIfNonZero(scored, QStringLiteral("recency"),
                          kRecencyPenalty * std::exp(-days / kRecencyHalfLifeDays));
        }
    }

    // skips: penalize a track the user tends to skip.
    if (affinity.playEvents > 0) {
        const double skipRate = static_cast<double>(affinity.skipped) / std::max(1, affinity.playEvents);
        pushIfNonZero(scored, QStringLiteral("skips"), kSkipPenalty * skipRate);
    }

    // same-artist: a soft nudge away from an artist heard in the rolling window.
    // The hard "no same artist within k picks" throttle lives in RadioSession.
    if (!candidate.artistFolded.isEmpty() && seed.recentArtistsFolded.contains(candidate.artistFolded)) {
        pushIfNonZero(scored, QStringLiteral("same-artist"), kSameArtistPenalty);
    }

    return scored;
}

} // namespace TrackScorer
