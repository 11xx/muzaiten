#pragma once

#include <QtTypes>

// File identity used by the incremental-rescan diff: a path maps to this record.
// The scanner skips re-reading a file only when mtime+size are unchanged AND its
// metadata has already been read (metadataScanned). Enumerated-only placeholder
// rows carry mtime+size but no tags (metadataScanned == false), so they are
// always re-queued for a metadata read rather than skipped.
struct TrackFingerprint {
    qint64 mtime = 0;
    qint64 size = 0;
    bool metadataScanned = true;
};
