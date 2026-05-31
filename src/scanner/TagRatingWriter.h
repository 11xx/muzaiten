#pragma once

#include <QString>

struct TagRatingWriteResult {
    bool ok = false;
    QString error;
};

class TagRatingWriter final {
public:
    TagRatingWriteResult writeRating(const QString &path, int rating0To100) const;
};
