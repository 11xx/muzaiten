#pragma once

#include "reco/TrackScorer.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

namespace RadioFilters {

QVector<TrackScorer::Candidate> excludeFlaggedCandidates(
    const QVector<TrackScorer::Candidate> &candidates,
    const QSet<QString> &flaggedPaths);

QHash<QString, TrackScorer::Affinity> excludeFlaggedAffinities(
    const QHash<QString, TrackScorer::Affinity> &affinities,
    const QSet<QString> &flaggedPaths);

QHash<QString, QString> excludeFlaggedPathMappings(
    const QHash<QString, QString> &pathToSongKey,
    const QSet<QString> &flaggedPaths);

} // namespace RadioFilters
