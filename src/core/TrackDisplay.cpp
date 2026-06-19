#include "core/TrackDisplay.h"

#include "core/Track.h"

#include <QFileInfo>

namespace trackdisplay {

QString title(const Track &track)
{
    if (!track.title.trimmed().isEmpty()) {
        return track.title;
    }
    const QString file = track.filename.isEmpty() ? track.path : track.filename;
    const QString base = QFileInfo(file).completeBaseName();
    return base.isEmpty() ? file : base;
}

QString artist(const Track &track)
{
    return track.albumArtistName.trimmed().isEmpty() ? track.artistName : track.albumArtistName;
}

QString year(const Track &track)
{
    for (const QString &candidate : {track.originalDate, track.date}) {
        const QString trimmed = candidate.trimmed();
        if (!trimmed.isEmpty()) {
            return trimmed.left(4);
        }
    }
    return {};
}

} // namespace trackdisplay
