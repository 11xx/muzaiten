#include "db/Database.h"

#include "db/Schema.h"
#include "search/SearchRecord.h"
#include "search/fold/Fold.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

#include <algorithm>

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

bool tableHasColumn(QSqlDatabase database, const QString &table, const QString &column)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
        return false;
    }
    while (query.next()) {
        if (query.value(1).toString() == column) {
            return true;
        }
    }
    return false;
}

bool ensureColumn(QSqlDatabase database, const QString &table, const QString &column, const QString &definition, QString *error)
{
    if (tableHasColumn(database, table, column)) {
        return true;
    }
    QSqlQuery query(database);
    return execSql(query, QStringLiteral("ALTER TABLE %1 ADD COLUMN %2").arg(table, definition), error);
}

QString cleanRootPath(const QString &path)
{
    const QString cleaned = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (cleaned.size() > 1 && cleaned.endsWith(QLatin1Char('/'))) {
        return cleaned.left(cleaned.size() - 1);
    }
    return cleaned;
}

bool hasScanRoots(QSqlDatabase database)
{
    QSqlQuery query(database);
    return query.exec(QStringLiteral("SELECT 1 FROM scan_roots LIMIT 1")) && query.next();
}

QString sqlQuote(const QString &value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

// Predicate restricting tracks to the enabled library roots. The roots are
// resolved once by the caller and inlined as an explicit OR chain, instead of a
// correlated EXISTS subquery that re-scanned scan_roots for every track row (the
// dominant per-row cost when building the search index over a large library).
// Empty roots -> "0" (no track matches), matching the old EXISTS semantics when
// scan_roots exist but none are library-enabled. Paths are SQL- and LIKE-escaped
// (mirroring trackFingerprints) so arbitrary path characters are handled safely.
QString enabledLibraryRootPredicate(const QString &trackAlias, const QVector<ScanRoot> &roots)
{
    if (roots.isEmpty()) {
        return QStringLiteral("0");
    }
    QStringList clauses;
    clauses.reserve(roots.size());
    for (const ScanRoot &root : roots) {
        QString likePrefix = root.path;
        likePrefix.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        likePrefix.replace(QLatin1Char('%'), QStringLiteral("\\%"));
        likePrefix.replace(QLatin1Char('_'), QStringLiteral("\\_"));
        likePrefix += QStringLiteral("/%");
        clauses << QStringLiteral("%1.path = %2 OR %1.path LIKE %3 ESCAPE '\\'")
                       .arg(trackAlias, sqlQuote(root.path), sqlQuote(likePrefix));
    }
    return QStringLiteral("(") + clauses.join(QStringLiteral(" OR ")) + QStringLiteral(")");
}

// Browse-visibility predicate: fully-scanned rows always, plus path-guessed
// placeholders (metadata_scanned=0 with a guessed album_artist_name) when the
// "show guessed metadata" setting is on. Blank placeholders (NULL artist) and the
// off state both reduce to the plain metadata_scanned=1 check.
QString visibleTrackPredicate(const QString &alias, bool showGuessed)
{
    if (showGuessed) {
        return QStringLiteral("(%1.metadata_scanned = 1 OR %1.album_artist_name IS NOT NULL)").arg(alias);
    }
    return QStringLiteral("%1.metadata_scanned = 1").arg(alias);
}

// "?, ?, ..." for an IN (...) clause with `count` bound parameters.
QString sqlPlaceholders(qsizetype count)
{
    QStringList marks;
    marks.reserve(count);
    for (qsizetype i = 0; i < count; ++i) {
        marks << QStringLiteral("?");
    }
    return marks.join(QStringLiteral(", "));
}

// Collapse a repeated string (album/artist/codec/date and their lowercased
// forms) to a single shared COW buffer, so the in-memory search index holds one
// allocation per distinct value instead of one per track.  QString is implicitly
// shared, so callers and the matcher read it exactly as before.
QString internString(QHash<QString, QString> &pool, const QString &value)
{
    if (value.isEmpty()) {
        return value;
    }
    const auto it = pool.constFind(value);
    if (it != pool.constEnd()) {
        return it.value();
    }
    pool.insert(value, value);
    return value;
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

    QSqlQuery pragma(m_db);
    pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    pragma.exec(QStringLiteral("PRAGMA busy_timeout=5000"));
    // Burst-friendly tuning: keep temp B-trees/sorts in RAM, memory-map the DB
    // for fewer read syscalls on large-library scans/queries, and give the page
    // cache ~64 MB (negative = KiB). All are per-connection and safe with WAL.
    pragma.exec(QStringLiteral("PRAGMA temp_store=MEMORY"));
    pragma.exec(QStringLiteral("PRAGMA mmap_size=268435456"));
    pragma.exec(QStringLiteral("PRAGMA cache_size=-65536"));

    return migrate();
}

bool Database::migrate()
{
    QSqlQuery query(m_db);
    const QStringList statements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS scan_roots (id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE, name TEXT, scan_enabled INTEGER NOT NULL DEFAULT 1, library_enabled INTEGER NOT NULL DEFAULT 1, created_at TEXT NOT NULL, updated_at TEXT, last_scanned_at TEXT, last_error TEXT)"),
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

    const QVector<QPair<QString, QString>> scanRootColumns = {
        {QStringLiteral("name"), QStringLiteral("name TEXT")},
        {QStringLiteral("scan_enabled"), QStringLiteral("scan_enabled INTEGER NOT NULL DEFAULT 1")},
        {QStringLiteral("library_enabled"), QStringLiteral("library_enabled INTEGER NOT NULL DEFAULT 1")},
        {QStringLiteral("updated_at"), QStringLiteral("updated_at TEXT")},
        {QStringLiteral("last_error"), QStringLiteral("last_error TEXT")},
    };
    for (const auto &column : scanRootColumns) {
        if (!ensureColumn(m_db, QStringLiteral("scan_roots"), column.first, column.second, &m_lastError)) {
            return false;
        }
    }

    const QStringList sourceDirectoryStatements = {
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_scan_roots_scan_enabled ON scan_roots(scan_enabled)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_scan_roots_library_enabled ON scan_roots(library_enabled)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tracks_path ON tracks(path)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tracks_parent_dir ON tracks(parent_dir)"),
        QStringLiteral("INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(5, datetime('now'))"),
    };
    for (const QString &statement : sourceDirectoryStatements) {
        if (!execSql(query, statement, &m_lastError)) {
            return false;
        }
    }

    const QVector<QPair<QString, QString>> trackColumns = {
        {QStringLiteral("missing"), QStringLiteral("missing INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("missing_since"), QStringLiteral("missing_since TEXT")},
    };
    for (const auto &column : trackColumns) {
        if (!ensureColumn(m_db, QStringLiteral("tracks"), column.first, column.second, &m_lastError)) {
            return false;
        }
    }

    const QStringList fullMetadataStatements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS track_metadata (track_id INTEGER PRIMARY KEY, format INTEGER NOT NULL DEFAULT 1, raw_size INTEGER NOT NULL, data BLOB NOT NULL, FOREIGN KEY(track_id) REFERENCES tracks(id) ON DELETE CASCADE)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tracks_missing ON tracks(missing)"),
        QStringLiteral("INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(6, datetime('now'))"),
    };
    for (const QString &statement : fullMetadataStatements) {
        if (!execSql(query, statement, &m_lastError)) {
            return false;
        }
    }

    // v7: promote audio technical properties to tracks table for fast search indexing
    const QVector<QPair<QString, QString>> techColumns = {
        {QStringLiteral("sample_rate_hz"), QStringLiteral("sample_rate_hz INTEGER")},
        {QStringLiteral("bitrate_kbps"),   QStringLiteral("bitrate_kbps INTEGER")},
        {QStringLiteral("channels"),       QStringLiteral("channels INTEGER")},
        {QStringLiteral("codec"),          QStringLiteral("codec TEXT")},
    };
    for (const auto &column : techColumns) {
        if (!ensureColumn(m_db, QStringLiteral("tracks"), column.first, column.second, &m_lastError)) {
            return false;
        }
    }

    // Backfill from stored metadata blobs for rows that have a blob but NULL tech columns
    {
        QSqlQuery bfQuery(m_db);
        bfQuery.prepare(QStringLiteral(
            "SELECT t.path, t.id, m.raw_size, m.data "
            "FROM tracks t JOIN track_metadata m ON m.track_id = t.id "
            "WHERE t.sample_rate_hz IS NULL AND t.bitrate_kbps IS NULL"));
        if (bfQuery.exec()) {
            QSqlQuery upd(m_db);
            upd.prepare(QStringLiteral(
                "UPDATE tracks SET sample_rate_hz=?, bitrate_kbps=?, channels=?, codec=? WHERE id=?"));
            while (bfQuery.next()) {
                const qint64 trackId = bfQuery.value(1).toLongLong();
                const qint64 rawSize = bfQuery.value(2).toLongLong();
                const QByteArray blob = bfQuery.value(3).toByteArray();
                const MetadataBlob::FullMetadata meta = MetadataBlob::decode(blob, rawSize);
                if (meta.sampleRateHz > 0 || meta.bitrateKbps > 0 || !meta.codec.isEmpty()) {
                    upd.addBindValue(meta.sampleRateHz > 0 ? QVariant(meta.sampleRateHz) : QVariant());
                    upd.addBindValue(meta.bitrateKbps > 0 ? QVariant(meta.bitrateKbps) : QVariant());
                    upd.addBindValue(meta.channels > 0 ? QVariant(meta.channels) : QVariant());
                    upd.addBindValue(meta.codec.isEmpty() ? QVariant() : QVariant(meta.codec));
                    upd.addBindValue(trackId);
                    upd.exec();
                }
            }
        }
    }

    if (!execSql(query, QStringLiteral("INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(7, datetime('now'))"), &m_lastError)) {
        return false;
    }

    // v8: fast-first-pass placeholders + bit depth.
    //  - metadata_scanned distinguishes enumerated-only placeholder rows (0) from
    //    fully tag-read rows (1). Existing rows default to 1 so they are unaffected;
    //    only the new fast-pass insert writes 0. Browse/search queries filter
    //    metadata_scanned=1; the directory/file view intentionally does not.
    //  - bit_depth holds the lossless sample bit depth (lossy formats stay NULL).
    const QVector<QPair<QString, QString>> v8Columns = {
        {QStringLiteral("metadata_scanned"), QStringLiteral("metadata_scanned INTEGER NOT NULL DEFAULT 1")},
        {QStringLiteral("bit_depth"),        QStringLiteral("bit_depth INTEGER")},
    };
    for (const auto &column : v8Columns) {
        if (!ensureColumn(m_db, QStringLiteral("tracks"), column.first, column.second, &m_lastError)) {
            return false;
        }
    }
    const QStringList v8Statements = {
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tracks_metadata_scanned ON tracks(metadata_scanned)"),
        QStringLiteral("INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(8, datetime('now'))"),
    };
    for (const QString &statement : v8Statements) {
        if (!execSql(query, statement, &m_lastError)) {
            return false;
        }
    }

    return Schema::currentVersion == 8;
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

void Database::beginScanSession()
{
    m_scanSession = true;
    m_artistIdCache.clear();
    m_albumIdCache.clear();
}

void Database::endScanSession()
{
    m_scanSession = false;
    m_artistIdCache.clear();
    m_albumIdCache.clear();
}

qint64 Database::upsertArtist(const QString &name, const QString &sortName)
{
    const QString safeName = name.trimmed().isEmpty() ? QStringLiteral("[unknown]") : name.trimmed();
    if (m_scanSession) {
        const auto cached = m_artistIdCache.constFind(safeName);
        if (cached != m_artistIdCache.constEnd()) {
            return cached.value();
        }
    }
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
    const qint64 id = select.value(0).toLongLong();
    if (m_scanSession) {
        m_artistIdCache.insert(safeName, id);
    }
    return id;
}

qint64 Database::upsertAlbum(const Track &track, qint64 albumArtistId)
{
    const QString title = track.albumTitle.trimmed().isEmpty() ? QStringLiteral("[unknown album]") : track.albumTitle.trimmed();
    QString cacheKey;
    if (m_scanSession) {
        cacheKey = QString::number(albumArtistId) + QLatin1Char('\x1f') + title;
        const auto cached = m_albumIdCache.constFind(cacheKey);
        if (cached != m_albumIdCache.constEnd()) {
            return cached.value();
        }
    }
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
    const qint64 id = select.value(0).toLongLong();
    if (m_scanSession) {
        m_albumIdCache.insert(cacheKey, id);
    }
    return id;
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
        "INSERT INTO tracks(path, parent_dir, filename, title, artist_name, album_artist_name, album_title, album_id, track_number, track_total, disc_number, disc_total, duration_ms, rating_0_100, rating_source, play_count, date, original_date, musicbrainz_recording_id, musicbrainz_track_id, musicbrainz_release_id, file_size, file_mtime, scanned_at, scan_error, sample_rate_hz, bitrate_kbps, channels, codec, bit_depth) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, datetime('now'), ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(path) DO UPDATE SET parent_dir=excluded.parent_dir, filename=excluded.filename, title=excluded.title, artist_name=excluded.artist_name, album_artist_name=excluded.album_artist_name, album_title=excluded.album_title, album_id=excluded.album_id, track_number=excluded.track_number, track_total=excluded.track_total, disc_number=excluded.disc_number, disc_total=excluded.disc_total, duration_ms=excluded.duration_ms, rating_0_100=excluded.rating_0_100, rating_source=excluded.rating_source, play_count=excluded.play_count, date=excluded.date, original_date=excluded.original_date, musicbrainz_recording_id=excluded.musicbrainz_recording_id, musicbrainz_track_id=excluded.musicbrainz_track_id, musicbrainz_release_id=excluded.musicbrainz_release_id, file_size=excluded.file_size, file_mtime=excluded.file_mtime, scanned_at=datetime('now'), scan_error=excluded.scan_error, missing=0, missing_since=NULL, sample_rate_hz=excluded.sample_rate_hz, bitrate_kbps=excluded.bitrate_kbps, channels=excluded.channels, codec=excluded.codec, bit_depth=excluded.bit_depth, "
        "metadata_scanned=1 "
        "RETURNING id"));
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
    query.addBindValue(track.sampleRateHz > 0 ? QVariant(track.sampleRateHz) : QVariant());
    query.addBindValue(track.bitrateKbps > 0 ? QVariant(track.bitrateKbps) : QVariant());
    query.addBindValue(track.channels > 0 ? QVariant(track.channels) : QVariant());
    query.addBindValue(track.codec.isEmpty() ? QVariant() : QVariant(track.codec));
    query.addBindValue(track.bitDepth > 0 ? QVariant(track.bitDepth) : QVariant());

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }

    if (!track.fullMetadataBlob.isEmpty()) {
        // The upsert's RETURNING id gives the row id directly (insert or update),
        // avoiding a separate SELECT per track. Fall back to a lookup only if the
        // driver didn't surface the returned row.
        qint64 trackId = 0;
        if (query.next()) {
            trackId = query.value(0).toLongLong();
        }
        if (trackId <= 0) {
            QSqlQuery idQuery(m_db);
            idQuery.prepare(QStringLiteral("SELECT id FROM tracks WHERE path = ?"));
            idQuery.addBindValue(track.path);
            if (idQuery.exec() && idQuery.next()) {
                trackId = idQuery.value(0).toLongLong();
            }
        }
        if (trackId > 0) {
            QSqlQuery metaQuery(m_db);
            metaQuery.prepare(QStringLiteral(
                "INSERT INTO track_metadata(track_id, format, raw_size, data) VALUES(?, 1, ?, ?) "
                "ON CONFLICT(track_id) DO UPDATE SET format=1, raw_size=excluded.raw_size, data=excluded.data"));
            metaQuery.addBindValue(trackId);
            metaQuery.addBindValue(track.fullMetadataRawSize);
            metaQuery.addBindValue(track.fullMetadataBlob);
            if (!metaQuery.exec()) {
                m_lastError = metaQuery.lastError().text();
                return false;
            }
        }
    }
    return true;
}

bool Database::insertEnumeratedPlaceholders(const QVector<Track> &tracks)
{
    if (tracks.isEmpty()) {
        return true;
    }
    if (!m_db.transaction()) {
        m_lastError = m_db.lastError().text();
        return false;
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO tracks(path, parent_dir, filename, title, artist_name, album_artist_name, album_title, "
        "track_number, file_size, file_mtime, scanned_at, metadata_scanned) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, datetime('now'), 0) "
        "ON CONFLICT(path) DO NOTHING"));
    for (const Track &track : tracks) {
        query.addBindValue(track.path);
        query.addBindValue(track.parentDir);
        query.addBindValue(track.filename);
        query.addBindValue(track.title);
        // Guessed fields are empty unless the path guesser populated them; store
        // NULL so blank placeholders stay isolated from the artist/album browse.
        query.addBindValue(track.artistName.isEmpty() ? QVariant() : QVariant(track.artistName));
        query.addBindValue(track.albumArtistName.isEmpty() ? QVariant() : QVariant(track.albumArtistName));
        query.addBindValue(track.albumTitle.isEmpty() ? QVariant() : QVariant(track.albumTitle));
        query.addBindValue(track.trackNumber > 0 ? QVariant(track.trackNumber) : QVariant());
        query.addBindValue(track.fileSize);
        query.addBindValue(track.fileMtime);
        if (!query.exec()) {
            m_lastError = query.lastError().text();
            m_db.rollback();
            return false;
        }
    }
    if (!m_db.commit()) {
        m_lastError = m_db.lastError().text();
        return false;
    }
    return true;
}

QStringList Database::enumeratedOnlyPaths(const QString &parentDir, int limit) const
{
    QStringList paths;
    QString sql = QStringLiteral("SELECT t.path FROM tracks t WHERE t.metadata_scanned = 0 AND t.missing = 0");
    if (!parentDir.isEmpty()) {
        sql += QStringLiteral(" AND t.parent_dir = ?");
    }
    if (hasScanRoots(m_db)) {
        sql += QStringLiteral(" AND %1").arg(enabledLibraryRootPredicate(QStringLiteral("t"), enabledLibraryRoots()));
    }
    if (limit > 0) {
        sql += QStringLiteral(" LIMIT %1").arg(limit);
    }
    QSqlQuery query(m_db);
    query.prepare(sql);
    if (!parentDir.isEmpty()) {
        query.addBindValue(parentDir);
    }
    if (query.exec()) {
        while (query.next()) {
            paths.append(query.value(0).toString());
        }
    }
    return paths;
}

int Database::enumeratedOnlyCount() const
{
    QString sql = QStringLiteral("SELECT COUNT(*) FROM tracks t WHERE t.metadata_scanned = 0 AND t.missing = 0");
    if (hasScanRoots(m_db)) {
        sql += QStringLiteral(" AND %1").arg(enabledLibraryRootPredicate(QStringLiteral("t"), enabledLibraryRoots()));
    }
    QSqlQuery query(m_db);
    if (query.exec(sql) && query.next()) {
        return query.value(0).toInt();
    }
    return 0;
}

QHash<QString, TrackFingerprint> Database::trackFingerprints(const QString &rootPrefix) const
{
    QHash<QString, TrackFingerprint> fingerprints;
    QSqlQuery query(m_db);
    if (rootPrefix.isEmpty()) {
        query.prepare(QStringLiteral("SELECT path, file_mtime, file_size, metadata_scanned FROM tracks"));
    } else {
        query.prepare(QStringLiteral("SELECT path, file_mtime, file_size, metadata_scanned FROM tracks WHERE path = ? OR path LIKE ? ESCAPE '\\'"));
        query.addBindValue(rootPrefix);
        QString likePrefix = rootPrefix;
        likePrefix.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        likePrefix.replace(QLatin1Char('%'), QStringLiteral("\\%"));
        likePrefix.replace(QLatin1Char('_'), QStringLiteral("\\_"));
        query.addBindValue(likePrefix + QStringLiteral("/%"));
    }
    if (!query.exec()) {
        return fingerprints;
    }
    while (query.next()) {
        TrackFingerprint fingerprint;
        fingerprint.mtime = query.value(1).toLongLong();
        fingerprint.size = query.value(2).toLongLong();
        fingerprint.metadataScanned = query.value(3).toInt() != 0;
        fingerprints.insert(query.value(0).toString(), fingerprint);
    }
    return fingerprints;
}

int Database::markTracksMissing(const QStringList &paths)
{
    if (paths.isEmpty()) {
        return 0;
    }
    // One UPDATE ... WHERE path IN (...) per chunk inside a single transaction,
    // instead of a separate exec() per path. Chunked to stay well under SQLite's
    // bound-parameter limit.
    constexpr int kChunk = 500;
    const bool ownTransaction = m_db.transaction();
    int marked = 0;
    for (qsizetype start = 0; start < paths.size(); start += kChunk) {
        const qsizetype count = std::min<qsizetype>(kChunk, paths.size() - start);
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral(
            "UPDATE tracks SET missing=1, missing_since=datetime('now') WHERE missing = 0 AND path IN (%1)")
            .arg(sqlPlaceholders(count)));
        for (qsizetype i = 0; i < count; ++i) {
            query.addBindValue(paths.at(start + i));
        }
        if (query.exec()) {
            marked += query.numRowsAffected();
        }
    }
    if (ownTransaction) {
        m_db.commit();
    }
    return marked;
}

int Database::removeMissingTracks()
{
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("DELETE FROM tracks WHERE missing = 1"))) {
        m_lastError = query.lastError().text();
        return 0;
    }
    return query.numRowsAffected();
}

int Database::missingTrackCount() const
{
    QSqlQuery query(m_db);
    if (query.exec(QStringLiteral("SELECT COUNT(*) FROM tracks WHERE missing = 1")) && query.next()) {
        return query.value(0).toInt();
    }
    return 0;
}


MetadataBlob::FullMetadata Database::fullMetadata(const QString &path) const
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT m.raw_size, m.data FROM track_metadata m JOIN tracks t ON t.id = m.track_id WHERE t.path = ?"));
    query.addBindValue(path);
    if (query.exec() && query.next()) {
        const qint64 rawSize = query.value(0).toLongLong();
        const QByteArray blob = query.value(1).toByteArray();
        return MetadataBlob::decode(blob, rawSize);
    }
    return {};
}

QVector<Artist> Database::albumArtists() const
{
    QVector<Artist> artists;
    QSqlQuery query(m_db);
    QString sql = QStringLiteral("SELECT album_artist_name, COUNT(DISTINCT album_title) FROM tracks t WHERE t.missing = 0 AND ")
        + visibleTrackPredicate(QStringLiteral("t"), m_showGuessedPlaceholders);
    if (hasScanRoots(m_db)) {
        sql += QStringLiteral(" AND %1").arg(enabledLibraryRootPredicate(QStringLiteral("t"), enabledLibraryRoots()));
    }
    sql += QStringLiteral(" GROUP BY album_artist_name ORDER BY lower(album_artist_name)");
    query.exec(sql);
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
        "MIN(t.parent_dir), uar.rating_0_100, "
        "MIN(t.original_date), MAX(t.file_mtime) "
        "FROM tracks t "
        "LEFT JOIN user_track_ratings utr ON utr.track_path = t.path "
        "LEFT JOIN pending_track_rating_writes p ON p.track_path = t.path "
        "LEFT JOIN user_album_ratings uar ON uar.album_artist_name = t.album_artist_name AND uar.album_title = t.album_title "
        "WHERE t.album_artist_name = ? AND t.missing = 0 AND %3 %2 "
        "GROUP BY t.album_title, uar.rating_0_100")
                      .arg(effectiveTrackRating,
                           hasScanRoots(m_db) ? QStringLiteral("AND %1").arg(enabledLibraryRootPredicate(QStringLiteral("t"), enabledLibraryRoots())) : QString(),
                           visibleTrackPredicate(QStringLiteral("t"), m_showGuessedPlaceholders)));
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
        album.originalDate = query.value(7).toString();
        album.addedMtime = query.value(8).toLongLong();
        albums.push_back(album);
    }
    return albums;
}

QVector<Track> Database::tracksForArtist(const QString &albumArtist, const QString &albumTitleFilter) const
{
    return tracksForArtist(albumArtist,
                           albumTitleFilter.isEmpty() ? QStringList() : QStringList{albumTitleFilter});
}

QVector<Track> Database::tracksForArtist(const QString &albumArtist, const QStringList &albumTitleFilters) const
{
    QStringList filters = albumTitleFilters;
    filters.removeAll(QString());

    QVector<Track> tracks;
    QSqlQuery query(m_db);
    QString sql = QStringLiteral(
        "SELECT t.path, t.parent_dir, t.filename, t.title, t.artist_name, t.album_artist_name, t.album_title, "
        "t.track_number, t.disc_number, t.duration_ms, t.rating_0_100, utr.rating_0_100, t.date, t.original_date, t.file_size, p.status "
        "FROM tracks t "
        "LEFT JOIN user_track_ratings utr ON utr.track_path = t.path "
        "LEFT JOIN pending_track_rating_writes p ON p.track_path = t.path "
        "WHERE t.album_artist_name = ? AND t.missing = 0 AND ")
        + visibleTrackPredicate(QStringLiteral("t"), m_showGuessedPlaceholders);
    if (hasScanRoots(m_db)) {
        sql += QStringLiteral(" AND %1").arg(enabledLibraryRootPredicate(QStringLiteral("t"), enabledLibraryRoots()));
    }
    if (!filters.isEmpty()) {
        sql += QStringLiteral(" AND t.album_title IN (%1)").arg(sqlPlaceholders(filters.size()));
    }
    sql += QStringLiteral(" ORDER BY lower(t.album_title), t.disc_number, t.track_number, lower(t.title)");
    query.prepare(sql);
    query.addBindValue(albumArtist);
    for (const QString &filter : filters) {
        query.addBindValue(filter);
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

Track Database::trackForPath(const QString &path) const
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT t.path, t.parent_dir, t.filename, t.title, t.artist_name, t.album_artist_name, t.album_title, "
        "t.track_number, t.disc_number, t.duration_ms, t.rating_0_100, utr.rating_0_100, t.date, t.original_date, t.file_size, t.file_mtime, p.status "
        "FROM tracks t "
        "LEFT JOIN user_track_ratings utr ON utr.track_path = t.path "
        "LEFT JOIN pending_track_rating_writes p ON p.track_path = t.path "
        "WHERE t.path = ?"));
    query.addBindValue(path);
    if (!query.exec() || !query.next()) {
        return {};
    }

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
    const QString pendingStatus = query.value(16).toString();
    const bool pendingDbRating = pendingStatus == QStringLiteral("pending")
        || pendingStatus == QStringLiteral("failed")
        || pendingStatus == QStringLiteral("blocked_no_writable_path");
    track.effectiveRating0To100 = pendingDbRating && track.hasUserRating
        ? query.value(11).toInt()
        : (track.rating0To100 >= 0 ? track.rating0To100 : (track.hasUserRating ? query.value(11).toInt() : Rating::unset));
    track.date = query.value(12).toString();
    track.originalDate = query.value(13).toString();
    track.fileSize = query.value(14).toLongLong();
    track.fileMtime = query.value(15).toLongLong();
    return track;
}

QVector<Track> Database::searchTracksLike(const QString &text, int limit) const
{
    QVector<Track> tracks;
    const QString needle = text.trimmed();
    if (needle.isEmpty() || limit <= 0) {
        return tracks;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT t.path, t.parent_dir, t.filename, t.title, t.artist_name, t.album_artist_name, t.album_title, "
        "t.track_number, t.disc_number, t.duration_ms, t.rating_0_100, utr.rating_0_100, t.date, t.original_date, t.file_size, t.file_mtime, p.status "
        "FROM tracks t "
        "LEFT JOIN user_track_ratings utr ON utr.track_path = t.path "
        "LEFT JOIN pending_track_rating_writes p ON p.track_path = t.path "
        "WHERE t.missing = 0 AND (t.title LIKE ? ESCAPE '\\' OR t.artist_name LIKE ? ESCAPE '\\' "
        "OR t.album_artist_name LIKE ? ESCAPE '\\' OR t.album_title LIKE ? ESCAPE '\\' OR t.filename LIKE ? ESCAPE '\\') "
        "ORDER BY lower(t.album_artist_name), lower(t.album_title), t.disc_number, t.track_number, lower(t.title) "
        "LIMIT ?"));
    QString escaped = needle;
    escaped.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    escaped.replace(QLatin1Char('%'), QLatin1String("\\%"));
    escaped.replace(QLatin1Char('_'), QLatin1String("\\_"));
    const QString pattern = QStringLiteral("%%%1%%").arg(escaped);
    for (int i = 0; i < 5; ++i) {
        query.addBindValue(pattern);
    }
    query.addBindValue(limit);
    if (!query.exec()) {
        return tracks;
    }
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
        const QString pendingStatus = query.value(16).toString();
        const bool pendingDbRating = pendingStatus == QStringLiteral("pending")
            || pendingStatus == QStringLiteral("failed")
            || pendingStatus == QStringLiteral("blocked_no_writable_path");
        track.effectiveRating0To100 = pendingDbRating && track.hasUserRating
            ? query.value(11).toInt()
            : (track.rating0To100 >= 0 ? track.rating0To100 : (track.hasUserRating ? query.value(11).toInt() : Rating::unset));
        track.date = query.value(12).toString();
        track.originalDate = query.value(13).toString();
        track.fileSize = query.value(14).toLongLong();
        track.fileMtime = query.value(15).toLongLong();
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

bool Database::removeSetting(const QString &key)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM app_settings WHERE key = ?"));
    query.addBindValue(key);
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

QVector<ScanRoot> Database::scanRoots() const
{
    QVector<ScanRoot> roots;
    QSqlQuery query(m_db);
    query.exec(QStringLiteral(
        "SELECT id, name, path, scan_enabled, library_enabled, created_at, updated_at, last_scanned_at, last_error "
        "FROM scan_roots "
        "ORDER BY lower(path)"));
    while (query.next()) {
        ScanRoot root;
        root.id = query.value(0).toInt();
        root.name = query.value(1).toString();
        root.path = query.value(2).toString();
        root.scanEnabled = query.value(3).toBool();
        root.libraryEnabled = query.value(4).toBool();
        root.createdAt = query.value(5).toString();
        root.updatedAt = query.value(6).toString();
        root.lastScannedAt = query.value(7).toString();
        root.lastError = query.value(8).toString();
        roots.push_back(root);
    }
    return roots;
}

QVector<ScanRoot> Database::enabledScanRoots() const
{
    QVector<ScanRoot> roots;
    for (const ScanRoot &root : scanRoots()) {
        if (root.scanEnabled) {
            roots.push_back(root);
        }
    }
    return roots;
}

QVector<ScanRoot> Database::enabledLibraryRoots() const
{
    QVector<ScanRoot> roots;
    for (const ScanRoot &root : scanRoots()) {
        if (root.libraryEnabled) {
            roots.push_back(root);
        }
    }
    return roots;
}

bool Database::saveScanRoot(const ScanRoot &root)
{
    const QString path = cleanRootPath(root.path);
    if (path.trimmed().isEmpty()) {
        m_lastError = QStringLiteral("Source directory path is required");
        return false;
    }

    const QFileInfo info(path);
    if (!info.exists() || !info.isDir() || info.isSymLink()) {
        m_lastError = QStringLiteral("Source directory must be an existing non-symlink directory");
        return false;
    }

    const QString fallbackName = info.fileName().isEmpty() ? path : info.fileName();
    const QString name = root.name.trimmed().isEmpty() ? fallbackName : root.name.trimmed();
    QSqlQuery query(m_db);
    if (root.id > 0) {
        query.prepare(QStringLiteral(
            "UPDATE scan_roots "
            "SET name = ?, path = ?, scan_enabled = ?, library_enabled = ?, updated_at = datetime('now') "
            "WHERE id = ?"));
        query.addBindValue(name);
        query.addBindValue(path);
        query.addBindValue(root.scanEnabled ? 1 : 0);
        query.addBindValue(root.libraryEnabled ? 1 : 0);
        query.addBindValue(root.id);
    } else {
        query.prepare(QStringLiteral(
            "INSERT INTO scan_roots(path, name, scan_enabled, library_enabled, created_at, updated_at) "
            "VALUES(?, ?, ?, ?, datetime('now'), datetime('now'))"));
        query.addBindValue(path);
        query.addBindValue(name);
        query.addBindValue(root.scanEnabled ? 1 : 0);
        query.addBindValue(root.libraryEnabled ? 1 : 0);
    }

    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool Database::removeScanRoot(int id)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM scan_roots WHERE id = ?"));
    query.addBindValue(id);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool Database::setScanRootLastScanned(int id, const QString &lastError)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE scan_roots "
        "SET last_scanned_at = datetime('now'), last_error = ?, updated_at = datetime('now') "
        "WHERE id = ?"));
    query.addBindValue(lastError);
    query.addBindValue(id);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

QVector<Track> Database::tracksForDirectory(const QString &directory) const
{
    QVector<Track> tracks;
    const QString cleanDirectory = cleanRootPath(directory);
    QSqlQuery query(m_db);
    QString sql = QStringLiteral(
        "SELECT t.path, t.parent_dir, t.filename, t.title, t.artist_name, t.album_artist_name, t.album_title, "
        "t.track_number, t.disc_number, t.duration_ms, t.rating_0_100, utr.rating_0_100, t.date, t.original_date, t.file_size, p.status "
        "FROM tracks t "
        "LEFT JOIN user_track_ratings utr ON utr.track_path = t.path "
        "LEFT JOIN pending_track_rating_writes p ON p.track_path = t.path "
        "WHERE t.parent_dir = ? AND t.missing = 0");
    if (hasScanRoots(m_db)) {
        sql += QStringLiteral(" AND %1").arg(enabledLibraryRootPredicate(QStringLiteral("t"), enabledLibraryRoots()));
    }
    sql += QStringLiteral(" ORDER BY t.disc_number, t.track_number, lower(t.filename)");
    query.prepare(sql);
    query.addBindValue(cleanDirectory);
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

QVector<Track> Database::randomTracks(int count, const QSet<QString> &excludePaths) const
{
    QVector<Track> tracks;
    if (count <= 0) {
        return tracks;
    }
    // Over-fetch so the in-memory exclusion of queued paths still leaves enough
    // rows; ORDER BY RANDOM() is fine at library scale for an occasional pick.
    const int fetch = count + static_cast<int>(excludePaths.size());
    QSqlQuery query(m_db);
    QString sql = QStringLiteral(
        "SELECT t.path, t.parent_dir, t.filename, t.title, t.artist_name, t.album_artist_name, t.album_title, "
        "t.track_number, t.disc_number, t.duration_ms, t.rating_0_100, utr.rating_0_100, t.date, t.original_date, t.file_size, p.status "
        "FROM tracks t "
        "LEFT JOIN user_track_ratings utr ON utr.track_path = t.path "
        "LEFT JOIN pending_track_rating_writes p ON p.track_path = t.path "
        "WHERE t.missing = 0");
    if (hasScanRoots(m_db)) {
        sql += QStringLiteral(" AND %1").arg(enabledLibraryRootPredicate(QStringLiteral("t"), enabledLibraryRoots()));
    }
    sql += QStringLiteral(" ORDER BY RANDOM() LIMIT ?");
    query.prepare(sql);
    query.addBindValue(fetch);
    query.exec();
    while (query.next() && tracks.size() < count) {
        const QString path = query.value(0).toString();
        if (excludePaths.contains(path)) {
            continue;
        }
        Track track;
        track.path = path;
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

QStringList Database::localLibraryDirectories(const QString &parentDirectory) const
{
    QStringList directories;
    const QString parent = parentDirectory.trimmed().isEmpty() ? QString() : cleanRootPath(parentDirectory);
    QSqlQuery query(m_db);
    QString sql = QStringLiteral("SELECT DISTINCT t.parent_dir FROM tracks t WHERE t.missing = 0");
    if (hasScanRoots(m_db)) {
        sql += QStringLiteral(" AND %1").arg(enabledLibraryRootPredicate(QStringLiteral("t"), enabledLibraryRoots()));
    }
    sql += QStringLiteral(" ORDER BY lower(t.parent_dir)");
    query.exec(sql);

    const QVector<ScanRoot> libraryRoots = enabledLibraryRoots();
    while (query.next()) {
        const QString dir = cleanRootPath(query.value(0).toString());
        if (parent.isEmpty()) {
            if (libraryRoots.isEmpty()) {
                directories.push_back(dir);
                continue;
            }
            for (const ScanRoot &root : libraryRoots) {
                if (dir == root.path || dir.startsWith(root.path + QLatin1Char('/'))) {
                    directories.push_back(root.path);
                    break;
                }
            }
            continue;
        }

        if (!dir.startsWith(parent + QLatin1Char('/'))) {
            continue;
        }
        const QString remainder = dir.mid(parent.size() + 1);
        const qsizetype slash = remainder.indexOf(QLatin1Char('/'));
        directories.push_back(slash < 0 ? dir : parent + QLatin1Char('/') + remainder.left(slash));
    }

    directories.removeDuplicates();
    std::sort(directories.begin(), directories.end(), [](const QString &left, const QString &right) {
        return QString::localeAwareCompare(left, right) < 0;
    });
    return directories;
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

QVector<Search::SearchRecord> Database::allTracksForSearch() const
{
    QVector<Search::SearchRecord> records;
    QSqlQuery query(m_db);
    QString sql = QStringLiteral(
        "SELECT t.path, t.filename, t.title, t.artist_name, t.album_artist_name, t.album_title, "
        "t.date, t.duration_ms, t.sample_rate_hz, t.bitrate_kbps, t.channels, t.codec, "
        "COALESCE(utr.rating_0_100, t.rating_0_100), "
        "t.track_number, t.disc_number, t.file_mtime, t.file_size, t.bit_depth "
        "FROM tracks t "
        "LEFT JOIN user_track_ratings utr ON utr.track_path = t.path "
        "WHERE t.missing = 0 AND t.metadata_scanned = 1");
    if (hasScanRoots(m_db)) {
        sql += QStringLiteral(" AND %1").arg(enabledLibraryRootPredicate(QStringLiteral("t"), enabledLibraryRoots()));
    }
    if (!query.exec(sql)) {
        return records;
    }
    // Dedup pool for the high-repetition fields (one buffer per distinct value).
    QHash<QString, QString> pool;
    while (query.next()) {
        Search::SearchRecord rec;
        rec.path              = query.value(0).toString();
        rec.filename          = query.value(1).toString();
        rec.title             = query.value(2).toString();
        rec.artistName        = internString(pool, query.value(3).toString());
        rec.albumArtistName   = internString(pool, query.value(4).toString());
        rec.albumTitle        = internString(pool, query.value(5).toString());
        rec.date              = internString(pool, query.value(6).toString());
        rec.durationMs        = query.value(7).toLongLong();
        rec.sampleRateHz      = query.value(8).toInt();
        rec.bitrateKbps       = query.value(9).toInt();
        rec.channels          = query.value(10).toInt();
        rec.codec             = internString(pool, query.value(11).toString());
        rec.rating0To100      = query.value(12).isNull() ? -1 : query.value(12).toInt();
        rec.trackNumber       = query.value(13).toInt();
        rec.discNumber        = query.value(14).toInt();
        rec.fileMtime         = query.value(15).toLongLong();
        rec.fileSize          = query.value(16).toLongLong();
        rec.bitDepth          = query.value(17).toInt();
        rec.source            = Search::TrackSource::Local;
        // Pre-compute folded versions (lowercase + ASCII transliteration +
        // romaji) for diacritic/script-insensitive matching; intern the
        // repeated ones (path/title/filename are near-unique, left as-is).
        rec.normTitle        = Search::Fold::foldText(rec.title);
        rec.normArtist       = internString(pool, Search::Fold::foldText(rec.artistName));
        rec.normAlbumArtist  = internString(pool, Search::Fold::foldText(rec.albumArtistName));
        rec.normAlbum        = internString(pool, Search::Fold::foldText(rec.albumTitle));
        rec.normFilename     = Search::Fold::foldText(rec.filename);
        rec.normPath         = Search::Fold::foldText(rec.path);
        records.push_back(std::move(rec));
    }
    return records;
}

QVector<Search::SearchRecord> Database::allMpdTracksForSearch() const
{
    QVector<Search::SearchRecord> records;
    QSqlQuery query(m_db);
    const QString sql = QStringLiteral(
        "SELECT uri, title, artist_name, album_artist_name, album_title, date, duration_ms, "
        "track_number, disc_number "
        "FROM mpd_tracks");
    if (!query.exec(sql)) {
        return records;
    }
    QHash<QString, QString> pool;
    while (query.next()) {
        Search::SearchRecord rec;
        rec.path            = query.value(0).toString();
        rec.filename        = rec.path.section(QLatin1Char('/'), -1);
        rec.title           = query.value(1).toString();
        rec.artistName      = internString(pool, query.value(2).toString());
        rec.albumArtistName = internString(pool, query.value(3).toString());
        rec.albumTitle      = internString(pool, query.value(4).toString());
        rec.date            = internString(pool, query.value(5).toString());
        rec.durationMs      = query.value(6).toLongLong();
        rec.trackNumber     = query.value(7).toInt();
        rec.discNumber      = query.value(8).toInt();
        rec.rating0To100    = -1;
        rec.source          = Search::TrackSource::Mpd;
        rec.normTitle        = Search::Fold::foldText(rec.title);
        rec.normArtist       = internString(pool, Search::Fold::foldText(rec.artistName));
        rec.normAlbumArtist  = internString(pool, Search::Fold::foldText(rec.albumArtistName));
        rec.normAlbum        = internString(pool, Search::Fold::foldText(rec.albumTitle));
        rec.normFilename     = Search::Fold::foldText(rec.filename);
        rec.normPath         = Search::Fold::foldText(rec.path);
        records.push_back(std::move(rec));
    }
    return records;
}

QVector<Track> Database::mpdTracksForArtist(const QString &albumArtist, const QString &musicDirectory, const QString &albumTitleFilter) const
{
    return mpdTracksForArtist(albumArtist, musicDirectory,
                              albumTitleFilter.isEmpty() ? QStringList() : QStringList{albumTitleFilter});
}

QVector<Track> Database::mpdTracksForArtist(const QString &albumArtist, const QString &musicDirectory, const QStringList &albumTitleFilters) const
{
    QStringList filters = albumTitleFilters;
    filters.removeAll(QString());

    QVector<Track> tracks;
    QSqlQuery query(m_db);
    QString sql = QStringLiteral(
        "SELECT uri, title, artist_name, album_artist_name, album_title, "
        "track_number, disc_number, duration_ms, date "
        "FROM mpd_tracks "
        "WHERE album_artist_name = ?");
    if (!filters.isEmpty()) {
        sql += QStringLiteral(" AND album_title IN (%1)").arg(sqlPlaceholders(filters.size()));
    }
    sql += QStringLiteral(" ORDER BY lower(album_title), disc_number, track_number, lower(title)");
    query.prepare(sql);
    query.addBindValue(albumArtist);
    for (const QString &filter : filters) {
        query.addBindValue(filter);
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
