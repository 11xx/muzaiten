#include "scanner/TagReader.h"

#include "core/Rating.h"

#include <QFileInfo>
#include <QStringList>

#include <cmath>

#include <taglib/audioproperties.h>
#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>
#include <taglib/tstring.h>
#include <taglib/tstringlist.h>

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

int ratingProperty0To100(const TagLib::PropertyMap &properties, Rating::Source *source)
{
    const QString rating = firstProperty(properties, {QStringLiteral("RATING")});
    if (!rating.isEmpty()) {
        bool ok = false;
        const int parsed = rating.toInt(&ok);
        if (ok) {
            if (source != nullptr) {
                *source = Rating::Source::MusicBeeCompatible;
            }
            return Rating::normalized0To100(parsed);
        }
    }

    const QString fmps = firstProperty(properties, {QStringLiteral("FMPS_RATING")});
    if (!fmps.isEmpty()) {
        bool ok = false;
        const double parsed = fmps.toDouble(&ok);
        if (ok) {
            if (source != nullptr) {
                *source = Rating::Source::VorbisRating;
            }
            return Rating::normalized0To100(static_cast<int>(std::lround(parsed * 100.0)));
        }
    }

    return Rating::unset;
}

int firstTrackPart(const TagLib::PropertyMap &properties, const QStringList &keys)
{
    const QString value = firstProperty(properties, keys);
    const QString first = value.section(QLatin1Char('/'), 0, 0);
    bool ok = false;
    const int parsed = first.toInt(&ok);
    return ok ? parsed : 0;
}

} // namespace

Track TagReader::read(const QString &path) const
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
        track.durationMs = static_cast<qint64>(audio->lengthInMilliseconds());
    }

    const TagLib::PropertyMap properties = file.file()->properties();
    track.albumArtistName = firstProperty(properties, {
        QStringLiteral("ALBUMARTIST"),
        QStringLiteral("ALBUM ARTIST"),
        QStringLiteral("ALBUM_ARTIST"),
    });
    if (track.albumArtistName.isEmpty()) {
        track.albumArtistName = track.artistName;
    }

    Rating::Source ratingSource = Rating::Source::None;
    const int rating = ratingProperty0To100(properties, &ratingSource);
    if (rating >= 0) {
        track.rating0To100 = rating;
        track.ratingSource = ratingSource;
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
