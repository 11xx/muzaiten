#pragma once

#include <QString>

#include "core/Rating.h"

struct Album {
    qint64 id = 0;
    QString title;
    QString albumArtistName;
    QString date;
    QString representativeDir;
    QString artworkCachePath;
    int trackCount = 0;
    int knownRatingCount = 0;
    int averageRating0To100 = -1;
    int effectiveRating0To100 = Rating::unset;
    bool hasUserRating = false;
};
