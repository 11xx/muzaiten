#pragma once

#include "reco/WeightLearner.h"

#include <QVector>
#include <QString>
#include <QtTypes>

class QSqlDatabase;

namespace WeightLearnerData {

// A pick joins the first radio play event, or else the first unheard queue
// removal, of the same track within this many seconds after the pick.
inline constexpr int kJoinWindowSecs = 12 * 60 * 60;

// Label weight of a pick removed from the queue unheard: a rejection, but
// with less evidence than a skip of a track that actually started playing.
inline constexpr double kRemovalWeight = 0.5;

struct LoadResult {
    QVector<WeightLearner::Sample> samples;
    QString error;
    int playSamples = 0;       // picks joined to a play event
    int removalSamples = 0;    // picks joined to an unheard queue removal
    int skippedInvalidWeights = 0;
    int skippedNoSignals = 0;
};

bool isEarlySkip(const QString &outcome, qint64 playedMs, qint64 durationMs);
LoadResult loadSamples(const QSqlDatabase &history);
LoadResult loadSamplesFromPath(const QString &path);

} // namespace WeightLearnerData
