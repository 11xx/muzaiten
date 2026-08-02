#include "reco/TrackScorer.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace {

constexpr double kTempoFalloffOctaves = 1.0;
constexpr double kEnergyFalloff = 1.0;

struct WeightEntry {
    const char *key;
    double TrackScorer::Weights::*member;
    double minimum;
    double maximum;
    const char *label;
    const char *description;
    bool fallbackOnly;
};

constexpr double kMaximumWeight = std::numeric_limits<double>::max();

constexpr std::array<WeightEntry, 18> kWeightEntries{{
    {"genreWeight", &TrackScorer::Weights::genreWeight, 0.0, kMaximumWeight,
     "Genre match", "Reward for matching the seed and rolling-context genres.", true},
    {"genreIdfSaturation", &TrackScorer::Weights::genreIdfSaturation, 0.001, kMaximumWeight,
     "Genre rarity saturation", "Shared genre rarity value that reaches the full genre reward.", true},
    {"genreCrowdingSoftLimit", &TrackScorer::Weights::genreCrowdingSoftLimit, 1.0, kMaximumWeight,
     "Genre crowding soft limit", "Genre counts above this are damped so tag soup does not dominate.", true},
    {"eraWeight", &TrackScorer::Weights::eraWeight, 0.0, kMaximumWeight,
     "Era proximity", "Reward for release years close to the current context.", false},
    {"eraSpanYears", &TrackScorer::Weights::eraSpanYears, 0.001, kMaximumWeight,
     "Era span", "Year distance where the era reward falls to zero.", false},
    {"tempoWeight", &TrackScorer::Weights::tempoWeight, 0.0, kMaximumWeight,
     "Tempo proximity", "Reward for tempo close to the current sonic context.", false},
    {"energyWeight", &TrackScorer::Weights::energyWeight, 0.0, kMaximumWeight,
     "Energy proximity", "Reward for DSP energy close to the current sonic context.", false},
    {"audioWeight", &TrackScorer::Weights::audioWeight, 0.0, kMaximumWeight,
     "Audio similarity", "Reward for CLAP embedding similarity to the session centroid.", false},
    {"ratingWeight", &TrackScorer::Weights::ratingWeight, 0.0, kMaximumWeight,
     "Rating", "Reward from effective library rating.", false},
    {"userRatingBoost", &TrackScorer::Weights::userRatingBoost, 0.0, kMaximumWeight,
     "User rating boost", "Multiplier for ratings explicitly set by the user.", false},
    {"historyWeight", &TrackScorer::Weights::historyWeight, 0.0, kMaximumWeight,
     "Listening history", "Reward from accumulated local and imported listens.", false},
    {"historySaturation", &TrackScorer::Weights::historySaturation, 0.001, kMaximumWeight,
     "History saturation", "Listen count where the history reward nears its full value.", false},
    {"noveltyWeight", &TrackScorer::Weights::noveltyWeight, 0.0, kMaximumWeight,
     "Novelty", "Reward for tracks with little or no listening history.", false},
    {"noveltyZeroAt", &TrackScorer::Weights::noveltyZeroAt, 0.001, kMaximumWeight,
     "Novelty zero point", "Listen count where the novelty reward falls to zero.", false},
    {"recencyPenalty", &TrackScorer::Weights::recencyPenalty, -100.0, 0.0,
     "Recency penalty", "Penalty for tracks played recently.", false},
    {"recencyHalfLifeDays", &TrackScorer::Weights::recencyHalfLifeDays, 0.001, kMaximumWeight,
     "Recency half-life", "Days for the recent-play penalty to halve.", false},
    {"skipPenalty", &TrackScorer::Weights::skipPenalty, -100.0, 0.0,
     "Skip penalty", "Penalty for tracks with a high skip rate.", false},
    {"sameArtistPenalty", &TrackScorer::Weights::sameArtistPenalty, -100.0, 0.0,
     "Same artist penalty", "Soft penalty for repeating recently heard artists.", false},
}};

auto findWeightEntry(const QString &key)
{
    return std::find_if(kWeightEntries.cbegin(), kWeightEntries.cend(), [&key](const WeightEntry &entry) {
        return key == QLatin1String(entry.key);
    });
}

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

double octaveTolerantTempoProximity(double candidateBpm, double contextBpm)
{
    constexpr std::array<double, 3> kOctaveMultipliers{0.5, 1.0, 2.0};
    double minimumDistance = std::numeric_limits<double>::infinity();
    for (const double multiplier : kOctaveMultipliers) {
        minimumDistance = std::min(minimumDistance,
                                   std::abs(std::log2(candidateBpm / (contextBpm * multiplier))));
    }
    return std::max(0.0, 1.0 - minimumDistance / kTempoFalloffOctaves);
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

bool hasUsableVector(const QVector<float> &vector)
{
    return !vector.isEmpty() && std::all_of(vector.cbegin(), vector.cend(), [](float value) {
        return std::isfinite(value);
    });
}

bool hasInformativeGenre(const QString &genre)
{
    const QString normalized = genre.trimmed();
    return !normalized.isEmpty()
        && normalized.compare(QLatin1String("unknown"), Qt::CaseInsensitive) != 0
        && normalized.compare(QLatin1String("untagged"), Qt::CaseInsensitive) != 0;
}

bool hasCompleteDspClapPair(const TrackScorer::Candidate &candidate,
                            const TrackScorer::SeedContext &seed)
{
    if (!candidate.hasValidDspClap() || !seed.hasValidDspClap()) {
        return false;
    }

    const bool completeScalars = candidate.tempoBpm > 0.0 && std::isfinite(candidate.tempoBpm)
        && candidate.energy >= 0.0 && std::isfinite(candidate.energy)
        && seed.contextTempoBpm > 0.0 && std::isfinite(seed.contextTempoBpm)
        && seed.contextEnergy >= 0.0 && std::isfinite(seed.contextEnergy);
    if (completeScalars) {
        return true;
    }

    if (candidate.contentGroupId < 0 || seed.embeddingsByGroup == nullptr
        || !hasUsableVector(seed.audioCentroid)) {
        return false;
    }
    const auto it = seed.embeddingsByGroup->constFind(candidate.contentGroupId);
    return it != seed.embeddingsByGroup->constEnd()
        && hasUsableVector(it.value())
        && it.value().size() == seed.audioCentroid.size();
}

double applyDecayToWeight(double baseWeight, int trackNumber, const TrackScorer::RadioSessionDecay &decay,
                          double floor)
{
    // A zero scoring-weight is an intentional user setting, not an invitation
    // for the session floor to silently turn that signal back on.
    return baseWeight > 0.0
        ? std::max(floor, baseWeight * TrackScorer::computeDecayFactor(trackNumber, decay))
        : baseWeight;
}

} // namespace

namespace TrackScorer {

bool Candidate::hasValidDspClap() const
{
    const bool completeScalars = tempoBpm > 0.0 && std::isfinite(tempoBpm)
        && energy >= 0.0 && std::isfinite(energy);
    return completeScalars || contentGroupId >= 0;
}

bool Candidate::hasValidGenre() const
{
    return std::any_of(genresFolded.cbegin(), genresFolded.cend(), hasInformativeGenre);
}

bool SeedContext::hasValidDspClap() const
{
    const bool completeScalars = contextTempoBpm > 0.0 && std::isfinite(contextTempoBpm)
        && contextEnergy >= 0.0 && std::isfinite(contextEnergy);
    return completeScalars || (embeddingsByGroup != nullptr && hasUsableVector(audioCentroid));
}

bool SeedContext::hasValidGenre() const
{
    return std::any_of(genresFolded.cbegin(), genresFolded.cend(), hasInformativeGenre);
}

Weights defaultWeights()
{
    return {};
}

RadioSessionDecay defaultSessionDecay()
{
    return {};
}

double computeDecayFactor(int trackNumber, const RadioSessionDecay &decay)
{
    if (trackNumber < decay.decayStartTrack) {
        return 1.0;
    }

    const int depth = trackNumber - decay.decayStartTrack;
    if (decay.decayCurve < 0.5) {
        return std::max(0.0, 1.0 - static_cast<double>(depth) * 0.1);
    }
    return std::exp(-static_cast<double>(depth) * 0.15);
}

double applyDecayToNoveltyWeight(double baseWeight, int trackNumber, const RadioSessionDecay &decay)
{
    return applyDecayToWeight(baseWeight, trackNumber, decay, decay.noveltyDecayFloor);
}

double applyDecayToRatingWeight(double baseWeight, int trackNumber, const RadioSessionDecay &decay)
{
    return applyDecayToWeight(baseWeight, trackNumber, decay, decay.ratingDecayFloor);
}

Weights getWeights(bool dspAvailable)
{
    return getWeights(defaultWeights(), dspAvailable);
}

Weights getWeights(const Weights &weights, bool dspAvailable)
{
    Weights effective = weights;
    if (dspAvailable) {
        effective.genreWeight = 0.0;
    } else {
        effective.tempoWeight = 0.0;
        effective.energyWeight = 0.0;
        effective.audioWeight = 0.0;
    }
    return effective;
}

QVector<WeightSpec> weightSpecs()
{
    const Weights defaults = defaultWeights();
    QVector<WeightSpec> specs;
    specs.reserve(static_cast<qsizetype>(kWeightEntries.size()));
    for (const WeightEntry &entry : kWeightEntries) {
        specs.push_back({QString::fromLatin1(entry.key), QString::fromLatin1(entry.label),
                         QString::fromLatin1(entry.description), entry.minimum, entry.maximum,
                         defaults.*(entry.member), entry.fallbackOnly});
    }
    return specs;
}

bool weightValue(const Weights &weights, const QString &key, double *value)
{
    const auto entry = findWeightEntry(key);
    if (entry == kWeightEntries.cend()) {
        return false;
    }
    if (value != nullptr) {
        *value = weights.*(entry->member);
    }
    return true;
}

bool setWeightValue(Weights &weights, const QString &key, double value)
{
    const auto entry = findWeightEntry(key);
    if (entry == kWeightEntries.cend()) {
        return false;
    }
    weights.*(entry->member) = value;
    return true;
}

QByteArray weightsToJson(const Weights &weights)
{
    QJsonObject object;
    for (const WeightEntry &entry : kWeightEntries) {
        object.insert(QString::fromLatin1(entry.key), weights.*(entry.member));
    }
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
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (findWeightEntry(it.key()) == kWeightEntries.cend()) {
            if (error != nullptr) {
                *error = QStringLiteral("unknown radio scoring weight: %1").arg(it.key());
            }
            return weights;
        }
    }

    for (const WeightEntry &entry : kWeightEntries) {
        if (!assignNumber(object, QString::fromLatin1(entry.key), weights.*(entry.member),
                          entry.minimum, entry.maximum, error)) {
            // Invalid input rejects the whole object: keys assigned before the
            // failing one must not leak through, callers treat error as all-or-nothing.
            return defaultWeights();
        }
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
    const bool dspClapAvailable = hasCompleteDspClapPair(candidate, seed);
    const Weights effectiveWeights = getWeights(weights, dspClapAvailable);

    // Genre is a metadata fallback only. A complete DSP/CLAP pair is richer
    // and more reliable than tags, so it suppresses this component entirely.
    if (effectiveWeights.genreWeight != 0.0 && seed.hasValidGenre() && candidate.hasValidGenre()) {
        const QSet<QString> seedGenres(seed.genresFolded.cbegin(), seed.genresFolded.cend());
        double idfSum = 0.0;
        for (const QString &genre : candidate.genresFolded) {
            if (seedGenres.contains(genre)) {
                idfSum += seed.genreIdf.value(genre, 0.0);
            }
        }
        idfSum *= crowdingScale(seed.genresFolded.size(), candidate.genresFolded.size(),
                                effectiveWeights.genreCrowdingSoftLimit);
        const double genreScore = std::min(1.0, idfSum / effectiveWeights.genreIdfSaturation);
        const double explorationScale = 1.25 - exploration / 200.0;
        pushIfNonZero(scored, QStringLiteral("genre"),
                      effectiveWeights.genreWeight * genreScore * explorationScale);
    }

    // era: linear proximity in years, both years known.
    if (candidate.year > 0 && seed.year > 0) {
        const double delta = std::min(static_cast<double>(std::abs(candidate.year - seed.year)),
                                      effectiveWeights.eraSpanYears);
        pushIfNonZero(scored, QStringLiteral("era"),
                      effectiveWeights.eraWeight * (1.0 - delta / effectiveWeights.eraSpanYears));
    }

    // tempo/energy: acoustic proximity to the current sonic context. Unknown
    // values stay silent so a closed or unfeatured FeatureStore preserves the
    // previous score exactly.
    if (candidate.tempoBpm > 0.0 && seed.contextTempoBpm > 0.0) {
        pushIfNonZero(scored, QStringLiteral("tempo"),
                      effectiveWeights.tempoWeight * octaveTolerantTempoProximity(candidate.tempoBpm,
                                                                                   seed.contextTempoBpm));
    }
    if (candidate.energy >= 0.0 && seed.contextEnergy >= 0.0) {
        pushIfNonZero(scored, QStringLiteral("energy"),
                      effectiveWeights.energyWeight * linearProximity(candidate.energy,
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
                          effectiveWeights.audioWeight * std::max(0.0, dotProduct(it.value(), seed.audioCentroid)));
        }
    }

    // Behavioral signals stay active regardless of metadata or DSP availability.
    // rating: effective rating, with a boost when it is the user's own rating.
    if (candidate.effectiveRating0To100 >= 0) {
        const double base = applyDecayToRatingWeight(effectiveWeights.ratingWeight,
                                                     seed.sessionTrackNumber, seed.sessionDecay)
            * (candidate.effectiveRating0To100 / 100.0);
        pushIfNonZero(scored, QStringLiteral("rating"),
                      candidate.hasUserRating ? base * effectiveWeights.userRatingBoost : base);
    }

    // history: how much this track has been heard, saturating.
    const int heard = affinity.listenCount + affinity.baselineMax + affinity.finished;
    if (heard > 0) {
        const double ratio = std::min(1.0, std::log1p(heard) / std::log1p(effectiveWeights.historySaturation));
        pushIfNonZero(scored, QStringLiteral("history"), effectiveWeights.historyWeight * ratio);
    }

    // novelty: reward the unheard, decaying to nothing by kNoveltyZeroAt total
    // plays, and scaled up with exploration.
    const int totalPlays = affinity.playEvents + affinity.listenCount + affinity.baselineMax;
    const double noveltyRatio = std::max(0.0, 1.0 - totalPlays / effectiveWeights.noveltyZeroAt);
    if (noveltyRatio > 0.0) {
        const double explorationScale = 0.5 + exploration / 100.0;
        const double noveltyWeight = applyDecayToNoveltyWeight(effectiveWeights.noveltyWeight,
                                                                seed.sessionTrackNumber, seed.sessionDecay);
        pushIfNonZero(scored, QStringLiteral("novelty"), noveltyWeight * noveltyRatio * explorationScale);
    }

    // recency: penalize a track played recently, exponentially fading with time.
    if (affinity.lastPlayedAtSecs > 0 && seed.nowSecs > 0) {
        const double days = static_cast<double>(seed.nowSecs - affinity.lastPlayedAtSecs) / 86400.0;
        if (days >= 0.0) {
            pushIfNonZero(scored, QStringLiteral("recency"),
                          effectiveWeights.recencyPenalty
                              * std::exp(-days / effectiveWeights.recencyHalfLifeDays));
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
        pushIfNonZero(scored, QStringLiteral("skips"), effectiveWeights.skipPenalty * skipRate);
    }

    // same-artist: a soft nudge away from an artist heard in the rolling window.
    // The hard "no same artist within k picks" throttle lives in RadioSession.
    if (!candidate.artistFolded.isEmpty() && seed.recentArtistsFolded.contains(candidate.artistFolded)) {
        pushIfNonZero(scored, QStringLiteral("same-artist"), effectiveWeights.sameArtistPenalty);
    }

    return scored;
}

} // namespace TrackScorer
