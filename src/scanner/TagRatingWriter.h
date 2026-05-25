#pragma once

#include "core/Rating.h"

#include <QString>

struct TagRatingWriteResult {
    bool ok = false;
    QString error;
    bool existingTagWon = false;
    int fileRating0To100 = Rating::unset;
};

class TagRatingWriter final {
public:
    TagRatingWriteResult writeRating(const QString &path, int rating0To100) const;
};
