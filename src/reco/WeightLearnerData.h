#pragma once

#include "reco/WeightLearner.h"

#include <QVector>
#include <QString>
#include <QtTypes>

class QSqlDatabase;

namespace WeightLearnerData {

inline constexpr int kJoinWindowSecs = 12 * 60 * 60;

struct LoadResult {
    QVector<WeightLearner::Sample> samples;
    QString error;
    int skippedInvalidWeights = 0;
    int skippedNoSignals = 0;
};

bool isEarlySkip(const QString &outcome, qint64 playedMs, qint64 durationMs);
LoadResult loadSamples(const QSqlDatabase &history);
LoadResult loadSamplesFromPath(const QString &path);

} // namespace WeightLearnerData
