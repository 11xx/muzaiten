#pragma once

#include "core/MetadataBlob.h"

#include <QString>
#include <QStringList>

// Genre normalization shared between the scan-time track_genres population and
// the one-time blob backfill (see Database::migrate, v10). Keeps "grouping"
// (folded) and "display" (as tagged) genre forms consistent everywhere genres
// are read out of a metadata blob.
namespace GenreTags {

// Case-fold + whitespace-normalize a genre for grouping/joins.
QString folded(const QString &genre);

// Display-form genres from a decoded metadata blob: all values of the GENRE
// tag, split on ';' ',' '/' and NUL, trimmed/simplified, empties dropped,
// deduped case-insensitively (first-seen casing wins). Order preserved.
QStringList fromMetadata(const MetadataBlob::FullMetadata &metadata);

// True for tagger placeholders that are not real genres ("Other", "Unknown",
// ...): junk co-tags that some taggers write in lieu of leaving GENRE empty.
// They must never seed a radio candidate pool nor count as a genre match —
// callers should filter with `informative()` before either. `folded` must
// already be case/whitespace-folded (see `folded()` above).
bool isNonGenre(const QString &folded);

// `foldedGenres` with every `isNonGenre()` placeholder removed, order
// preserved.
QStringList informative(const QStringList &foldedGenres);

} // namespace GenreTags
