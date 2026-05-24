#pragma once

#include "core/MusicBrainzIds.h"

#include <QString>
#include <QtTypes>

struct MpdTrack {
    QString uri;
    QString title;
    QString artistName;
    QString albumArtistName;
    QString albumTitle;
    int trackNumber = 0;
    int discNumber = 0;
    qint64 durationMs = 0;
    QString date;
    MusicBrainzIds musicBrainz;
};
