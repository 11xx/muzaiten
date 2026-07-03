#include "reco/TrackScorer.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

void pushIfNonZero(TrackScorer::Scored &scored, const QString &name, double value)
{
    if (value != 0.0) {
        scored.components.push_back({name, value});
        scored.score += value;
    }
}

double crowdingScale(qsizetype seedGenreCount, qsizetype candidateGenreCount, double softLimit)
{
    if (softLimit <= 0.0 || seedGenreCount <= 0 || candidateGenreCount <= 0) {
        return 1.0;
    }
    const double seedUsed = std::min(static_cast<double>(seedGenreCount), softLimit);
    const double candidateUsed = std::min(static_cast<double>(candidateGenreCount), softLimit);
    return std::sqrt((seedUsed * candidateUsed)
                     / (static_cast<double>(seedGenreCount) * static_cast<double>(candidateGenreCount)));
}

void assignNumber(const QJsonObject &object, const QString &key, double &target, double minimum,
                  double maximum = std::numeric_limits<double>::infinity())
{
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) {
        return;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < minimum || number > maximum) {
        return;
    }
    target = number;
}

} // namespace

namespace TrackScorer {

Weights defaultWeights()
{
    return {};
}

Weights weightsFromJson(const QByteArray &json, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }
    Weights weights = defaultWeights();
    if (json.trimmed().isEmpty()) {
        return weights;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error != nullptr) {
            *error = parseError.error != QJsonParseError::NoError
                ? parseError.errorString()
                : QStringLiteral("radio scoring weights must be a JSON object");
        }
        return weights;
    }

    const QJsonObject object = document.object();
    assignNumber(object, QStringLiteral("genreWeight"), weights.genreWeight, 0.0);
    assignNumber(object, QStringLiteral("genreIdfSaturation"), weights.genreIdfSaturation, 0.001);
    assignNumber(object, QStringLiteral("genreCrowdingSoftLimit"), weights.genreCrowdingSoftLimit, 1.0);
    assignNumber(object, QStringLiteral("eraWeight"), weights.eraWeight, 0.0);
    assignNumber(object, QStringLiteral("eraSpanYears"), weights.eraSpanYears, 0.001);
    assignNumber(object, QStringLiteral("ratingWeight"), weights.ratingWeight, 0.0);
    assignNumber(object, QStringLiteral("userRatingBoost"), weights.userRatingBoost, 0.0);
    assignNumber(object, QStringLiteral("historyWeight"), weights.historyWeight, 0.0);
    assignNumber(object, QStringLiteral("historySaturation"), weights.historySaturation, 0.001);
    assignNumber(object, QStringLiteral("noveltyWeight"), weights.noveltyWeight, 0.0);
    assignNumber(object, QStringLiteral("noveltyZeroAt"), weights.noveltyZeroAt, 0.001);
    assignNumber(object, QStringLiteral("recencyPenalty"), weights.recencyPenalty, -100.0, 0.0);
    assignNumber(object, QStringLiteral("recencyHalfLifeDays"), weights.recencyHalfLifeDays, 0.001);
    assignNumber(object, QStringLiteral("skipPenalty"), weights.skipPenalty, -100.0, 0.0);
    assignNumber(object, QStringLiteral("sameArtistPenalty"), weights.sameArtistPenalty, -100.0, 0.0);
    return weights;
}

Scored score(const Candidate &candidate, const Affinity &affinity, const SeedContext &seed)
{
    return score(candidate, affinity, seed, defaultWeights());
}

Scored score(const Candidate &candidate, const Affinity &affinity, const SeedContext &seed,
             const Weights &weights)
{
    Scored scored;
    scored.path = candidate.path;

    const double exploration = std::clamp(seed.exploration0To100, 0, 100);

    // genre: sum of IDF weights shared with the rolling context, damped when
    // either side carries a crowded tag-soup genre list. This keeps clean
    // one/two-genre matches strong while broad classifier-derived lists stop
    // saturating similarity merely by offering many possible overlaps.
    if (!seed.genresFolded.isEmpty() && !candidate.genresFolded.isEmpty()) {
        const QSet<QString> seedGenres(seed.genresFolded.cbegin(), seed.genresFolded.cend());
        double idfSum = 0.0;
        for (const QString &genre : candidate.genresFolded) {
            if (seedGenres.contains(genre)) {
                idfSum += seed.genreIdf.value(genre, 0.0);
            }
        }
        idfSum *= crowdingScale(seed.genresFolded.size(), candidate.genresFolded.size(),
                                weights.genreCrowdingSoftLimit);
        const double genreScore = std::min(1.0, idfSum / weights.genreIdfSaturation);
        const double explorationScale = 1.25 - exploration / 200.0;
        pushIfNonZero(scored, QStringLiteral("genre"), weights.genreWeight * genreScore * explorationScale);
    }

    // era: linear proximity in years, both years known.
    if (candidate.year > 0 && seed.year > 0) {
        const double delta = std::min(static_cast<double>(std::abs(candidate.year - seed.year)), weights.eraSpanYears);
        pushIfNonZero(scored, QStringLiteral("era"), weights.eraWeight * (1.0 - delta / weights.eraSpanYears));
    }

    // rating: effective rating, with a boost when it is the user's own rating.
    if (candidate.effectiveRating0To100 >= 0) {
        const double base = weights.ratingWeight * (candidate.effectiveRating0To100 / 100.0);
        pushIfNonZero(scored, QStringLiteral("rating"), candidate.hasUserRating ? base * weights.userRatingBoost : base);
    }

    // history: how much this track has been heard, saturating.
    const int heard = affinity.listenCount + affinity.baselineMax + affinity.finished;
    if (heard > 0) {
        const double ratio = std::min(1.0, std::log1p(heard) / std::log1p(weights.historySaturation));
        pushIfNonZero(scored, QStringLiteral("history"), weights.historyWeight * ratio);
    }

    // novelty: reward the unheard, decaying to nothing by kNoveltyZeroAt total
    // plays, and scaled up with exploration.
    const int totalPlays = affinity.playEvents + affinity.listenCount + affinity.baselineMax;
    const double noveltyRatio = std::max(0.0, 1.0 - totalPlays / weights.noveltyZeroAt);
    if (noveltyRatio > 0.0) {
        const double explorationScale = 0.5 + exploration / 100.0;
        pushIfNonZero(scored, QStringLiteral("novelty"), weights.noveltyWeight * noveltyRatio * explorationScale);
    }

    // recency: penalize a track played recently, exponentially fading with time.
    if (affinity.lastPlayedAtSecs > 0 && seed.nowSecs > 0) {
        const double days = static_cast<double>(seed.nowSecs - affinity.lastPlayedAtSecs) / 86400.0;
        if (days >= 0.0) {
            pushIfNonZero(scored, QStringLiteral("recency"),
                          weights.recencyPenalty * std::exp(-days / weights.recencyHalfLifeDays));
        }
    }

    // skips: penalize a track the user tends to skip (early skips only — the
    // aggregation already excludes skips past the scrobble threshold). The +2
    // smoothing keeps a lone "not right now" skip from branding a low-evidence
    // track (one skip on one spin reads ~0.33, not 1.0); with more spins the
    // rate converges to the truth.
    if (affinity.playEvents > 0 && affinity.skipped > 0) {
        const double skipRate = static_cast<double>(affinity.skipped)
            / (static_cast<double>(affinity.playEvents) + 2.0);
        pushIfNonZero(scored, QStringLiteral("skips"), weights.skipPenalty * skipRate);
    }

    // same-artist: a soft nudge away from an artist heard in the rolling window.
    // The hard "no same artist within k picks" throttle lives in RadioSession.
    if (!candidate.artistFolded.isEmpty() && seed.recentArtistsFolded.contains(candidate.artistFolded)) {
        pushIfNonZero(scored, QStringLiteral("same-artist"), weights.sameArtistPenalty);
    }

    return scored;
}

} // namespace TrackScorer
