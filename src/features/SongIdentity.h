#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QtTypes>

namespace SongIdentity {

struct TrackIdentity {
    QString path;
    QString artist;
    QString title;
    QString mbRecordingId;
    qint64 contentGroupId = -1;
};

// Builds path -> resolved song key. Content groups are strongest, MBID equality
// is next, and artist/title fallback only merges rows that lack an MBID, so a
// closed FeatureStore preserves the FoldKey::songKey-only behavior.
QHash<QString, QString> resolvedSongKeys(const QList<TrackIdentity> &tracks);

// Returns the transitive identity component containing targetPath. This is the
// narrow counterpart to resolvedSongKeys: callers may supply only rows reached
// by walking group, MBID, and fallback-key indexes instead of the whole library.
QStringList pathsConnectedToTrack(const QList<TrackIdentity> &tracks, const QString &targetPath);

} // namespace SongIdentity
