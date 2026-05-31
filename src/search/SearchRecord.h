#pragma once

// Lightweight per-track record held in the in-memory search index.  One record
// per library + MPD track; built once when the index is (re)loaded, kept for
// the session. Only the fields needed for matching and display are stored here.
// Full Track objects are resolved on-demand via Database::trackForPath when the
// user enqueues a selection.

#include <QString>
#include <QtTypes>

namespace Search {

enum class TrackSource { Local, Mpd };

struct SearchRecord {
    // ---- display (original case) ----------------------------------------
    QString title;
    QString artistName;
    QString albumArtistName;
    QString albumTitle;
    QString date;        // year string, e.g. "2024" or "2024-03-15"
    QString filename;    // basename only, e.g. "01 So What.flac"
    QString path;        // full path or MPD URI

    // ---- normalized for case-insensitive matching (lowercased) -----------
    QString normTitle;
    QString normArtist;
    QString normAlbumArtist;
    QString normAlbum;
    QString normFilename;
    QString normPath;

    // ---- numeric / short-string tokens ------------------------------------
    qint64 durationMs = 0;
    int rating0To100 = -1;   // effective rating; -1 = unset
    int sampleRateHz = 0;
    int bitrateKbps = 0;
    int channels = 0;
    int bitDepth = 0;        // not scanned yet (placeholder for a future scan); 0 = unknown
    QString codec;           // lower-cased extension / codec, e.g. "flac"

    // ---- ordering helpers (used by the ranking layer / MusicSort) ---------
    int trackNumber = 0;
    int discNumber = 0;
    qint64 fileMtime = 0;
    qint64 fileSize = 0;

    TrackSource source = TrackSource::Local;
};

} // namespace Search
