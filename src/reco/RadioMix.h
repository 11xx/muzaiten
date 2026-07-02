#pragma once

#include "reco/TrackScorer.h"

#include <QHash>
#include <QString>
#include <QVector>

#include <optional>

namespace RadioMix {

enum class Mode {
    Rediscovery,
    DeepCuts,
};

std::optional<Mode> modeFromString(const QString &mode);

QVector<TrackScorer::Candidate> filterCandidates(
    Mode mode,
    const QVector<TrackScorer::Candidate> &candidates,
    const QHash<QString, TrackScorer::Affinity> &affinities,
    qint64 nowSecs);

bool isRediscoveryCandidate(const TrackScorer::Candidate &candidate,
                            const TrackScorer::Affinity &affinity,
                            qint64 nowSecs,
                            int minimumAgeDays);

bool isDeepCutCandidate(const TrackScorer::Candidate &candidate,
                        const QHash<QString, bool> &likedArtists,
                        const QHash<QString, TrackScorer::Affinity> &affinities);

} // namespace RadioMix
