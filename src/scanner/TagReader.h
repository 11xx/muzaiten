#pragma once

#include "core/MetadataBlob.h"
#include "core/Track.h"

#include <QString>

class TagReader final {
public:
    // Reads curated fields into the returned Track. When fullMetadata is
    // non-null, it is additionally populated with the complete tag set and
    // technical audio properties (no images).
    Track read(const QString &path, MetadataBlob::FullMetadata *fullMetadata = nullptr) const;
};
