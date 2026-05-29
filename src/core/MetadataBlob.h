#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QtTypes>

// Complete, format-agnostic metadata captured from an audio file: every text
// tag (including Picard/MusicBrainz and custom tags) plus the technical audio
// properties. Serialized to JSON and zstd-compressed for per-track archival
// storage. Images are intentionally excluded.
namespace MetadataBlob {

struct FullMetadata {
    QMap<QString, QStringList> tags;
    int bitrateKbps = 0;
    int sampleRateHz = 0;
    int channels = 0;
    QString codec;
};

struct Encoded {
    QByteArray data;     // zstd frame of the compact JSON
    qint64 rawSize = 0;  // uncompressed JSON length
};

bool isEmpty(const FullMetadata &metadata);

// Compact JSON of the metadata, zstd-compressed (level 3).
Encoded encode(const FullMetadata &metadata);

// Inverse of encode(). rawSize is a hint/fallback; the zstd frame carries its
// own content size and is preferred when available.
FullMetadata decode(const QByteArray &blob, qint64 rawSize = 0);

} // namespace MetadataBlob
