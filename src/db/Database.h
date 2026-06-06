#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QVector>

#include <QHash>
#include <QPair>
#include <QSet>
#include <QStringList>

#include "core/Album.h"
#include "core/Artist.h"
#include "core/MetadataBlob.h"
#include "core/ScanRoot.h"
#include "core/Track.h"
#include "core/TrackFingerprint.h"
#include "fs/LinkRoot.h"
#include "mpd/MpdTrack.h"
#include "search/SearchRecord.h"

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
    // Brackets a scan/ingest run so upsertTrack can memoize artist/album ids
    // (append-only during a scan) instead of re-querying them per track. The
    // caches are cleared on begin and end; outside a session no caching happens.
    void beginScanSession();
    void endScanSession();
    bool upsertTrack(const Track &track);
    // Fast first pass: insert path-derived placeholder rows (metadata_scanned=0)
    // for newly-enumerated files, without touching artists/albums. ON CONFLICT DO
    // NOTHING so it never clobbers an already-scanned row or resets its flag.
    bool insertEnumeratedPlaceholders(const QVector<Track> &tracks);
    // path -> {file_mtime, file_size, metadata_scanned} for tracks under
    // rootPrefix, for the incremental-rescan diff. Empty rootPrefix returns all
    // tracks. metadata_scanned lets the diff re-queue enumerated-only placeholders.
    QHash<QString, TrackFingerprint> trackFingerprints(const QString &rootPrefix = {}) const;
    // Paths of enumerated-only placeholder rows (metadata_scanned=0) awaiting a tag
    // read, optionally scoped to one directory and/or capped, for the lazy fill.
    QStringList enumeratedOnlyPaths(const QString &parentDir = {}, int limit = 0) const;
    // Flags the given track paths as missing (file no longer present).
    int markTracksMissing(const QStringList &paths);
    int removeMissingTracks();
    int missingTrackCount() const;
    MetadataBlob::FullMetadata fullMetadata(const QString &path) const;
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
    bool removeSetting(const QString &key);
    QVector<LinkRoot> linkRoots() const;
    bool saveLinkRoot(const LinkRoot &linkRoot);
    bool removeLinkRoot(int id);
    QVector<ScanRoot> scanRoots() const;
    QVector<ScanRoot> enabledScanRoots() const;
    QVector<ScanRoot> enabledLibraryRoots() const;
    bool saveScanRoot(const ScanRoot &root);
    bool removeScanRoot(int id);
    bool setScanRootLastScanned(int id, const QString &lastError = {});
    Track trackForPath(const QString &path) const;
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
    // Narrow to several albums in a single query (album-sort order). An empty
    // list returns the whole artist; empty entries are ignored.
    QVector<Track> mpdTracksForArtist(const QString &albumArtist, const QString &musicDirectory, const QStringList &albumTitleFilters) const;
    QVector<Artist> albumArtists() const;
    QVector<Album> albumsForArtist(const QString &albumArtist) const;
    QVector<Track> tracksForArtist(const QString &albumArtist, const QString &albumTitleFilter = {}) const;
    QVector<Track> tracksForArtist(const QString &albumArtist, const QStringList &albumTitleFilters) const;
    // Lightweight row set for building the search index — all non-missing local tracks.
    QVector<Search::SearchRecord> allTracksForSearch() const;
    // Lightweight row set for MPD-imported tracks.
    QVector<Search::SearchRecord> allMpdTracksForSearch() const;

private:
    qint64 upsertArtist(const QString &name, const QString &sortName = {});
    qint64 upsertAlbum(const Track &track, qint64 albumArtistId);

    QString m_connectionName;
    QSqlDatabase m_db;
    QString m_lastError;

    // Per-scan-session id memoization (see beginScanSession). Only consulted
    // while m_scanSession is true; artists/albums are append-only during a scan
    // so a resolved id stays valid until the session ends.
    bool m_scanSession = false;
    QHash<QString, qint64> m_artistIdCache;
    QHash<QString, qint64> m_albumIdCache;
};
