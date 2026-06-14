#pragma once

// Lightweight per-track record held in the in-memory search index.  One record
// per library + MPD track; built once when the index is (re)loaded, kept for
// the session. Only the fields needed for matching and display are stored here.
// Full Track objects are resolved on-demand via Database::trackForPath when the
// user enqueues a selection.

#include "search/fold/Fold.h"

#include <QHash>
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

    // ---- sort/reading names (raw; folded into the norms below) -----------
    // Often romaji/kana for non-Latin titles (Picard *sort / *sorten tags).
    // Transient: folded into the norms then cleared to reclaim memory.
    QString titleSort;
    QString artistSort;
    QString albumArtistSort;
    QString albumSort;

    // ---- normalized for case-insensitive matching (folded) ---------------
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

// Compute a record's folded norm fields from its display fields and sort/reading
// names (full fold: diacritics/scripts + CJK romanization + sort-reading
// enrichment). When `pool` is given, the high-repetition norms (artist/
// album-artist/album) are interned through it to save memory. The raw sort
// names are folded in and then cleared to reclaim memory.
inline void foldRecordNorms(SearchRecord &rec, QHash<QString, QString> *pool = nullptr)
{
    const auto intern = [pool](const QString &v) {
        if (!pool || v.isEmpty()) {
            return v;
        }
        const auto it = pool->constFind(v);
        if (it != pool->constEnd()) {
            return it.value();
        }
        pool->insert(v, v);
        return v;
    };
    const auto foldField = [&](const QString &text, const QString &reading) {
        QString f = Fold::foldText(text);
        if (!reading.isEmpty()) {
            const QString fr = Fold::foldText(reading);
            if (!fr.isEmpty() && fr != f) {
                f += QLatin1Char(' ') + fr;
            }
        }
        return f;
    };
    rec.normTitle       = foldField(rec.title, rec.titleSort);
    rec.normArtist      = intern(foldField(rec.artistName, rec.artistSort));
    rec.normAlbumArtist = intern(foldField(rec.albumArtistName, rec.albumArtistSort));
    rec.normAlbum       = intern(foldField(rec.albumTitle, rec.albumSort));
    rec.normFilename    = Fold::foldText(rec.filename);
    rec.normPath        = Fold::foldText(rec.path);
    rec.titleSort.clear();
    rec.artistSort.clear();
    rec.albumArtistSort.clear();
    rec.albumSort.clear();
}

} // namespace Search
