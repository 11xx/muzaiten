#include "reco/AffinityPool.h"

#include <QStringList>

#include <algorithm>

namespace {

void addAffinity(TrackScorer::Affinity &pooled, const TrackScorer::Affinity &affinity)
{
    pooled.playEvents += affinity.playEvents;
    pooled.finished += affinity.finished;
    pooled.skipped += affinity.skipped;
    pooled.listenCount += affinity.listenCount;
    pooled.lastPlayedAtSecs = std::max(pooled.lastPlayedAtSecs, affinity.lastPlayedAtSecs);
    pooled.baselineMax = std::max(pooled.baselineMax, affinity.baselineMax);
}

} // namespace

namespace AffinityPool {

QHash<QString, TrackScorer::Affinity> poolBySongKey(
    const QHash<QString, TrackScorer::Affinity> &byPath,
    const QHash<QString, QString> &pathToSongKey)
{
    QHash<QString, TrackScorer::Affinity> pooledByPath = byPath;
    if (byPath.isEmpty() || pathToSongKey.isEmpty()) {
        return pooledByPath;
    }

    QHash<QString, QStringList> pathsBySongKey;
    pathsBySongKey.reserve(pathToSongKey.size());
    for (auto it = pathToSongKey.cbegin(); it != pathToSongKey.cend(); ++it) {
        if (!it.value().isEmpty()) {
            pathsBySongKey[it.value()].push_back(it.key());
        }
    }

    QHash<QString, TrackScorer::Affinity> pooledBySongKey;
    for (auto it = byPath.cbegin(); it != byPath.cend(); ++it) {
        const QString songKey = pathToSongKey.value(it.key());
        if (songKey.isEmpty()) {
            continue;
        }
        addAffinity(pooledBySongKey[songKey], it.value());
    }

    for (auto it = pooledBySongKey.cbegin(); it != pooledBySongKey.cend(); ++it) {
        const QStringList paths = pathsBySongKey.value(it.key());
        for (const QString &path : paths) {
            pooledByPath.insert(path, it.value());
        }
    }
    return pooledByPath;
}

} // namespace AffinityPool
