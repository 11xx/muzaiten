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
    QByteArray fullMetadataBlob;     // zstd(JSON) of the complete tag set; empty if not captured
    qint64 fullMetadataRawSize = 0;  // uncompressed length of the archive JSON
    // Technical audio properties (promoted from the metadata blob for fast access / search indexing)
    int sampleRateHz = 0;
    int bitrateKbps = 0;
    int channels = 0;
    int bitDepth = 0;  // lossless sample bit depth; 0 = unknown / lossy
    QString codec;  // file extension lower-cased, e.g. "flac", "mp3", "opus"
};

Q_DECLARE_METATYPE(Track)
Q_DECLARE_METATYPE(QVector<Track>)
