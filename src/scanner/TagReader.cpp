#include "scanner/TagReader.h"

#include "core/Rating.h"
#include "scanner/TagRating.h"

#include <QFileInfo>
#include <QStringList>

#include <taglib/audioproperties.h>
#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>
#include <taglib/tstring.h>
#include <taglib/tstringlist.h>

// Per-format property subclasses that expose bitsPerSample(). The base
// AudioProperties does not, so bit depth requires a downcast (see readBitsPerSample).
#include <taglib/aiffproperties.h>
#include <taglib/apeproperties.h>
#include <taglib/dsdiffproperties.h>
#include <taglib/dsfproperties.h>
#include <taglib/flacproperties.h>
#include <taglib/mp4properties.h>
#include <taglib/trueaudioproperties.h>
#include <taglib/wavpackproperties.h>
#include <taglib/wavproperties.h>

namespace {

QString toQString(const TagLib::String &value)
{
    return QString::fromStdString(value.to8Bit(true));
}

QString firstProperty(const TagLib::PropertyMap &properties, const QStringList &keys)
{
    for (const QString &key : keys) {
        const TagLib::String tagKey(key.toStdString(), TagLib::String::UTF8);
        const auto values = properties[tagKey];
        if (!values.isEmpty()) {
            const QString value = toQString(values.front()).trimmed();
            if (!value.isEmpty()) {
                return value;
            }
        }
    }
    return {};
}

int firstIntProperty(const TagLib::PropertyMap &properties, const QStringList &keys)
{
    const QString value = firstProperty(properties, keys);
    bool ok = false;
    const int parsed = value.toInt(&ok);
    return ok ? parsed : 0;
}

int firstTrackPart(const TagLib::PropertyMap &properties, const QStringList &keys)
{
    const QString value = firstProperty(properties, keys);
    const QString first = value.section(QLatin1Char('/'), 0, 0);
    bool ok = false;
    const int parsed = first.toInt(&ok);
    return ok ? parsed : 0;
}

// Bit depth lives on per-format AudioProperties subclasses, not the base class.
// Probe the lossless formats that expose bitsPerSample(); lossy formats
// (MP3/Vorbis/Opus/Speex/Musepack) have no fixed sample depth and return 0.
int readBitsPerSample(const TagLib::AudioProperties *audio)
{
    if (audio == nullptr) {
        return 0;
    }
    if (const auto *p = dynamic_cast<const TagLib::FLAC::Properties *>(audio)) {
        return p->bitsPerSample();
    }
    if (const auto *p = dynamic_cast<const TagLib::RIFF::WAV::Properties *>(audio)) {
        return p->bitsPerSample();
    }
    if (const auto *p = dynamic_cast<const TagLib::RIFF::AIFF::Properties *>(audio)) {
        return p->bitsPerSample();
    }
    if (const auto *p = dynamic_cast<const TagLib::APE::Properties *>(audio)) {
        return p->bitsPerSample();
    }
    if (const auto *p = dynamic_cast<const TagLib::WavPack::Properties *>(audio)) {
        return p->bitsPerSample();
    }
    if (const auto *p = dynamic_cast<const TagLib::MP4::Properties *>(audio)) {
        return p->bitsPerSample();
    }
    if (const auto *p = dynamic_cast<const TagLib::DSF::Properties *>(audio)) {
        return p->bitsPerSample();
    }
    if (const auto *p = dynamic_cast<const TagLib::DSDIFF::Properties *>(audio)) {
        return p->bitsPerSample();
    }
    if (const auto *p = dynamic_cast<const TagLib::TrueAudio::Properties *>(audio)) {
        return p->bitsPerSample();
    }
    return 0;
}

} // namespace

Track TagReader::read(const QString &path, MetadataBlob::FullMetadata *fullMetadata) const
{
    QFileInfo info(path);

    Track track;
    track.path = info.absoluteFilePath();
    track.parentDir = info.absolutePath();
    track.filename = info.fileName();
    track.fileSize = info.size();
    track.fileMtime = info.lastModified().toSecsSinceEpoch();

    TagLib::FileRef file(path.toUtf8().constData(), true);
    if (file.isNull()) {
        track.scanError = QStringLiteral("TagLib could not open file");
        return track;
    }

    if (const TagLib::Tag *tag = file.tag()) {
        track.title = toQString(tag->title());
        track.artistName = toQString(tag->artist());
        track.albumTitle = toQString(tag->album());
        track.date = QString::number(tag->year());
        track.trackNumber = static_cast<int>(tag->track());
    }

    if (const TagLib::AudioProperties *audio = file.audioProperties()) {
        const int bitDepth = readBitsPerSample(audio);
        track.durationMs = static_cast<qint64>(audio->lengthInMilliseconds());
        track.bitrateKbps = audio->bitrate();
        track.sampleRateHz = audio->sampleRate();
        track.channels = audio->channels();
        track.bitDepth = bitDepth;
        if (fullMetadata != nullptr) {
            fullMetadata->bitrateKbps = audio->bitrate();
            fullMetadata->sampleRateHz = audio->sampleRate();
            fullMetadata->channels = audio->channels();
            fullMetadata->bitDepth = bitDepth;
        }
    }

    const TagLib::PropertyMap properties = file.file()->properties();
    track.codec = info.suffix().toLower();
    if (fullMetadata != nullptr) {
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            QStringList values;
            values.reserve(static_cast<int>(it->second.size()));
            for (const auto &value : it->second) {
                values.append(toQString(value));
            }
            fullMetadata->tags.insert(toQString(it->first), values);
        }
        fullMetadata->codec = track.codec;
    }
    track.albumArtistName = firstProperty(properties, {
        QStringLiteral("ALBUMARTIST"),
        QStringLiteral("ALBUM ARTIST"),
        QStringLiteral("ALBUM_ARTIST"),
    });
    if (track.albumArtistName.isEmpty()) {
        track.albumArtistName = track.artistName;
    }

    const TagRatingReadResult rating = readRating(properties);
    if (rating.rating0To100 >= 0) {
        track.rating0To100 = rating.rating0To100;
        track.ratingSource = rating.source;
    }

    track.trackNumber = track.trackNumber == 0 ? firstTrackPart(properties, {QStringLiteral("TRACKNUMBER"), QStringLiteral("TRACK")}) : track.trackNumber;
    track.trackTotal = firstIntProperty(properties, {QStringLiteral("TRACKTOTAL"), QStringLiteral("TOTALTRACKS")});
    track.discNumber = firstTrackPart(properties, {QStringLiteral("DISCNUMBER"), QStringLiteral("DISC")});
    track.discTotal = firstIntProperty(properties, {QStringLiteral("DISCTOTAL"), QStringLiteral("TOTALDISCS")});
    track.originalDate = firstProperty(properties, {QStringLiteral("ORIGINALDATE"), QStringLiteral("ORIGINALYEAR")});

    track.musicBrainz.artistId = firstProperty(properties, {QStringLiteral("MUSICBRAINZ_ARTISTID")});
    track.musicBrainz.albumArtistId = firstProperty(properties, {QStringLiteral("MUSICBRAINZ_ALBUMARTISTID")});
    track.musicBrainz.releaseId = firstProperty(properties, {QStringLiteral("MUSICBRAINZ_ALBUMID"), QStringLiteral("MUSICBRAINZ_RELEASEID")});
    track.musicBrainz.releaseGroupId = firstProperty(properties, {QStringLiteral("MUSICBRAINZ_RELEASEGROUPID")});
    track.musicBrainz.recordingId = firstProperty(properties, {QStringLiteral("MUSICBRAINZ_TRACKID"), QStringLiteral("MUSICBRAINZ_RECORDINGID")});
    track.musicBrainz.trackId = firstProperty(properties, {QStringLiteral("MUSICBRAINZ_RELEASETRACKID")});
    track.musicBrainz.workId = firstProperty(properties, {QStringLiteral("MUSICBRAINZ_WORKID")});

    if (track.title.isEmpty()) {
        track.title = info.completeBaseName();
    }
    if (track.albumTitle.isEmpty()) {
        track.albumTitle = QStringLiteral("[unknown album]");
    }
    if (track.artistName.isEmpty()) {
        track.artistName = QStringLiteral("[unknown artist]");
    }
    if (track.albumArtistName.isEmpty()) {
        track.albumArtistName = track.artistName;
    }

    return track;
}
