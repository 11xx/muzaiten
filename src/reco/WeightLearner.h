#pragma once

#include "reco/TrackScorer.h"

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>

namespace WeightLearner {

struct ComponentSpec {
    QString componentName;
    QString weightKey;
    double defaultWeight = 0.0;
};

struct Sample {
    QHash<QString, double> features;
    bool earlySkip = false;
};

struct Options {
    int minSamples = 200;
    int minPositiveLabels = 20;
    int iterations = 1200;
    double learningRate = 0.5;
    double l2Lambda = 1.0;
};

struct ComponentResult {
    QString componentName;
    QString weightKey;
    double coefficient = 0.0;
    double multiplier = 1.0;
    double defaultWeight = 0.0;
    double suggestedWeight = 0.0;
    int nonZeroSamples = 0;
};

struct Result {
    bool ok = false;
    QString error;
    int sampleCount = 0;
    int positiveLabels = 0;
    QVector<ComponentResult> components;
    TrackScorer::Weights suggestedWeights;
    QByteArray suggestedWeightsJson;
};

QVector<ComponentSpec> componentSpecs();
bool componentWeight(const TrackScorer::Weights &weights, const QString &componentName, double *weight);
Result learn(const QVector<Sample> &samples, Options options = {});

} // namespace WeightLearner
