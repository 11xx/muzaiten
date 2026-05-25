#pragma once

#include "core/Rating.h"

#include <taglib/tpropertymap.h>

struct TagRatingReadResult {
    int rating0To100 = Rating::unset;
    Rating::Source source = Rating::Source::None;
};

TagRatingReadResult readRating(const TagLib::PropertyMap &properties);
void setMusicBeeRating(TagLib::PropertyMap &properties, int rating0To100);
