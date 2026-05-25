#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QVector>

#include "core/Album.h"
#include "core/Artist.h"
#include "core/ScanRoot.h"
#include "core/Track.h"
#include "fs/LinkRoot.h"
#include "mpd/MpdTrack.h"

class Database final {
public:
    explicit Database(QString connectionName);
    ~Database();

    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    bool open(const QString &path);
    bool migrate();
    QString lastError() const;

    bool beginTransaction();
    bool commitTransaction();
    bool upsertTrack(const Track &track);
    bool setUserTrackRating(const QString &trackPath, int rating0To100);
    bool clearUserTrackRating(const QString &trackPath);
    bool setPendingTrackRatingWrite(const QString &trackPath, int rating0To100, const QString &status, const QString &lastError = {});
    bool clearPendingTrackRatingWrite(const QString &trackPath);
    QVector<Track> tracksWithUserRatings() const;
    QVector<Track> tracksWithPendingRatingWrites() const;
    bool updateScannedTrackRating(const QString &trackPath, int rating0To100, Rating::Source source, qint64 fileSize, qint64 fileMtime);
    bool setUserAlbumRating(const QString &albumArtistName, const QString &albumTitle, int rating0To100);
    bool clearUserAlbumRating(const QString &albumArtistName, const QString &albumTitle);
    QString setting(const QString &key, const QString &fallback = {}) const;
    bool setSetting(const QString &key, const QString &value);
    QVector<LinkRoot> linkRoots() const;
    bool saveLinkRoot(const LinkRoot &linkRoot);
    bool removeLinkRoot(int id);
    QVector<ScanRoot> scanRoots() const;
    QVector<ScanRoot> enabledScanRoots() const;
    QVector<ScanRoot> enabledLibraryRoots() const;
    bool saveScanRoot(const ScanRoot &root);
    bool removeScanRoot(int id);
    bool setScanRootLastScanned(int id, const QString &lastError = {});
    QVector<Track> tracksForDirectory(const QString &directory) const;
    QStringList localLibraryDirectories(const QString &parentDirectory = {}) const;
    qint64 upsertMediaSource(const QString &kind, const QString &name, const QString &rootHint, const QString &configPath);
    bool clearMpdTracksForSource(qint64 sourceId);
    bool upsertMpdTrack(qint64 sourceId, const MpdTrack &track);
    int mpdTrackCount(qint64 sourceId) const;
    qint64 mpdSourceId() const;
    QVector<Artist> mpdAlbumArtists() const;
    QVector<Album> mpdAlbumsForArtist(const QString &albumArtist, const QString &musicDirectory) const;
    QVector<Track> mpdTracksForArtist(const QString &albumArtist, const QString &musicDirectory, const QString &albumTitleFilter = {}) const;
    QVector<Artist> albumArtists() const;
    QVector<Album> albumsForArtist(const QString &albumArtist) const;
    QVector<Track> tracksForArtist(const QString &albumArtist, const QString &albumTitleFilter = {}) const;

private:
    qint64 upsertArtist(const QString &name, const QString &sortName = {});
    qint64 upsertAlbum(const Track &track, qint64 albumArtistId);

    QString m_connectionName;
    QSqlDatabase m_db;
    QString m_lastError;
};
