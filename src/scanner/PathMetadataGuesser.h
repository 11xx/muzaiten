#pragma once

#include <QString>

// Transient, best-effort guesser that derives artist/album/title/track from a
// file's path while its real tags are still pending the background metadata fill.
// It is deliberately a separate, self-contained module (no DB, no Qt-GUI) that the
// scanner only opts into: removing it and its one call site reverts to plain,
// directory-view-only placeholders.
//
// The string-splitting and clean-up heuristics are adapted from the MIT-licensed
// Web Scrobbler (separator list + first-separator split) and Metadata Filter
// (trim/clean-up filter rules) projects — the same parsers credited by Pano
// Scrobbler. See https://github.com/web-scrobbler/web-scrobbler and
// https://github.com/web-scrobbler/metadata-filter.
struct GuessedMetadata {
    QString artist;
    QString albumArtist;
    QString album;
    QString title;
    int trackNumber = 0;

    bool isEmpty() const { return artist.isEmpty() && album.isEmpty() && title.isEmpty(); }
};

namespace PathMetadataGuesser {

// Guess metadata for filePath relative to libraryRoot (used to scope how many of
// the parent directories are "Artist"/"Album"). Both should be absolute paths.
GuessedMetadata guess(const QString &filePath, const QString &libraryRoot);

} // namespace PathMetadataGuesser
