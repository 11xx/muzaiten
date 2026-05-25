#include "db/Database.h"

#include "db/Schema.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

namespace {

QString sourceName(Rating::Source source)
{
    switch (source) {
    case Rating::Source::None:
        return QStringLiteral("none");
    case Rating::Source::MusicBeeCompatible:
        return QStringLiteral("musicbee-compatible");
    case Rating::Source::VorbisRating:
        return QStringLiteral("vorbis-rating");
    case Rating::Source::Id3Popularimeter:
        return QStringLiteral("id3-popm");
    case Rating::Source::Mp4Rate:
        return QStringLiteral("mp4-rate");
    case Rating::Source::Unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

bool execSql(QSqlQuery &query, const QString &sql, QString *error)
{
    if (!query.exec(sql)) {
        if (error != nullptr) {
            *error = query.lastError().text();
        }
        return false;
    }
    return true;
}

} // namespace

Database::Database(QString connectionName)
    : m_connectionName(std::move(connectionName))
{
}

Database::~Database()
{
    if (m_db.isValid()) {
        m_db.close();
    }
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool Database::open(const QString &path)
{
    QFileInfo info(path);
    QDir().mkpath(info.absolutePath());

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(path);
    if (!m_db.open()) {
        m_lastError = m_db.lastError().text();
        return false;
    }
    return migrate();
}

bool Database::migrate()
{
    QSqlQuery query(m_db);
    const QStringList statements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS scan_roots (id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, created_at TEXT NOT NULL, last_scanned_at TEXT)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS artists (id INTEGER PRIMARY KEY, name TEXT NOT NULL, sort_name TEXT, musicbrainz_artist_id TEXT, UNIQUE(name, musicbrainz_artist_id))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS albums (id INTEGER PRIMARY KEY, title TEXT NOT NULL, album_artist_id INTEGER, sort_title TEXT, date TEXT, original_date TEXT, musicbrainz_release_id TEXT, musicbrainz_release_group_id TEXT, artwork_cache_key TEXT, FOREIGN KEY(album_artist_id) REFERENCES artists(id))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS tracks (id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, parent_dir TEXT NOT NULL, filename TEXT NOT NULL, title TEXT, artist_name TEXT, album_artist_name TEXT, album_title TEXT, album_id INTEGER, track_number INTEGER, track_total INTEGER, disc_number INTEGER, disc_total INTEGER, duration_ms INTEGER, rating_0_100 INTEGER, rating_source TEXT NOT NULL DEFAULT 'none', play_count INTEGER, date TEXT, original_date TEXT, musicbrainz_recording_id TEXT, musicbrainz_track_id TEXT, musicbrainz_release_id TEXT, file_size INTEGER NOT NULL, file_mtime INTEGER NOT NULL, scanned_at TEXT NOT NULL, scan_error TEXT, FOREIGN KEY(album_id) REFERENCES albums(id))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS artwork (id INTEGER PRIMARY KEY, album_id INTEGER, source_type TEXT NOT NULL, source_path TEXT, cache_path TEXT, width INTEGER, height INTEGER, content_hash TEXT, updated_at TEXT NOT NULL, FOREIGN KEY(album_id) REFERENCES albums(id))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_artists_name ON artists(name)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_albums_album_artist ON albums(album_artist_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tracks_album ON tracks(album_id)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tracks_album_artist ON tracks(album_artist_name)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tracks_rating ON tracks(rating_0_100)"),
        QStringLiteral("INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(1, datetime('now'))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS user_track_ratings (track_path TEXT PRIMARY KEY, rating_0_100 INTEGER, updated_at TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS user_album_ratings (album_artist_name TEXT NOT NULL, album_title TEXT NOT NULL, rating_0_100 INTEGER, updated_at TEXT NOT NULL, PRIMARY KEY(album_artist_name, album_title))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS app_settings (key TEXT PRIMARY KEY, value TEXT NOT NULL, updated_at TEXT NOT NULL)"),
        QStringLiteral("INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(2, datetime('now'))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS media_sources (id INTEGER PRIMARY KEY AUTOINCREMENT, kind TEXT NOT NULL, name TEXT NOT NULL, root_hint TEXT, config_path TEXT, enabled INTEGER NOT NULL DEFAULT 1, created_at TEXT NOT NULL, updated_at TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS link_roots (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, source_prefix TEXT NOT NULL, target_prefix TEXT NOT NULL, priority INTEGER NOT NULL, readable INTEGER NOT NULL DEFAULT 1, writable INTEGER NOT NULL DEFAULT 0, enabled INTEGER NOT NULL DEFAULT 1, created_at TEXT NOT NULL, updated_at TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS mpd_tracks (id INTEGER PRIMARY KEY AUTOINCREMENT, source_id INTEGER NOT NULL, uri TEXT NOT NULL, title TEXT, artist_name TEXT, album_artist_name TEXT, album_title TEXT, track_number INTEGER, disc_number INTEGER, duration_ms INTEGER, date TEXT, musicbrainz_artist_id TEXT, musicbrainz_album_artist_id TEXT, musicbrainz_album_id TEXT, musicbrainz_recording_id TEXT, musicbrainz_release_track_id TEXT, last_seen_at TEXT NOT NULL, UNIQUE(source_id, uri))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS playlists (id INTEGER PRIMARY KEY AUTOINCREMENT, source_kind TEXT NOT NULL, source_id INTEGER, name TEXT NOT NULL, external_path TEXT, created_at TEXT NOT NULL, updated_at TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS playlist_tracks (playlist_id INTEGER NOT NULL, position INTEGER NOT NULL, source_kind TEXT NOT NULL, source_uri TEXT NOT NULL, track_path TEXT, title_snapshot TEXT, artist_snapshot TEXT, album_snapshot TEXT, duration_ms INTEGER, PRIMARY KEY(playlist_id, position))"),
        QStringLiteral("INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(3, datetime('now'))"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS pending_track_rating_writes (track_path TEXT PRIMARY KEY, rating_0_100 INTEGER NOT NULL, status TEXT NOT NULL, last_error TEXT, updated_at TEXT NOT NULL)"),
        QStringLiteral("INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(4, datetime('now'))"),
    };

    for (const QString &statement : statements) {
        if (!execSql(query, statement, &m_lastError)) {
            return false;
        }
    }
    return Schema::currentVersion == 4;
}

QString Database::lastError() const
{
    return m_lastError;
}

bool Database::beginTransaction()
{
    if (!m_db.transaction()) {
        m_lastError = m_db.lastError().text();
        return false;
    }
    return true;
}

bool Database::commitTransaction()
{
    if (!m_db.commit()) {
        m_lastError = m_db.lastError().text();
        return false;
    }
    return true;
}

qint64 Database::upsertArtist(const QString &name, const QString &sortName)
{
    const QString safeName = name.trimmed().isEmpty() ? QStringLiteral("[unknown]") : name.trimmed();
    QSqlQuery insert(m_db);
    insert.prepare(QStringLiteral("INSERT OR IGNORE INTO artists(name, sort_name, musicbrainz_artist_id) VALUES(?, ?, NULL)"));
    insert.addBindValue(safeName);
    insert.addBindValue(sortName);
    if (!insert.exec()) {
        m_lastError = insert.lastError().text();
        return 0;
    }

    QSqlQuery select(m_db);
    select.prepare(QStringLiteral("SELECT id FROM artists WHERE name = ?"));
    select.addBindValue(safeName);
    if (!select.exec() || !select.next()) {
        m_lastError = select.lastError().text();
        return 0;
    }
    return select.value(0).toLongLong();
}

qint64 Database::upsertAlbum(const Track &track, qint64 albumArtistId)
{
    const QString title = track.albumTitle.trimmed().isEmpty() ? QStringLiteral("[unknown album]") : track.albumTitle.trimmed();
    QSqlQuery insert(m_db);
    insert.prepare(QStringLiteral("INSERT OR IGNORE INTO albums(title, album_artist_id, sort_title, date, original_date, musicbrainz_release_id, musicbrainz_release_group_id, artwork_cache_key) VALUES(?, ?, NULL, ?, ?, ?, ?, NULL)"));
    insert.addBindValue(title);
    insert.addBindValue(albumArtistId);
    insert.addBindValue(track.date);
    insert.addBindValue(track.originalDate);
    insert.addBindValue(track.musicBrainz.releaseId);
    insert.addBindValue(track.musicBrainz.releaseGroupId);
    if (!insert.exec()) {
        m_lastError = insert.lastError().text();
        return 0;
    }

    QSqlQuery select(m_db);
    select.prepare(QStringLiteral("SELECT id FROM albums WHERE title = ? AND album_artist_id = ?"));
    select.addBindValue(title);
    select.addBindValue(albumArtistId);
    if (!select.exec() || !select.next()) {
        m_lastError = select.lastError().text();
        return 0;
    }
    return select.value(0).toLongLong();
}

bool Database::upsertTrack(const Track &track)
{
    const qint64 artistId = upsertArtist(track.albumArtistName.isEmpty() ? track.artistName : track.albumArtistName);
    if (artistId == 0) {
        return false;
    }
    const qint64 albumId = upsertAlbum(track, artistId);
    if (albumId == 0) {
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO tracks(path, parent_dir, filename, title, artist_name, album_artist_name, album_title, album_id, track_number, track_total, disc_number, disc_total, duration_ms, rating_0_100, rating_source, play_count, date, original_date, musicbrainz_recording_id, musicbrainz_track_id, musicbrainz_release_id, file_size, file_mtime, scanned_at, scan_error) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, datetime('now'), ?) "
        "ON CONFLICT(path) DO UPDATE SET parent_dir=excluded.parent_dir, filename=excluded.filename, title=excluded.title, artist_name=excluded.artist_name, album_artist_name=excluded.album_artist_name, album_title=excluded.album_title, album_id=excluded.album_id, track_number=excluded.track_number, track_total=excluded.track_total, disc_number=excluded.disc_number, disc_total=excluded.disc_total, duration_ms=excluded.duration_ms, rating_0_100=excluded.rating_0_100, rating_source=excluded.rating_source, play_count=excluded.play_count, date=excluded.date, original_date=excluded.original_date, musicbrainz_recording_id=excluded.musicbrainz_recording_id, musicbrainz_track_id=excluded.musicbrainz_track_id, musicbrainz_release_id=excluded.musicbrainz_release_id, file_size=excluded.file_size, file_mtime=excluded.file_mtime, scanned_at=datetime('now'), scan_error=excluded.scan_error"));
    query.addBindValue(track.path);
    query.addBindValue(track.parentDir);
    query.addBindValue(track.filename);
    query.addBindValue(track.title);
    query.addBindValue(track.artistName);
    query.addBindValue(track.albumArtistName);
    query.addBindValue(track.albumTitle);
    query.addBindValue(albumId);
    query.addBindValue(track.trackNumber);
    query.addBindValue(track.trackTotal);
    query.addBindValue(track.discNumber);
    query.addBindValue(track.discTotal);
    query.addBindValue(track.durationMs);
    query.addBindValue(track.rating0To100 >= 0 ? QVariant(track.rating0To100) : QVariant(QMetaType(QMetaType::Int)));
    query.addBindValue(sourceName(track.ratingSource));
    query.addBindValue(track.playCount);
    query.addBindValue(track.date);
    query.addBindValue(track.originalDate);
    query.addBindValue(track.musicBrainz.recordingId);
    query.addBindValue(track.musicBrainz.trackId);
    query.addBindValue(track.musicBrainz.releaseId);
    query.addBindValue(track.fileSize);
    query.addBindValue(track.fileMtime);
    query.addBindValue(track.scanError);

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

QVector<Artist> Database::albumArtists() const
{
    QVector<Artist> artists;
    QSqlQuery query(m_db);
    query.exec(QStringLiteral("SELECT album_artist_name, COUNT(DISTINCT album_title) FROM tracks GROUP BY album_artist_name ORDER BY lower(album_artist_name)"));
    while (query.next()) {
        Artist artist;
        artist.name = query.value(0).toString();
        artist.albumCount = query.value(1).toInt();
        artists.push_back(artist);
    }
    return artists;
}

QVector<Album> Database::albumsForArtist(const QString &albumArtist) const
{
    QVector<Album> albums;
    QSqlQuery query(m_db);
    const QString effectiveTrackRating = QStringLiteral(
        "COALESCE("
        "CASE WHEN p.status IN ('pending', 'failed', 'blocked_no_writable_path') THEN utr.rating_0_100 ELSE t.rating_0_100 END, "
        "utr.rating_0_100)");
    query.prepare(QStringLiteral(
        "SELECT t.album_title, MIN(t.date), COUNT(*), "
        "AVG(%1), "
        "COUNT(%1), "
        "MIN(t.parent_dir), uar.rating_0_100 "
        "FROM tracks t "
        "LEFT JOIN user_track_ratings utr ON utr.track_path = t.path "
        "LEFT JOIN pending_track_rating_writes p ON p.track_path = t.path "
        "LEFT JOIN user_album_ratings uar ON uar.album_artist_name = t.album_artist_name AND uar.album_title = t.album_title "
        "WHERE t.album_artist_name = ? "
        "GROUP BY t.album_title, uar.rating_0_100 "
        "ORDER BY lower(t.album_title)")
                      .arg(effectiveTrackRating));
    query.addBindValue(albumArtist);
    query.exec();
    while (query.next()) {
        Album album;
        album.title = query.value(0).toString();
        album.albumArtistName = albumArtist;
        album.date = query.value(1).toString();
        album.trackCount = query.value(2).toInt();
        album.averageRating0To100 = query.value(3).isNull() ? -1 : query.value(3).toInt();
        album.knownRatingCount = query.value(4).toInt();
        album.representativeDir = query.value(5).toString();
        album.hasUserRating = !query.value(6).isNull();
        album.effectiveRating0To100 = album.hasUserRating ? query.value(6).toInt() : album.averageRating0To100;
        albums.push_back(album);
    }
    return albums;
}

QVector<Track> Database::tracksForArtist(const QString &albumArtist, const QString &albumTitleFilter) const
{
    QVector<Track> tracks;
    QSqlQuery query(m_db);
    QString sql = QStringLiteral(
        "SELECT t.path, t.parent_dir, t.filename, t.title, t.artist_name, t.album_artist_name, t.album_title, "
        "t.track_number, t.disc_number, t.duration_ms, t.rating_0_100, utr.rating_0_100, t.date, t.original_date, t.file_size, p.status "
        "FROM tracks t "
        "LEFT JOIN user_track_ratings utr ON utr.track_path = t.path "
        "LEFT JOIN pending_track_rating_writes p ON p.track_path = t.path "
        "WHERE t.album_artist_name = ?");
    if (!albumTitleFilter.isEmpty()) {
        sql += QStringLiteral(" AND t.album_title = ?");
    }
    sql += QStringLiteral(" ORDER BY lower(t.album_title), t.disc_number, t.track_number, lower(t.title)");
    query.prepare(sql);
    query.addBindValue(albumArtist);
    if (!albumTitleFilter.isEmpty()) {
        query.addBindValue(albumTitleFilter);
    }
    query.exec();
    while (query.next()) {
        Track track;
        track.path = query.value(0).toString();
        track.parentDir = query.value(1).toString();
        track.filename = query.value(2).toString();
        track.title = query.value(3).toString();
        track.artistName = query.value(4).toString();
        track.albumArtistName = query.value(5).toString();
        track.albumTitle = query.value(6).toString();
        track.trackNumber = query.value(7).toInt();
        track.discNumber = query.value(8).toInt();
        track.durationMs = query.value(9).toLongLong();
        track.rating0To100 = query.value(10).isNull() ? Rating::unset : query.value(10).toInt();
        track.hasUserRating = !query.value(11).isNull();
        const QString pendingStatus = query.value(15).toString();
        const bool pendingDbRating = pendingStatus == QStringLiteral("pending")
            || pendingStatus == QStringLiteral("failed")
            || pendingStatus == QStringLiteral("blocked_no_writable_path");
        track.effectiveRating0To100 = pendingDbRating && track.hasUserRating
            ? query.value(11).toInt()
            : (track.rating0To100 >= 0 ? track.rating0To100 : (track.hasUserRating ? query.value(11).toInt() : Rating::unset));
        track.date = query.value(12).toString();
        track.originalDate = query.value(13).toString();
        track.fileSize = query.value(14).toLongLong();
        tracks.push_back(track);
    }
    return tracks;
}

bool Database::setUserTrackRating(const QString &trackPath, int rating0To100)
{
    const int normalized = Rating::normalized0To100(rating0To100);
    if (!Rating::isValidStoredValue(normalized) || normalized < 0) {
        m_lastError = QStringLiteral("Invalid rating");
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO user_track_ratings(track_path, rating_0_100, updated_at) VALUES(?, ?, datetime('now')) "
        "ON CONFLICT(track_path) DO UPDATE SET rating_0_100=excluded.rating_0_100, updated_at=datetime('now')"));
    query.addBindValue(trackPath);
    query.addBindValue(normalized);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool Database::clearUserTrackRating(const QString &trackPath)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM user_track_ratings WHERE track_path = ?"));
    query.addBindValue(trackPath);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool Database::setPendingTrackRatingWrite(const QString &trackPath, int rating0To100, const QString &status, const QString &lastError)
{
    static const QStringList allowed = {
        QStringLiteral("pending"),
        QStringLiteral("synced"),
        QStringLiteral("blocked_existing_tag"),
        QStringLiteral("blocked_no_writable_path"),
        QStringLiteral("failed"),
    };
    const int normalized = Rating::normalized0To100(rating0To100);
    if (!Rating::isValidStoredValue(normalized) || normalized < 0 || !allowed.contains(status)) {
        m_lastError = QStringLiteral("Invalid pending rating write");
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO pending_track_rating_writes(track_path, rating_0_100, status, last_error, updated_at) VALUES(?, ?, ?, ?, datetime('now')) "
        "ON CONFLICT(track_path) DO UPDATE SET rating_0_100=excluded.rating_0_100, status=excluded.status, last_error=excluded.last_error, updated_at=datetime('now')"));
    query.addBindValue(trackPath);
    query.addBindValue(normalized);
    query.addBindValue(status);
    query.addBindValue(lastError);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool Database::clearPendingTrackRatingWrite(const QString &trackPath)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM pending_track_rating_writes WHERE track_path = ?"));
    query.addBindValue(trackPath);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

QVector<Track> Database::tracksWithUserRatings() const
{
    QVector<Track> tracks;
    QSqlQuery query(m_db);
    query.exec(QStringLiteral(
        "SELECT t.path, t.parent_dir, t.filename, t.title, t.artist_name, t.album_artist_name, t.album_title, "
        "t.track_number, t.disc_number, t.duration_ms, t.rating_0_100, utr.rating_0_100, t.date, t.original_date, t.file_size, t.file_mtime, p.status "
        "FROM user_track_ratings utr JOIN tracks t ON t.path = utr.track_path "
        "LEFT JOIN pending_track_rating_writes p ON p.track_path = t.path "
        "ORDER BY lower(t.album_artist_name), lower(t.album_title), t.disc_number, t.track_number, lower(t.title)"));
    while (query.next()) {
        Track track;
        track.path = query.value(0).toString();
        track.parentDir = query.value(1).toString();
        track.filename = query.value(2).toString();
        track.title = query.value(3).toString();
        track.artistName = query.value(4).toString();
        track.albumArtistName = query.value(5).toString();
        track.albumTitle = query.value(6).toString();
        track.trackNumber = query.value(7).toInt();
        track.discNumber = query.value(8).toInt();
        track.durationMs = query.value(9).toLongLong();
        track.rating0To100 = query.value(10).isNull() ? Rating::unset : query.value(10).toInt();
        track.hasUserRating = true;
        const QString pendingStatus = query.value(16).toString();
        const bool pendingDbRating = pendingStatus == QStringLiteral("pending")
            || pendingStatus == QStringLiteral("failed")
            || pendingStatus == QStringLiteral("blocked_no_writable_path");
        track.effectiveRating0To100 = pendingDbRating ? query.value(11).toInt() : (track.rating0To100 >= 0 ? track.rating0To100 : query.value(11).toInt());
        track.date = query.value(12).toString();
        track.originalDate = query.value(13).toString();
        track.fileSize = query.value(14).toLongLong();
        track.fileMtime = query.value(15).toLongLong();
        tracks.push_back(track);
    }
    return tracks;
}

QVector<Track> Database::tracksWithPendingRatingWrites() const
{
    QVector<Track> tracks;
    QSqlQuery query(m_db);
    query.exec(QStringLiteral(
        "SELECT t.path, t.parent_dir, t.filename, t.title, t.artist_name, t.album_artist_name, t.album_title, "
        "t.track_number, t.disc_number, t.duration_ms, t.rating_0_100, p.rating_0_100, t.date, t.original_date, t.file_size, t.file_mtime "
        "FROM pending_track_rating_writes p JOIN tracks t ON t.path = p.track_path "
        "WHERE p.status IN ('pending', 'failed', 'blocked_no_writable_path') "
        "ORDER BY p.updated_at ASC"));
    while (query.next()) {
        Track track;
        track.path = query.value(0).toString();
        track.parentDir = query.value(1).toString();
        track.filename = query.value(2).toString();
        track.title = query.value(3).toString();
        track.artistName = query.value(4).toString();
        track.albumArtistName = query.value(5).toString();
        track.albumTitle = query.value(6).toString();
        track.trackNumber = query.value(7).toInt();
        track.discNumber = query.value(8).toInt();
        track.durationMs = query.value(9).toLongLong();
        track.rating0To100 = query.value(10).isNull() ? Rating::unset : query.value(10).toInt();
        track.hasUserRating = true;
        track.effectiveRating0To100 = query.value(11).toInt();
        track.date = query.value(12).toString();
        track.originalDate = query.value(13).toString();
        track.fileSize = query.value(14).toLongLong();
        track.fileMtime = query.value(15).toLongLong();
        tracks.push_back(track);
    }
    return tracks;
}

bool Database::updateScannedTrackRating(const QString &trackPath, int rating0To100, Rating::Source source, qint64 fileSize, qint64 fileMtime)
{
    const int normalized = Rating::normalized0To100(rating0To100);
    if (!Rating::isValidStoredValue(normalized) || normalized < 0) {
        m_lastError = QStringLiteral("Invalid scanned rating");
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE tracks SET rating_0_100 = ?, rating_source = ?, file_size = ?, file_mtime = ?, scanned_at = datetime('now') WHERE path = ?"));
    query.addBindValue(normalized);
    query.addBindValue(sourceName(source));
    query.addBindValue(fileSize);
    query.addBindValue(fileMtime);
    query.addBindValue(trackPath);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return query.numRowsAffected() > 0;
}

bool Database::setUserAlbumRating(const QString &albumArtistName, const QString &albumTitle, int rating0To100)
{
    const int normalized = Rating::normalized0To100(rating0To100);
    if (!Rating::isValidStoredValue(normalized) || normalized < 0) {
        m_lastError = QStringLiteral("Invalid rating");
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO user_album_ratings(album_artist_name, album_title, rating_0_100, updated_at) VALUES(?, ?, ?, datetime('now')) "
        "ON CONFLICT(album_artist_name, album_title) DO UPDATE SET rating_0_100=excluded.rating_0_100, updated_at=datetime('now')"));
    query.addBindValue(albumArtistName);
    query.addBindValue(albumTitle);
    query.addBindValue(normalized);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool Database::clearUserAlbumRating(const QString &albumArtistName, const QString &albumTitle)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM user_album_ratings WHERE album_artist_name = ? AND album_title = ?"));
    query.addBindValue(albumArtistName);
    query.addBindValue(albumTitle);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

QString Database::setting(const QString &key, const QString &fallback) const
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT value FROM app_settings WHERE key = ?"));
    query.addBindValue(key);
    if (!query.exec() || !query.next()) {
        return fallback;
    }
    return query.value(0).toString();
}

bool Database::setSetting(const QString &key, const QString &value)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO app_settings(key, value, updated_at) VALUES(?, ?, datetime('now')) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=datetime('now')"));
    query.addBindValue(key);
    query.addBindValue(value);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

QVector<LinkRoot> Database::linkRoots() const
{
    QVector<LinkRoot> roots;
    QSqlQuery query(m_db);
    query.exec(QStringLiteral(
        "SELECT id, name, source_prefix, target_prefix, priority, readable, writable, enabled "
        "FROM link_roots "
        "ORDER BY priority DESC, id ASC"));
    while (query.next()) {
        LinkRoot root;
        root.id = query.value(0).toInt();
        root.name = query.value(1).toString();
        root.sourcePrefix = query.value(2).toString();
        root.targetPrefix = query.value(3).toString();
        root.priority = query.value(4).toInt();
        root.readable = query.value(5).toBool();
        root.writable = query.value(6).toBool();
        root.enabled = query.value(7).toBool();
        roots.push_back(root);
    }
    return roots;
}

bool Database::saveLinkRoot(const LinkRoot &linkRoot)
{
    if (linkRoot.sourcePrefix.trimmed().isEmpty() || linkRoot.targetPrefix.trimmed().isEmpty()) {
        m_lastError = QStringLiteral("Link root source and target prefixes are required");
        return false;
    }

    QSqlQuery query(m_db);
    if (linkRoot.id > 0) {
        query.prepare(QStringLiteral(
            "UPDATE link_roots "
            "SET name = ?, source_prefix = ?, target_prefix = ?, priority = ?, readable = ?, writable = ?, enabled = ?, updated_at = datetime('now') "
            "WHERE id = ?"));
        query.addBindValue(linkRoot.name);
        query.addBindValue(linkRoot.sourcePrefix);
        query.addBindValue(linkRoot.targetPrefix);
        query.addBindValue(linkRoot.priority);
        query.addBindValue(linkRoot.readable ? 1 : 0);
        query.addBindValue(linkRoot.writable ? 1 : 0);
        query.addBindValue(linkRoot.enabled ? 1 : 0);
        query.addBindValue(linkRoot.id);
    } else {
        query.prepare(QStringLiteral(
            "INSERT INTO link_roots(name, source_prefix, target_prefix, priority, readable, writable, enabled, created_at, updated_at) "
            "VALUES(?, ?, ?, ?, ?, ?, ?, datetime('now'), datetime('now'))"));
        query.addBindValue(linkRoot.name);
        query.addBindValue(linkRoot.sourcePrefix);
        query.addBindValue(linkRoot.targetPrefix);
        query.addBindValue(linkRoot.priority);
        query.addBindValue(linkRoot.readable ? 1 : 0);
        query.addBindValue(linkRoot.writable ? 1 : 0);
        query.addBindValue(linkRoot.enabled ? 1 : 0);
    }

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool Database::removeLinkRoot(int id)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM link_roots WHERE id = ?"));
    query.addBindValue(id);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

qint64 Database::upsertMediaSource(const QString &kind, const QString &name, const QString &rootHint, const QString &configPath)
{
    QSqlQuery select(m_db);
    select.prepare(QStringLiteral("SELECT id FROM media_sources WHERE kind = ? AND name = ?"));
    select.addBindValue(kind);
    select.addBindValue(name);
    if (!select.exec()) {
        m_lastError = select.lastError().text();
        return 0;
    }

    if (select.next()) {
        const qint64 id = select.value(0).toLongLong();
        QSqlQuery update(m_db);
        update.prepare(QStringLiteral(
            "UPDATE media_sources "
            "SET root_hint = ?, config_path = ?, enabled = 1, updated_at = datetime('now') "
            "WHERE id = ?"));
        update.addBindValue(rootHint);
        update.addBindValue(configPath);
        update.addBindValue(id);
        if (!update.exec()) {
            m_lastError = update.lastError().text();
            return 0;
        }
        return id;
    }

    QSqlQuery insert(m_db);
    insert.prepare(QStringLiteral(
        "INSERT INTO media_sources(kind, name, root_hint, config_path, enabled, created_at, updated_at) "
        "VALUES(?, ?, ?, ?, 1, datetime('now'), datetime('now'))"));
    insert.addBindValue(kind);
    insert.addBindValue(name);
    insert.addBindValue(rootHint);
    insert.addBindValue(configPath);
    if (!insert.exec()) {
        m_lastError = insert.lastError().text();
        return 0;
    }
    return insert.lastInsertId().toLongLong();
}

bool Database::clearMpdTracksForSource(qint64 sourceId)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM mpd_tracks WHERE source_id = ?"));
    query.addBindValue(sourceId);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool Database::upsertMpdTrack(qint64 sourceId, const MpdTrack &track)
{
    if (sourceId <= 0 || track.uri.isEmpty()) {
        m_lastError = QStringLiteral("MPD source id and URI are required");
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO mpd_tracks(source_id, uri, title, artist_name, album_artist_name, album_title, track_number, disc_number, duration_ms, date, musicbrainz_artist_id, musicbrainz_album_artist_id, musicbrainz_album_id, musicbrainz_recording_id, musicbrainz_release_track_id, last_seen_at) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, datetime('now')) "
        "ON CONFLICT(source_id, uri) DO UPDATE SET title=excluded.title, artist_name=excluded.artist_name, album_artist_name=excluded.album_artist_name, album_title=excluded.album_title, track_number=excluded.track_number, disc_number=excluded.disc_number, duration_ms=excluded.duration_ms, date=excluded.date, musicbrainz_artist_id=excluded.musicbrainz_artist_id, musicbrainz_album_artist_id=excluded.musicbrainz_album_artist_id, musicbrainz_album_id=excluded.musicbrainz_album_id, musicbrainz_recording_id=excluded.musicbrainz_recording_id, musicbrainz_release_track_id=excluded.musicbrainz_release_track_id, last_seen_at=datetime('now')"));
    query.addBindValue(sourceId);
    query.addBindValue(track.uri);
    query.addBindValue(track.title);
    query.addBindValue(track.artistName);
    query.addBindValue(track.albumArtistName);
    query.addBindValue(track.albumTitle);
    query.addBindValue(track.trackNumber);
    query.addBindValue(track.discNumber);
    query.addBindValue(track.durationMs);
    query.addBindValue(track.date);
    query.addBindValue(track.musicBrainz.artistId);
    query.addBindValue(track.musicBrainz.albumArtistId);
    query.addBindValue(track.musicBrainz.releaseId);
    query.addBindValue(track.musicBrainz.recordingId);
    query.addBindValue(track.musicBrainz.trackId);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

int Database::mpdTrackCount(qint64 sourceId) const
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM mpd_tracks WHERE source_id = ?"));
    query.addBindValue(sourceId);
    if (!query.exec() || !query.next()) {
        return 0;
    }
    return query.value(0).toInt();
}

qint64 Database::mpdSourceId() const
{
    QSqlQuery query(m_db);
    query.exec(QStringLiteral("SELECT id FROM media_sources WHERE kind = 'mpd' ORDER BY id ASC LIMIT 1"));
    if (!query.next()) {
        return 0;
    }
    return query.value(0).toLongLong();
}

QVector<Artist> Database::mpdAlbumArtists() const
{
    QVector<Artist> artists;
    QSqlQuery query(m_db);
    query.exec(QStringLiteral(
        "SELECT album_artist_name, COUNT(DISTINCT album_title) "
        "FROM mpd_tracks "
        "WHERE album_artist_name IS NOT NULL AND album_artist_name != '' "
        "GROUP BY album_artist_name "
        "ORDER BY lower(album_artist_name)"));
    while (query.next()) {
        Artist artist;
        artist.name = query.value(0).toString();
        artist.albumCount = query.value(1).toInt();
        artists.push_back(artist);
    }
    return artists;
}

QVector<Album> Database::mpdAlbumsForArtist(const QString &albumArtist, const QString &musicDirectory) const
{
    QVector<Album> albums;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT album_title, MIN(date), COUNT(*), MIN(uri) "
        "FROM mpd_tracks "
        "WHERE album_artist_name = ? "
        "GROUP BY album_title "
        "ORDER BY lower(album_title)"));
    query.addBindValue(albumArtist);
    query.exec();
    while (query.next()) {
        Album album;
        album.title = query.value(0).toString();
        album.albumArtistName = albumArtist;
        album.date = query.value(1).toString();
        album.trackCount = query.value(2).toInt();
        album.averageRating0To100 = Rating::unset;
        album.effectiveRating0To100 = Rating::unset;
        const QString repUri = query.value(3).toString();
        if (!repUri.isEmpty() && !musicDirectory.trimmed().isEmpty()) {
            const QString resolvedPath = QDir::isAbsolutePath(repUri)
                ? repUri
                : QDir::cleanPath(QDir(musicDirectory).filePath(repUri));
            album.representativeDir = QFileInfo(resolvedPath).absolutePath();
        }
        albums.push_back(album);
    }
    return albums;
}

QVector<Track> Database::mpdTracksForArtist(const QString &albumArtist, const QString &musicDirectory, const QString &albumTitleFilter) const
{
    QVector<Track> tracks;
    QSqlQuery query(m_db);
    QString sql = QStringLiteral(
        "SELECT uri, title, artist_name, album_artist_name, album_title, "
        "track_number, disc_number, duration_ms, date "
        "FROM mpd_tracks "
        "WHERE album_artist_name = ?");
    if (!albumTitleFilter.isEmpty()) {
        sql += QStringLiteral(" AND album_title = ?");
    }
    sql += QStringLiteral(" ORDER BY lower(album_title), disc_number, track_number, lower(title)");
    query.prepare(sql);
    query.addBindValue(albumArtist);
    if (!albumTitleFilter.isEmpty()) {
        query.addBindValue(albumTitleFilter);
    }
    query.exec();
    while (query.next()) {
        Track track;
        const QString uri = query.value(0).toString();
        if (QDir::isAbsolutePath(uri)) {
            track.path = uri;
        } else {
            track.path = QDir::cleanPath(QDir(musicDirectory).filePath(uri));
        }
        track.parentDir = QFileInfo(track.path).absolutePath();
        track.filename = QFileInfo(track.path).fileName();
        track.title = query.value(1).toString();
        track.artistName = query.value(2).toString();
        track.albumArtistName = query.value(3).toString();
        track.albumTitle = query.value(4).toString();
        track.trackNumber = query.value(5).toInt();
        track.discNumber = query.value(6).toInt();
        track.durationMs = query.value(7).toLongLong();
        track.date = query.value(8).toString();
        track.rating0To100 = Rating::unset;
        track.effectiveRating0To100 = Rating::unset;
        tracks.push_back(track);
    }
    return tracks;
}
