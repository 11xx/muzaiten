#pragma once

#include "core/MusicBrainzIds.h"
#include "core/Rating.h"

#include <QString>
#include <QtTypes>

struct Track {
    QString path;
    QString parentDir;
    QString filename;
    QString title;
    QString artistName;
    QString albumArtistName;
    QString albumTitle;
    QString date;
    QString originalDate;
    int trackNumber = 0;
    int trackTotal = 0;
    int discNumber = 0;
    int discTotal = 0;
    qint64 durationMs = 0;
    int rating0To100 = Rating::unset;
    Rating::Source ratingSource = Rating::Source::None;
    int playCount = 0;
    MusicBrainzIds musicBrainz;
    qint64 fileSize = 0;
    qint64 fileMtime = 0;
    QString scanError;
};
