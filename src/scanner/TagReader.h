#pragma once

#include "core/Track.h"

#include <QString>

class TagReader final {
public:
    Track read(const QString &path) const;
};
