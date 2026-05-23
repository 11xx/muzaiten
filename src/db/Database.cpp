#include "db/Database.h"

#include "db/Schema.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
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
    };

    for (const QString &statement : statements) {
        if (!execSql(query, statement, &m_lastError)) {
            return false;
        }
    }
    return Schema::currentVersion == 1;
}

QString Database::lastError() const
{
    return m_lastError;
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
    query.prepare(QStringLiteral("SELECT album_title, MIN(date), COUNT(*), AVG(rating_0_100), COUNT(rating_0_100), MIN(parent_dir) FROM tracks WHERE album_artist_name = ? GROUP BY album_title ORDER BY lower(album_title)"));
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
        albums.push_back(album);
    }
    return albums;
}

QVector<Track> Database::tracksForArtist(const QString &albumArtist) const
{
    QVector<Track> tracks;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT path, parent_dir, filename, title, artist_name, album_artist_name, album_title, track_number, disc_number, duration_ms, rating_0_100, date FROM tracks WHERE album_artist_name = ? ORDER BY lower(album_title), disc_number, track_number, lower(title)"));
    query.addBindValue(albumArtist);
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
        track.date = query.value(11).toString();
        tracks.push_back(track);
    }
    return tracks;
}
