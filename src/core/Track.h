#pragma once

#include "core/MusicBrainzIds.h"
#include "core/Rating.h"

#include <QByteArray>
#include <QString>
#include <QtTypes>
#include <QMetaType>
#include <QVector>

struct Track {
    QString path;
    QString parentDir;
    QString filename;
    QString title;
    QString artistName;
    QString albumArtistName;
    QString albumTitle;
    // Sort/reading names (Picard titlesort/artistsort/albumartistsort/albumsort).
    // For non-Latin titles these often carry a romaji/kana reading; folded into
    // the search index to improve recall (e.g. "utada" → 宇多田ヒカル).
    QString titleSort;
    QString artistSort;
    QString albumArtistSort;
    QString albumSort;
    QString date;
    QString originalDate;
    int trackNumber = 0;
    int trackTotal = 0;
    int discNumber = 0;
    int discTotal = 0;
    qint64 durationMs = 0;
    int rating0To100 = Rating::unset;
    int effectiveRating0To100 = Rating::unset;
    Rating::Source ratingSource = Rating::Source::None;
    bool hasUserRating = false;
    int playCount = 0;
    MusicBrainzIds musicBrainz;
    qint64 fileSize = 0;
    qint64 fileMtime = 0;
    QString scanError;
    bool missing = false;
    QByteArray fullMetadataBlob;     // zstd(JSON) of the complete tag set; empty if not captured
    qint64 fullMetadataRawSize = 0;  // uncompressed length of the archive JSON
    // Technical audio properties (promoted from the metadata blob for fast access / search indexing)
    int sampleRateHz = 0;
    int bitrateKbps = 0;
    int channels = 0;
    int bitDepth = 0;  // lossless sample bit depth; 0 = unknown / lossy
    QString codec;  // file extension lower-cased, e.g. "flac", "mp3", "opus"
};

// DSD (Direct Stream Digital) source formats: the .dsf and .dsdiff (.dff)
// containers. DSD needs a dedicated playback path — native bit-perfect to the
// DAC, or live DSD→PCM decode — distinct from ordinary PCM codecs, so callers
// branch on this. Track::codec is already the lower-cased extension; the
// toLower() guard just keeps the helper safe for ad-hoc callers.
inline bool isDsdCodec(const QString &codec)
{
    const QString c = codec.toLower();
    return c == QStringLiteral("dsf") || c == QStringLiteral("dff");
}

inline bool isDsdTrack(const Track &track)
{
    if (isDsdCodec(track.codec)) {
        return true;
    }
    // codec is unset for tracks built straight from a path (enqueue-by-path, the
    // file explorer, snapshots written before codec was persisted). Fall back to
    // the file extension — exactly what the scanner stores — so DSD detection
    // never silently fails and downgrades the track to PCM.
    if (track.codec.isEmpty()) {
        const qsizetype dot = track.path.lastIndexOf(QLatin1Char('.'));
        if (dot >= 0) {
            return isDsdCodec(track.path.mid(dot + 1));
        }
    }
    return false;
}

Q_DECLARE_METATYPE(Track)
Q_DECLARE_METATYPE(QVector<Track>)
