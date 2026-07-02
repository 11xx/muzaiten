#include "reco/RadioFilters.h"

namespace RadioFilters {

QVector<TrackScorer::Candidate> excludeFlaggedCandidates(
    const QVector<TrackScorer::Candidate> &candidates,
    const QSet<QString> &flaggedPaths)
{
    if (flaggedPaths.isEmpty()) {
        return candidates;
    }

    QVector<TrackScorer::Candidate> filtered;
    filtered.reserve(candidates.size());
    for (const TrackScorer::Candidate &candidate : candidates) {
        if (!flaggedPaths.contains(candidate.path)) {
            filtered.push_back(candidate);
        }
    }
    return filtered;
}

QHash<QString, TrackScorer::Affinity> excludeFlaggedAffinities(
    const QHash<QString, TrackScorer::Affinity> &affinities,
    const QSet<QString> &flaggedPaths)
{
    if (flaggedPaths.isEmpty()) {
        return affinities;
    }

    QHash<QString, TrackScorer::Affinity> filtered;
    filtered.reserve(affinities.size());
    for (auto it = affinities.cbegin(); it != affinities.cend(); ++it) {
        if (!flaggedPaths.contains(it.key())) {
            filtered.insert(it.key(), it.value());
        }
    }
    return filtered;
}

QHash<QString, QString> excludeFlaggedPathMappings(
    const QHash<QString, QString> &pathToSongKey,
    const QSet<QString> &flaggedPaths)
{
    if (flaggedPaths.isEmpty()) {
        return pathToSongKey;
    }

    QHash<QString, QString> filtered;
    filtered.reserve(pathToSongKey.size());
    for (auto it = pathToSongKey.cbegin(); it != pathToSongKey.cend(); ++it) {
        if (!flaggedPaths.contains(it.key())) {
            filtered.insert(it.key(), it.value());
        }
    }
    return filtered;
}

} // namespace RadioFilters
