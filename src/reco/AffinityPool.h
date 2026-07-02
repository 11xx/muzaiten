#pragma once

#include "reco/TrackScorer.h"

#include <QHash>
#include <QString>

namespace AffinityPool {

QHash<QString, TrackScorer::Affinity> poolBySongKey(
    const QHash<QString, TrackScorer::Affinity> &byPath,
    const QHash<QString, QString> &pathToSongKey);

} // namespace AffinityPool
