#include "core/MusicSort.h"

#include "core/Album.h"
#include "core/Track.h"

#include <QCollator>

namespace MusicSort {

namespace {

// Parse the best available year (originalDate preferred, then date) to an int.
// Unknown years return 0 so they cluster together at one end.
int yearOf(const QString &originalDate, const QString &date)
{
    for (const QString &candidate : {originalDate, date}) {
        const QString y = candidate.trimmed().left(4);
        if (!y.isEmpty()) {
            bool ok = false;
            const int year = y.toInt(&ok);
            if (ok && year > 0) {
                return year;
            }
        }
    }
    return 0;
}

int cmpInt(qint64 a, qint64 b)
{
    return a < b ? -1 : (a > b ? 1 : 0);
}

int cmpStr(const QString &a, const QString &b)
{
    // Natural ordering: embedded numbers compare by value, so "2" sorts before
    // "11" instead of character-by-character ("11" before "2"). Case-insensitive.
    // thread_local, not static: QCollator::compare() mutates the underlying ICU
    // collator state, so a single shared instance is not safe for the concurrent
    // sorts that run on UI and worker threads.
    thread_local const QCollator collator = [] {
        QCollator c;
        c.setNumericMode(true);
        c.setCaseSensitivity(Qt::CaseInsensitive);
        return c;
    }();
    return collator.compare(a, b);
}

} // namespace

int compareField(SortField field, const Track &a, const Track &b)
{
    switch (field) {
    case SortField::Year:
        return cmpInt(yearOf(a.originalDate, a.date), yearOf(b.originalDate, b.date));
    case SortField::Title:
        return cmpStr(a.title, b.title);
    case SortField::AlbumTitle:
        return cmpStr(a.albumTitle, b.albumTitle);
    case SortField::AlbumArtist:
        return cmpStr(a.albumArtistName, b.albumArtistName);
    case SortField::Artist:
        return cmpStr(a.artistName, b.artistName);
    case SortField::DiscNumber:
        return cmpInt(a.discNumber, b.discNumber);
    case SortField::TrackNumber:
        return cmpInt(a.trackNumber, b.trackNumber);
    case SortField::Rating:
        return cmpInt(a.effectiveRating0To100, b.effectiveRating0To100);
    case SortField::Duration:
        return cmpInt(a.durationMs, b.durationMs);
    case SortField::DateAdded:
        return cmpInt(a.fileMtime, b.fileMtime);
    case SortField::FileName:
        return cmpStr(a.filename, b.filename);
    case SortField::FileSize:
        return cmpInt(a.fileSize, b.fileSize);
    case SortField::TrackCount:
        return 0; // not applicable to a single track
    }
    return 0;
}

int compareField(SortField field, const Album &a, const Album &b)
{
    switch (field) {
    case SortField::Year:
        return cmpInt(yearOf(a.originalDate, a.date), yearOf(b.originalDate, b.date));
    case SortField::Title:
    case SortField::AlbumTitle:
        return cmpStr(a.title, b.title);
    case SortField::AlbumArtist:
    case SortField::Artist:
        return cmpStr(a.albumArtistName, b.albumArtistName);
    case SortField::Rating:
        return cmpInt(a.effectiveRating0To100, b.effectiveRating0To100);
    case SortField::TrackCount:
        return cmpInt(a.trackCount, b.trackCount);
    case SortField::DateAdded:
        return cmpInt(a.addedMtime, b.addedMtime);
    case SortField::DiscNumber:
    case SortField::TrackNumber:
    case SortField::Duration:
    case SortField::FileName:
    case SortField::FileSize:
        return 0; // not applicable at album granularity
    }
    return 0;
}

const std::vector<SortField> &tiebreakChainFor(SortField primary)
{
    using S = SortField;
    static const std::vector<SortField> kYear        = {S::AlbumArtist, S::AlbumTitle, S::DiscNumber, S::TrackNumber, S::Title};
    static const std::vector<SortField> kRating      = {S::Year, S::AlbumArtist, S::AlbumTitle, S::DiscNumber, S::TrackNumber, S::Title};
    static const std::vector<SortField> kTrackNumber = {S::Year, S::AlbumTitle, S::DiscNumber, S::Title};
    static const std::vector<SortField> kAlbumTitle  = {S::Year, S::DiscNumber, S::TrackNumber, S::Title};
    static const std::vector<SortField> kArtist      = {S::Year, S::AlbumTitle, S::DiscNumber, S::TrackNumber, S::Title};
    static const std::vector<SortField> kTitle       = {S::Year, S::AlbumTitle, S::TrackNumber};
    static const std::vector<SortField> kGeneric     = {S::Year, S::AlbumTitle, S::Title};

    switch (primary) {
    case S::Year:                          return kYear;
    case S::Rating:                        return kRating;
    case S::TrackNumber:                   return kTrackNumber;
    case S::AlbumTitle:                    return kAlbumTitle;
    case S::Artist:
    case S::AlbumArtist:                   return kArtist;
    case S::Title:                         return kTitle;
    case S::Duration:
    case S::TrackCount:
    case S::DateAdded:
    case S::FileName:
    case S::FileSize:                      return kGeneric;
    case S::DiscNumber:                    return kAlbumTitle;
    }
    return kGeneric;
}

QString sortFieldToString(SortField field)
{
    switch (field) {
    case SortField::Year:        return QStringLiteral("year");
    case SortField::Title:       return QStringLiteral("title");
    case SortField::AlbumTitle:  return QStringLiteral("album");
    case SortField::AlbumArtist: return QStringLiteral("albumArtist");
    case SortField::Artist:      return QStringLiteral("artist");
    case SortField::DiscNumber:  return QStringLiteral("disc");
    case SortField::TrackNumber: return QStringLiteral("track");
    case SortField::Rating:      return QStringLiteral("rating");
    case SortField::Duration:    return QStringLiteral("duration");
    case SortField::TrackCount:  return QStringLiteral("trackCount");
    case SortField::DateAdded:   return QStringLiteral("dateAdded");
    case SortField::FileName:    return QStringLiteral("name");
    case SortField::FileSize:    return QStringLiteral("size");
    }
    return QStringLiteral("year");
}

SortField sortFieldFromString(const QString &value, SortField fallback)
{
    if (value == QStringLiteral("year"))        return SortField::Year;
    if (value == QStringLiteral("title"))       return SortField::Title;
    if (value == QStringLiteral("album"))       return SortField::AlbumTitle;
    if (value == QStringLiteral("albumArtist")) return SortField::AlbumArtist;
    if (value == QStringLiteral("artist"))      return SortField::Artist;
    if (value == QStringLiteral("disc"))        return SortField::DiscNumber;
    if (value == QStringLiteral("track"))       return SortField::TrackNumber;
    if (value == QStringLiteral("rating"))      return SortField::Rating;
    if (value == QStringLiteral("duration"))    return SortField::Duration;
    if (value == QStringLiteral("trackCount"))  return SortField::TrackCount;
    if (value == QStringLiteral("dateAdded"))   return SortField::DateAdded;
    if (value == QStringLiteral("name"))        return SortField::FileName;
    if (value == QStringLiteral("size"))        return SortField::FileSize;
    return fallback;
}

} // namespace MusicSort
