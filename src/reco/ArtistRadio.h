#pragma once

#include "core/Track.h"
#include "reco/TrackScorer.h"

#include <QHash>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace ArtistRadio {

constexpr int kArtistSeedGenreCap = 4;

QStringList aggregateSeedGenres(const QVector<QPair<QString, int>> &genreCounts,
                                const QHash<QString, QString> &aliases,
                                const QSet<QString> &ignored,
                                int cap = kArtistSeedGenreCap);

int medianTrackYear(const QVector<Track> &tracks);

Track representativeTrack(const QVector<Track> &tracks,
                          const QHash<QString, TrackScorer::Affinity> &affinities);

TrackScorer::Candidate syntheticSeedCandidate(const QString &artistName,
                                              const QStringList &genresFolded,
                                              int year);

} // namespace ArtistRadio
