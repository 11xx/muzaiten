#include "reco/WeightLearner.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

double clampedExp(double value)
{
    return std::exp(std::clamp(value, -40.0, 40.0));
}

double sigmoid(double value)
{
    if (value >= 0.0) {
        const double z = std::exp(-value);
        return 1.0 / (1.0 + z);
    }
    const double z = std::exp(value);
    return z / (1.0 + z);
}

double clampedMultiplier(double coefficient, double defaultWeight)
{
    if (defaultWeight == 0.0) {
        return 1.0;
    }
    // exp(-coefficient) for rewards AND penalties: a positive coefficient
    // means the component's recorded contribution raised skip probability.
    // For a reward that discredits the component (shrink). For a penalty the
    // coefficient is driven by NEGATIVE feature values, so predicting skips
    // shows up as a negative coefficient — the penalty warned and was
    // ignored, so it grows in magnitude; a positive coefficient means the
    // un-penalized tracks were the ones skipped and the penalty shrinks.
    return std::clamp(clampedExp(-coefficient), 0.25, 4.0);
}

void setComponentWeight(TrackScorer::Weights &weights, const QString &componentName, double value)
{
    if (componentName == QLatin1String("genre")) {
        weights.genreWeight = value;
    } else if (componentName == QLatin1String("era")) {
        weights.eraWeight = value;
    } else if (componentName == QLatin1String("tempo")) {
        weights.tempoWeight = value;
    } else if (componentName == QLatin1String("energy")) {
        weights.energyWeight = value;
    } else if (componentName == QLatin1String("audio")) {
        weights.audioWeight = value;
    } else if (componentName == QLatin1String("rating")) {
        weights.ratingWeight = value;
    } else if (componentName == QLatin1String("history")) {
        weights.historyWeight = value;
    } else if (componentName == QLatin1String("novelty")) {
        weights.noveltyWeight = value;
    } else if (componentName == QLatin1String("recency")) {
        weights.recencyPenalty = value;
    } else if (componentName == QLatin1String("skips")) {
        weights.skipPenalty = value;
    } else if (componentName == QLatin1String("same-artist")) {
        weights.sameArtistPenalty = value;
    }
}

} // namespace

namespace WeightLearner {

QVector<ComponentSpec> componentSpecs()
{
    const TrackScorer::Weights defaults = TrackScorer::defaultWeights();
    return {
        {QStringLiteral("genre"), QStringLiteral("genreWeight"), defaults.genreWeight},
        {QStringLiteral("era"), QStringLiteral("eraWeight"), defaults.eraWeight},
        {QStringLiteral("tempo"), QStringLiteral("tempoWeight"), defaults.tempoWeight},
        {QStringLiteral("energy"), QStringLiteral("energyWeight"), defaults.energyWeight},
        {QStringLiteral("audio"), QStringLiteral("audioWeight"), defaults.audioWeight},
        {QStringLiteral("rating"), QStringLiteral("ratingWeight"), defaults.ratingWeight},
        {QStringLiteral("history"), QStringLiteral("historyWeight"), defaults.historyWeight},
        {QStringLiteral("novelty"), QStringLiteral("noveltyWeight"), defaults.noveltyWeight},
        {QStringLiteral("recency"), QStringLiteral("recencyPenalty"), defaults.recencyPenalty},
        {QStringLiteral("skips"), QStringLiteral("skipPenalty"), defaults.skipPenalty},
        {QStringLiteral("same-artist"), QStringLiteral("sameArtistPenalty"), defaults.sameArtistPenalty},
    };
}

bool componentWeight(const TrackScorer::Weights &weights, const QString &componentName, double *weight)
{
    double value = 0.0;
    if (componentName == QLatin1String("genre")) {
        value = weights.genreWeight;
    } else if (componentName == QLatin1String("era")) {
        value = weights.eraWeight;
    } else if (componentName == QLatin1String("tempo")) {
        value = weights.tempoWeight;
    } else if (componentName == QLatin1String("energy")) {
        value = weights.energyWeight;
    } else if (componentName == QLatin1String("audio")) {
        value = weights.audioWeight;
    } else if (componentName == QLatin1String("rating")) {
        value = weights.ratingWeight;
    } else if (componentName == QLatin1String("history")) {
        value = weights.historyWeight;
    } else if (componentName == QLatin1String("novelty")) {
        value = weights.noveltyWeight;
    } else if (componentName == QLatin1String("recency")) {
        value = weights.recencyPenalty;
    } else if (componentName == QLatin1String("skips")) {
        value = weights.skipPenalty;
    } else if (componentName == QLatin1String("same-artist")) {
        value = weights.sameArtistPenalty;
    } else {
        return false;
    }
    if (weight != nullptr) {
        *weight = value;
    }
    return true;
}

Result learn(const QVector<Sample> &samples, Options options)
{
    Result result;
    result.sampleCount = static_cast<int>(samples.size());
    for (const Sample &sample : samples) {
        if (sample.earlySkip) {
            ++result.positiveLabels;
        }
    }

    options.minSamples = std::max(1, options.minSamples);
    options.minPositiveLabels = std::max(1, options.minPositiveLabels);
    options.iterations = std::max(1, options.iterations);
    options.learningRate = std::max(0.0001, options.learningRate);
    options.l2Lambda = std::max(0.0, options.l2Lambda);

    if (result.sampleCount < options.minSamples) {
        result.error = QStringLiteral("not enough listening data yet (have %1, need %2)")
                           .arg(result.sampleCount)
                           .arg(options.minSamples);
        return result;
    }
    if (result.positiveLabels < options.minPositiveLabels) {
        result.error = QStringLiteral("not enough early-skip listening data yet (have %1, need %2)")
                           .arg(result.positiveLabels)
                           .arg(options.minPositiveLabels);
        return result;
    }

    const QVector<ComponentSpec> specs = componentSpecs();
    QVector<double> coefficients(specs.size(), 0.0);
    QVector<int> nonZeroSamples(specs.size(), 0);

    const double positive = static_cast<double>(result.positiveLabels);
    const double negative = static_cast<double>(result.sampleCount - result.positiveLabels);
    double intercept = std::log(positive / std::max(1.0, negative));

    QVector<double> gradients(specs.size(), 0.0);
    for (int iteration = 0; iteration < options.iterations; ++iteration) {
        std::fill(gradients.begin(), gradients.end(), 0.0);
        double interceptGradient = 0.0;

        for (const Sample &sample : samples) {
            double z = intercept;
            for (qsizetype i = 0; i < specs.size(); ++i) {
                z += coefficients[i] * sample.features.value(specs[i].componentName, 0.0);
            }

            const double prediction = sigmoid(z);
            const double error = prediction - (sample.earlySkip ? 1.0 : 0.0);
            interceptGradient += error;
            for (qsizetype i = 0; i < specs.size(); ++i) {
                gradients[i] += error * sample.features.value(specs[i].componentName, 0.0);
            }
        }

        const double n = static_cast<double>(result.sampleCount);
        intercept -= options.learningRate * interceptGradient / n;
        for (qsizetype i = 0; i < specs.size(); ++i) {
            const double regularized = gradients[i] / n + (options.l2Lambda * coefficients[i]) / n;
            coefficients[i] -= options.learningRate * regularized;
        }
    }

    result.suggestedWeights = TrackScorer::defaultWeights();
    result.components.reserve(specs.size());
    for (qsizetype i = 0; i < specs.size(); ++i) {
        for (const Sample &sample : samples) {
            if (sample.features.value(specs[i].componentName, 0.0) != 0.0) {
                ++nonZeroSamples[i];
            }
        }

        const double multiplier = clampedMultiplier(coefficients[i], specs[i].defaultWeight);
        const double suggested = specs[i].defaultWeight * multiplier;
        setComponentWeight(result.suggestedWeights, specs[i].componentName, suggested);
        result.components.push_back({
            specs[i].componentName,
            specs[i].weightKey,
            coefficients[i],
            multiplier,
            specs[i].defaultWeight,
            suggested,
            nonZeroSamples[i],
        });
    }

    result.suggestedWeightsJson = TrackScorer::weightsToJson(result.suggestedWeights);
    result.ok = true;
    return result;
}

} // namespace WeightLearner
