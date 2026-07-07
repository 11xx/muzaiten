#include "reco/TrackScorer.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr double kTempoFalloffBpm = 60.0;
constexpr double kEnergyFalloff = 1.0;

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

QStringList weightKeys()
{
    return {
        QStringLiteral("genreWeight"),
        QStringLiteral("genreIdfSaturation"),
        QStringLiteral("genreCrowdingSoftLimit"),
        QStringLiteral("eraWeight"),
        QStringLiteral("eraSpanYears"),
        QStringLiteral("tempoWeight"),
        QStringLiteral("energyWeight"),
        QStringLiteral("audioWeight"),
        QStringLiteral("ratingWeight"),
        QStringLiteral("userRatingBoost"),
        QStringLiteral("historyWeight"),
        QStringLiteral("historySaturation"),
        QStringLiteral("noveltyWeight"),
        QStringLiteral("noveltyZeroAt"),
        QStringLiteral("recencyPenalty"),
        QStringLiteral("recencyHalfLifeDays"),
        QStringLiteral("skipPenalty"),
        QStringLiteral("sameArtistPenalty"),
    };
}

bool assignNumber(const QJsonObject &object, const QString &key, double &target, double minimum,
                  double maximum = std::numeric_limits<double>::infinity(), QString *error = nullptr)
{
    const QJsonValue value = object.value(key);
    if (value.isUndefined()) {
        return true;
    }
    if (!value.isDouble()) {
        if (error != nullptr) {
            *error = QStringLiteral("%1 must be a number").arg(key);
        }
        return false;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < minimum || number > maximum) {
        if (error != nullptr) {
            *error = QStringLiteral("%1 is outside its allowed range").arg(key);
        }
        return false;
    }
    target = number;
    return true;
}

double linearProximity(double left, double right, double span)
{
    if (span <= 0.0) {
        return 0.0;
    }
    return std::max(0.0, 1.0 - std::abs(left - right) / span);
}

double dotProduct(const QVector<float> &left, const QVector<float> &right)
{
    if (left.isEmpty() || left.size() != right.size()) {
        return 0.0;
    }

    double sum = 0.0;
    for (qsizetype i = 0; i < left.size(); ++i) {
        sum += static_cast<double>(left.at(i)) * static_cast<double>(right.at(i));
    }
    return std::isfinite(sum) ? sum : 0.0;
}

} // namespace

namespace TrackScorer {

Weights defaultWeights()
{
    return {};
}

QByteArray weightsToJson(const Weights &weights)
{
    QJsonObject object;
    object.insert(QStringLiteral("genreWeight"), weights.genreWeight);
    object.insert(QStringLiteral("genreIdfSaturation"), weights.genreIdfSaturation);
    object.insert(QStringLiteral("genreCrowdingSoftLimit"), weights.genreCrowdingSoftLimit);
    object.insert(QStringLiteral("eraWeight"), weights.eraWeight);
    object.insert(QStringLiteral("eraSpanYears"), weights.eraSpanYears);
    object.insert(QStringLiteral("tempoWeight"), weights.tempoWeight);
    object.insert(QStringLiteral("energyWeight"), weights.energyWeight);
    object.insert(QStringLiteral("audioWeight"), weights.audioWeight);
    object.insert(QStringLiteral("ratingWeight"), weights.ratingWeight);
    object.insert(QStringLiteral("userRatingBoost"), weights.userRatingBoost);
    object.insert(QStringLiteral("historyWeight"), weights.historyWeight);
    object.insert(QStringLiteral("historySaturation"), weights.historySaturation);
    object.insert(QStringLiteral("noveltyWeight"), weights.noveltyWeight);
    object.insert(QStringLiteral("noveltyZeroAt"), weights.noveltyZeroAt);
    object.insert(QStringLiteral("recencyPenalty"), weights.recencyPenalty);
    object.insert(QStringLiteral("recencyHalfLifeDays"), weights.recencyHalfLifeDays);
    object.insert(QStringLiteral("skipPenalty"), weights.skipPenalty);
    object.insert(QStringLiteral("sameArtistPenalty"), weights.sameArtistPenalty);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
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
    const QStringList keys = weightKeys();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!keys.contains(it.key())) {
            if (error != nullptr) {
                *error = QStringLiteral("unknown radio scoring weight: %1").arg(it.key());
            }
            return weights;
        }
    }

    if (!assignNumber(object, QStringLiteral("genreWeight"), weights.genreWeight, 0.0, std::numeric_limits<double>::infinity(), error)
        || !assignNumber(object, QStringLiteral("genreIdfSaturation"), weights.genreIdfSaturation, 0.001, std::numeric_limits<double>::infinity(), error)
        || !assignNumber(object, QStringLiteral("genreCrowdingSoftLimit"), weights.genreCrowdingSoftLimit, 1.0, std::numeric_limits<double>::infinity(), error)
        || !assignNumber(object, QStringLiteral("eraWeight"), weights.eraWeight, 0.0, std::numeric_limits<double>::infinity(), error)
        || !assignNumber(object, QStringLiteral("eraSpanYears"), weights.eraSpanYears, 0.001, std::numeric_limits<double>::infinity(), error)
        || !assignNumber(object, QStringLiteral("tempoWeight"), weights.tempoWeight, 0.0, std::numeric_limits<double>::infinity(), error)
        || !assignNumber(object, QStringLiteral("energyWeight"), weights.energyWeight, 0.0, std::numeric_limits<double>::infinity(), error)
        || !assignNumber(object, QStringLiteral("audioWeight"), weights.audioWeight, 0.0, std::numeric_limits<double>::infinity(), error)
        || !assignNumber(object, QStringLiteral("ratingWeight"), weights.ratingWeight, 0.0, std::numeric_limits<double>::infinity(), error)
        || !assignNumber(object, QStringLiteral("userRatingBoost"), weights.userRatingBoost, 0.0, std::numeric_limits<double>::infinity(), error)
        || !assignNumber(object, QStringLiteral("historyWeight"), weights.historyWeight, 0.0, std::numeric_limits<double>::infinity(), error)
        || !assignNumber(object, QStringLiteral("historySaturation"), weights.historySaturation, 0.001, std::numeric_limits<double>::infinity(), error)
        || !assignNumber(object, QStringLiteral("noveltyWeight"), weights.noveltyWeight, 0.0, std::numeric_limits<double>::infinity(), error)
        || !assignNumber(object, QStringLiteral("noveltyZeroAt"), weights.noveltyZeroAt, 0.001, std::numeric_limits<double>::infinity(), error)
        || !assignNumber(object, QStringLiteral("recencyPenalty"), weights.recencyPenalty, -100.0, 0.0, error)
        || !assignNumber(object, QStringLiteral("recencyHalfLifeDays"), weights.recencyHalfLifeDays, 0.001, std::numeric_limits<double>::infinity(), error)
        || !assignNumber(object, QStringLiteral("skipPenalty"), weights.skipPenalty, -100.0, 0.0, error)
        || !assignNumber(object, QStringLiteral("sameArtistPenalty"), weights.sameArtistPenalty, -100.0, 0.0, error)) {
        return weights;
    }
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

    // tempo/energy: acoustic proximity to the current sonic context. Unknown
    // values stay silent so a closed or unfeatured FeatureStore preserves the
    // previous score exactly.
    if (candidate.tempoBpm > 0.0 && seed.contextTempoBpm > 0.0) {
        pushIfNonZero(scored, QStringLiteral("tempo"),
                      weights.tempoWeight * linearProximity(candidate.tempoBpm,
                                                            seed.contextTempoBpm,
                                                            kTempoFalloffBpm));
    }
    if (candidate.energy >= 0.0 && seed.contextEnergy >= 0.0) {
        pushIfNonZero(scored, QStringLiteral("energy"),
                      weights.energyWeight * linearProximity(candidate.energy,
                                                             seed.contextEnergy,
                                                             kEnergyFalloff));
    }

    // audio: cosine proximity to the rolling CLAP embedding centroid. Vectors
    // are stored normalized in features.sqlite and the session normalizes the
    // centroid; negative cosine is dissimilarity, not a penalty.
    if (candidate.contentGroupId >= 0 && seed.embeddingsByGroup != nullptr && !seed.audioCentroid.isEmpty()) {
        const auto it = seed.embeddingsByGroup->constFind(candidate.contentGroupId);
        if (it != seed.embeddingsByGroup->constEnd()) {
            pushIfNonZero(scored, QStringLiteral("audio"),
                          weights.audioWeight * std::max(0.0, dotProduct(it.value(), seed.audioCentroid)));
        }
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
