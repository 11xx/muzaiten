#include "db/Database.h"

#include "core/GenreTags.h"
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

constexpr int kInteractiveCacheKiB = 65536;
constexpr int kIdleCacheKiB = 2000;

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

Rating::Source sourceFromName(const QString &name)
{
    if (name == QStringLiteral("musicbee-compatible")) {
        return Rating::Source::MusicBeeCompatible;
    }
    if (name == QStringLiteral("vorbis-rating")) {
        return Rating::Source::VorbisRating;
    }
    if (name == QStringLiteral("id3-popm")) {
        return Rating::Source::Id3Popularimeter;
    }
    if (name == QStringLiteral("mp4-rate")) {
        return Rating::Source::Mp4Rate;
    }
    if (name == QStringLiteral("unknown")) {
        return Rating::Source::Unknown;
    }
    return Rating::Source::None;
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

QString trackFlagColumn(Database::TrackFlag flag)
{
    switch (flag) {
    case Database::TrackFlag::NeverRadio:
        return QStringLiteral("never_radio");
    case Database::TrackFlag::NoLearn:
        return QStringLiteral("no_learn");
    }
    return {};
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

QStringList radioGenreLookupTerms(const QStringList &foldedGenres, const QHash<QString, QString> &aliases)
{
    QStringList terms;
    QSet<QString> seen;
    for (const QString &genre : foldedGenres) {
        const QString canonical = GenreTags::canonical(genre, aliases);
        if (!canonical.isEmpty() && !seen.contains(canonical)) {
            seen.insert(canonical);
            terms.push_back(canonical);
        }
        for (auto it = aliases.cbegin(); it != aliases.cend(); ++it) {
            if (it.value() == canonical && !seen.contains(it.key())) {
                seen.insert(it.key());
                terms.push_back(it.key());
            }
        }
    }
    return terms;
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

// Canonical column list + FROM/JOINs shared by every QVector<Track>/Track reader
// below. All of them select these columns in this exact order and decode each row
// through readTrackRow(), so the pending-rating overlay (the utr/p LEFT JOINs and
// the effective-rating resolution) stays in lockstep across queries instead of
// being hand-copied per site — the divergence review finding #6 guarded against.
constexpr char kTrackSelectColumns[] =
    "t.path, t.parent_dir, t.filename, t.title, t.artist_name, t.album_artist_name, t.album_title, "
    "t.track_number, t.disc_number, t.duration_ms, t.rating_0_100, utr.rating_0_100, t.date, "
    "t.original_date, t.file_size, t.file_mtime, p.status, t.missing";

constexpr char kTrackSelectFrom[] =
    "FROM tracks t "
    "LEFT JOIN user_track_ratings utr ON utr.track_path = t.path "
    "LEFT JOIN pending_track_rating_writes p ON p.track_path = t.path ";

// "SELECT <cols> <from/joins> " prefix; callers append their WHERE/ORDER/LIMIT.
QString trackSelectPrefix()
{
    return QStringLiteral("SELECT ") + QString::fromLatin1(kTrackSelectColumns) + QLatin1Char(' ')
        + QString::fromLatin1(kTrackSelectFrom);
}

// Decode one row produced by trackSelectPrefix() into a Track. Resolves the
// effective rating from the pending-write status: a queued/failed/blocked DB write
// shadows the on-disk rating with the user's latest value until the writer drains;
// otherwise the on-disk rating wins, falling back to the user rating only when no
// DB rating is set.
Track readTrackRow(const QSqlQuery &query)
{
    Track track;
    track.path            = query.value(0).toString();
    track.parentDir       = query.value(1).toString();
    track.filename        = query.value(2).toString();
    track.title           = query.value(3).toString();
    track.artistName      = query.value(4).toString();
    track.albumArtistName = query.value(5).toString();
    track.albumTitle      = query.value(6).toString();
    track.trackNumber     = query.value(7).toInt();
    track.discNumber      = query.value(8).toInt();
    track.durationMs      = query.value(9).toLongLong();
    track.rating0To100    = query.value(10).isNull() ? Rating::unset : query.value(10).toInt();
    track.hasUserRating   = !query.value(11).isNull();
    const QString pendingStatus = query.value(16).toString();
    const bool pendingDbRating = pendingStatus == QStringLiteral("pending")
        || pendingStatus == QStringLiteral("failed")
        || pendingStatus == QStringLiteral("blocked_no_writable_path");
    track.effectiveRating0To100 = pendingDbRating && track.hasUserRating
        ? query.value(11).toInt()
        : (track.rating0To100 >= 0 ? track.rating0To100 : (track.hasUserRating ? query.value(11).toInt() : Rating::unset));
    track.date         = query.value(12).toString();
    track.originalDate = query.value(13).toString();
    track.fileSize     = query.value(14).toLongLong();
    track.fileMtime    = query.value(15).toLongLong();
    track.missing      = query.value(17).toInt() != 0;
    return track;
}

// Leading 4-digit year of a release date ("2004-05-01", "2004", ...); 0 when the
// string has no parseable leading year. Kept in C++ rather than SQL so the date
// forms scanned into `date`/`original_date` are all handled the same way.
int parseLeadingYear(const QString &date)
{
    if (date.size() < 4) {
        return 0;
    }
    bool ok = false;
    const int year = QStringView(date).left(4).toInt(&ok);
    return ok ? year : 0;
}

// GROUP_CONCAT separator for the folded genre list: the ASCII unit separator can
// never appear inside a simplified()/case-folded genre value.
constexpr QChar kGenreSeparator = QChar(0x1F);

// Decode one row from the radio-candidate query (see the two callers below);
// mirrors readTrackRow's pending-write effective-rating overlay exactly.
RadioCandidateRow readRadioCandidateRow(const QSqlQuery &query)
{
    RadioCandidateRow row;
    row.path            = query.value(0).toString();
    row.artistName      = query.value(1).toString();
    row.title           = query.value(2).toString();
    row.albumArtistName = query.value(3).toString();
    row.albumTitle      = query.value(4).toString();
    row.mbRecordingId   = query.value(5).toString();
    row.releaseGroupId  = query.value(6).toString();
    const QString genres = query.value(7).toString();
    if (!genres.isEmpty()) {
        row.genresFolded = genres.split(kGenreSeparator, Qt::SkipEmptyParts);
    }
    const QString original = query.value(8).toString();
    row.year = parseLeadingYear(!original.isEmpty() ? original : query.value(9).toString());
    const int scannedRating = query.value(10).isNull() ? Rating::unset : query.value(10).toInt();
    row.hasUserRating = !query.value(11).isNull();
    const QString status = query.value(12).toString();
    const bool pendingDbRating = status == QStringLiteral("pending")
        || status == QStringLiteral("failed")
        || status == QStringLiteral("blocked_no_writable_path");
    row.effectiveRating0To100 = pendingDbRating && row.hasUserRating
        ? query.value(11).toInt()
        : (scannedRating >= 0 ? scannedRating : (row.hasUserRating ? query.value(11).toInt() : Rating::unset));
    return row;
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
    pragma.exec(QStringLiteral("PRAGMA cache_size=-%1").arg(kInteractiveCacheKiB));

    return migrate();
}

void Database::releaseCacheMemory()
{
    if (!m_db.isOpen()) {
        return;
    }

    QSqlQuery pragma(m_db);
    pragma.exec(QStringLiteral("PRAGMA cache_size=-%1").arg(kIdleCacheKiB));
    pragma.exec(QStringLiteral("PRAGMA shrink_memory"));
}

void Database::restoreCacheMemory()
{
    if (!m_db.isOpen()) {
        return;
    }

    QSqlQuery pragma(m_db);
    pragma.exec(QStringLiteral("PRAGMA cache_size=-%1").arg(kInteractiveCacheKiB));
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

    // v9: sort/reading names (Picard *sort tags) on tracks, folded into the
    // search index for non-Latin recall (romaji/kana sort names).
    const QVector<QPair<QString, QString>> sortColumns = {
        {QStringLiteral("title_sort"),        QStringLiteral("title_sort TEXT")},
        {QStringLiteral("artist_sort"),       QStringLiteral("artist_sort TEXT")},
        {QStringLiteral("album_artist_sort"), QStringLiteral("album_artist_sort TEXT")},
        {QStringLiteral("album_sort"),        QStringLiteral("album_sort TEXT")},
    };
    for (const auto &column : sortColumns) {
        if (!ensureColumn(m_db, QStringLiteral("tracks"), column.first, column.second, &m_lastError)) {
            return false;
        }
    }

    // (A transient backfill that populated these columns from existing metadata
    // blobs lived here; removed now that the library has been rescanned. New/
    // updated rows get the sort columns from TagReader on scan.)

    const QStringList v9Statements = {
        QStringLiteral("INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(9, datetime('now'))"),
    };
    for (const QString &statement : v9Statements) {
        if (!execSql(query, statement, &m_lastError)) {
            return false;
        }
    }

    // v10: queryable genres. track_genres mirrors the GENRE tag(s) already
    // captured in each track's metadata blob into SQL rows, so genre browsing/
    // filtering doesn't need to decode a blob per track.
    const QStringList v10Statements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS track_genres (track_id INTEGER NOT NULL, genre TEXT NOT NULL, genre_folded TEXT NOT NULL, PRIMARY KEY(track_id, genre_folded), FOREIGN KEY(track_id) REFERENCES tracks(id) ON DELETE CASCADE)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_track_genres_folded ON track_genres(genre_folded)"),
    };
    for (const QString &statement : v10Statements) {
        if (!execSql(query, statement, &m_lastError)) {
            return false;
        }
    }

    // One-time backfill from blobs already in the DB, guarded so it never
    // reruns: unlike v7's every-startup WHERE-clause backfill, decoding every
    // track's metadata blob on each launch would not scale to large libraries.
    {
        QSqlQuery versionCheck(m_db);
        versionCheck.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version = 10"));
        if (!versionCheck.exec()) {
            m_lastError = versionCheck.lastError().text();
            return false;
        }
        if (!versionCheck.next()) {
            if (!m_db.transaction()) {
                m_lastError = m_db.lastError().text();
                return false;
            }
            QSqlQuery bfQuery(m_db);
            bfQuery.prepare(QStringLiteral(
                "SELECT t.id, m.raw_size, m.data FROM tracks t JOIN track_metadata m ON m.track_id = t.id"));
            if (!bfQuery.exec()) {
                m_lastError = bfQuery.lastError().text();
                m_db.rollback();
                return false;
            }
            QSqlQuery ins(m_db);
            ins.prepare(QStringLiteral(
                "INSERT OR IGNORE INTO track_genres(track_id, genre, genre_folded) VALUES(?, ?, ?)"));
            while (bfQuery.next()) {
                const qint64 trackId = bfQuery.value(0).toLongLong();
                const qint64 rawSize = bfQuery.value(1).toLongLong();
                const QByteArray blob = bfQuery.value(2).toByteArray();
                const MetadataBlob::FullMetadata meta = MetadataBlob::decode(blob, rawSize);
                for (const QString &genre : GenreTags::fromMetadata(meta)) {
                    ins.addBindValue(trackId);
                    ins.addBindValue(genre);
                    ins.addBindValue(GenreTags::folded(genre));
                    if (!ins.exec()) {
                        m_lastError = ins.lastError().text();
                        m_db.rollback();
                        return false;
                    }
                }
            }
            if (!m_db.commit()) {
                m_lastError = m_db.lastError().text();
                return false;
            }
        }
    }

    if (!execSql(query, QStringLiteral("INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(10, datetime('now'))"), &m_lastError)) {
        return false;
    }

    // v11: per-track taste flags. AppCore applies these per song key so the
    // storage remains path-scoped and does not need recommender identity logic.
    const QStringList v11Statements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS user_track_flags (track_path TEXT PRIMARY KEY, never_radio INTEGER NOT NULL DEFAULT 0, no_learn INTEGER NOT NULL DEFAULT 0, updated_at TEXT NOT NULL)"),
        QStringLiteral("INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(11, datetime('now'))"),
    };
    for (const QString &statement : v11Statements) {
        if (!execSql(query, statement, &m_lastError)) {
            return false;
        }
    }

    // v12: engine-side genre aliases. Raw track_genres rows stay as scanned;
    // radio lookups and scoring canonicalize through this table at session time.
    const QStringList v12Statements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS genre_aliases (alias_folded TEXT PRIMARY KEY, canonical_folded TEXT NOT NULL, updated_at TEXT NOT NULL)"),
        QStringLiteral("INSERT OR IGNORE INTO genre_aliases(alias_folded, canonical_folded, updated_at) VALUES('clássica', 'classical', datetime('now'))"),
        QStringLiteral("INSERT OR IGNORE INTO genre_aliases(alias_folded, canonical_folded, updated_at) VALUES('classique', 'classical', datetime('now'))"),
        QStringLiteral("INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(12, datetime('now'))"),
    };
    for (const QString &statement : v12Statements) {
        if (!execSql(query, statement, &m_lastError)) {
            return false;
        }
    }

    // v13: re-backfill queryable genres after the splitter learned about '|'
    // packed genre tags. Metadata blobs stay untouched; track_genres is the
    // derived index and can be rebuilt deterministically.
    {
        QSqlQuery versionCheck(m_db);
        versionCheck.prepare(QStringLiteral("SELECT 1 FROM schema_migrations WHERE version = 13"));
        if (!versionCheck.exec()) {
            m_lastError = versionCheck.lastError().text();
            return false;
        }
        if (!versionCheck.next()) {
            if (!m_db.transaction()) {
                m_lastError = m_db.lastError().text();
                return false;
            }
            if (!execSql(query, QStringLiteral("DELETE FROM track_genres"), &m_lastError)) {
                m_db.rollback();
                return false;
            }
            QSqlQuery bfQuery(m_db);
            bfQuery.prepare(QStringLiteral(
                "SELECT t.id, m.raw_size, m.data FROM tracks t JOIN track_metadata m ON m.track_id = t.id"));
            if (!bfQuery.exec()) {
                m_lastError = bfQuery.lastError().text();
                m_db.rollback();
                return false;
            }
            QSqlQuery ins(m_db);
            ins.prepare(QStringLiteral(
                "INSERT OR IGNORE INTO track_genres(track_id, genre, genre_folded) VALUES(?, ?, ?)"));
            while (bfQuery.next()) {
                const qint64 trackId = bfQuery.value(0).toLongLong();
                const qint64 rawSize = bfQuery.value(1).toLongLong();
                const QByteArray blob = bfQuery.value(2).toByteArray();
                const MetadataBlob::FullMetadata meta = MetadataBlob::decode(blob, rawSize);
                for (const QString &genre : GenreTags::fromMetadata(meta)) {
                    ins.addBindValue(trackId);
                    ins.addBindValue(genre);
                    ins.addBindValue(GenreTags::folded(genre));
                    if (!ins.exec()) {
                        m_lastError = ins.lastError().text();
                        m_db.rollback();
                        return false;
                    }
                }
            }
            if (!m_db.commit()) {
                m_lastError = m_db.lastError().text();
                return false;
            }
        }
    }

    if (!execSql(query, QStringLiteral("INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(13, datetime('now'))"), &m_lastError)) {
        return false;
    }

    return Schema::currentVersion == 13;
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
        "INSERT INTO tracks(path, parent_dir, filename, title, artist_name, album_artist_name, album_title, album_id, track_number, track_total, disc_number, disc_total, duration_ms, rating_0_100, rating_source, play_count, date, original_date, musicbrainz_recording_id, musicbrainz_track_id, musicbrainz_release_id, file_size, file_mtime, scanned_at, scan_error, sample_rate_hz, bitrate_kbps, channels, codec, bit_depth, title_sort, artist_sort, album_artist_sort, album_sort) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, datetime('now'), ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(path) DO UPDATE SET parent_dir=excluded.parent_dir, filename=excluded.filename, title=excluded.title, artist_name=excluded.artist_name, album_artist_name=excluded.album_artist_name, album_title=excluded.album_title, album_id=excluded.album_id, track_number=excluded.track_number, track_total=excluded.track_total, disc_number=excluded.disc_number, disc_total=excluded.disc_total, duration_ms=excluded.duration_ms, rating_0_100=excluded.rating_0_100, rating_source=excluded.rating_source, play_count=excluded.play_count, date=excluded.date, original_date=excluded.original_date, musicbrainz_recording_id=excluded.musicbrainz_recording_id, musicbrainz_track_id=excluded.musicbrainz_track_id, musicbrainz_release_id=excluded.musicbrainz_release_id, file_size=excluded.file_size, file_mtime=excluded.file_mtime, scanned_at=datetime('now'), scan_error=excluded.scan_error, missing=0, missing_since=NULL, sample_rate_hz=excluded.sample_rate_hz, bitrate_kbps=excluded.bitrate_kbps, channels=excluded.channels, codec=excluded.codec, bit_depth=excluded.bit_depth, title_sort=excluded.title_sort, artist_sort=excluded.artist_sort, album_artist_sort=excluded.album_artist_sort, album_sort=excluded.album_sort, "
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
    query.addBindValue(track.titleSort.isEmpty() ? QVariant() : QVariant(track.titleSort));
    query.addBindValue(track.artistSort.isEmpty() ? QVariant() : QVariant(track.artistSort));
    query.addBindValue(track.albumArtistSort.isEmpty() ? QVariant() : QVariant(track.albumArtistSort));
    query.addBindValue(track.albumSort.isEmpty() ? QVariant() : QVariant(track.albumSort));

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

            // Delete-then-insert (not upsert) so genre tags removed on rescan
            // disappear from track_genres instead of lingering.
            const MetadataBlob::FullMetadata meta = MetadataBlob::decode(track.fullMetadataBlob, track.fullMetadataRawSize);
            QSqlQuery deleteGenres(m_db);
            deleteGenres.prepare(QStringLiteral("DELETE FROM track_genres WHERE track_id = ?"));
            deleteGenres.addBindValue(trackId);
            if (!deleteGenres.exec()) {
                m_lastError = deleteGenres.lastError().text();
                return false;
            }
            QSqlQuery insertGenre(m_db);
            insertGenre.prepare(QStringLiteral(
                "INSERT OR IGNORE INTO track_genres(track_id, genre, genre_folded) VALUES(?, ?, ?)"));
            for (const QString &genre : GenreTags::fromMetadata(meta)) {
                insertGenre.addBindValue(trackId);
                insertGenre.addBindValue(genre);
                insertGenre.addBindValue(GenreTags::folded(genre));
                if (!insertGenre.exec()) {
                    m_lastError = insertGenre.lastError().text();
                    return false;
                }
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

QStringList Database::missingTrackPaths() const
{
    QStringList paths;
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral("SELECT path FROM tracks WHERE missing = 1 ORDER BY path"))) {
        return paths;
    }
    while (query.next()) {
        paths.push_back(query.value(0).toString());
    }
    return paths;
}

QList<std::tuple<QString, QString, QString, QString>> Database::trackMatchRows() const
{
    QList<std::tuple<QString, QString, QString, QString>> rows;
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral(
            "SELECT path, artist_name, title, musicbrainz_recording_id FROM tracks WHERE missing = 0"))) {
        return rows;
    }
    while (query.next()) {
        rows.emplaceBack(query.value(0).toString(), query.value(1).toString(),
                         query.value(2).toString(), query.value(3).toString());
    }
    return rows;
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

QStringList Database::genresForTrack(const QString &path) const
{
    QStringList genres;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT g.genre FROM track_genres g JOIN tracks t ON t.id = g.track_id "
        "WHERE t.path = ? ORDER BY g.genre_folded"));
    query.addBindValue(path);
    if (query.exec()) {
        while (query.next()) {
            genres.append(query.value(0).toString());
        }
    }
    return genres;
}

QHash<QString, int> Database::genreTrackCounts(int *taggedTrackTotal) const
{
    QHash<QString, int> counts;
    // track_genres' primary key is (track_id, genre_folded), so COUNT(*) per
    // genre_folded is already a distinct-track count — no DISTINCT needed.
    QSqlQuery query(m_db);
    if (query.exec(QStringLiteral("SELECT genre_folded, COUNT(*) FROM track_genres GROUP BY genre_folded"))) {
        while (query.next()) {
            counts.insert(query.value(0).toString(), query.value(1).toInt());
        }
    }
    if (taggedTrackTotal != nullptr) {
        *taggedTrackTotal = 0;
        QSqlQuery totalQuery(m_db);
        if (totalQuery.exec(QStringLiteral("SELECT COUNT(DISTINCT track_id) FROM track_genres")) && totalQuery.next()) {
            *taggedTrackTotal = totalQuery.value(0).toInt();
        }
    }
    return counts;
}

QStringList Database::sampleArtistsForGenre(const QString &folded, int limit) const
{
    QStringList artists;
    if (folded.isEmpty() || limit <= 0) {
        return artists;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT DISTINCT t.artist_name "
        "FROM track_genres g JOIN tracks t ON t.id = g.track_id "
        "WHERE g.genre_folded = ? AND t.artist_name IS NOT NULL AND t.artist_name <> '' "
        "ORDER BY t.artist_name COLLATE NOCASE, t.artist_name "
        "LIMIT ?"));
    query.addBindValue(folded);
    query.addBindValue(limit);
    if (query.exec()) {
        while (query.next()) {
            artists.append(query.value(0).toString());
        }
    }
    return artists;
}

QHash<QString, QString> Database::genreAliases() const
{
    QHash<QString, QString> aliases;
    QSqlQuery query(m_db);
    if (query.exec(QStringLiteral("SELECT alias_folded, canonical_folded FROM genre_aliases"))) {
        while (query.next()) {
            aliases.insert(query.value(0).toString(), query.value(1).toString());
        }
    }
    return aliases;
}

bool Database::setGenreAlias(const QString &alias, const QString &canonical)
{
    const QString aliasFolded = GenreTags::folded(alias);
    const QString canonicalFolded = GenreTags::folded(canonical);
    if (aliasFolded.isEmpty() || canonicalFolded.isEmpty()) {
        m_lastError = QStringLiteral("Genre alias and canonical genre are required");
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO genre_aliases(alias_folded, canonical_folded, updated_at) VALUES(?, ?, datetime('now')) "
        "ON CONFLICT(alias_folded) DO UPDATE SET canonical_folded=excluded.canonical_folded, updated_at=datetime('now')"));
    query.addBindValue(aliasFolded);
    query.addBindValue(canonicalFolded);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool Database::removeGenreAlias(const QString &alias)
{
    const QString aliasFolded = GenreTags::folded(alias);
    if (aliasFolded.isEmpty()) {
        m_lastError = QStringLiteral("Genre alias is required");
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM genre_aliases WHERE alias_folded = ?"));
    query.addBindValue(aliasFolded);
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
    QString sql = trackSelectPrefix()
        + QStringLiteral("WHERE t.album_artist_name = ? AND t.missing = 0 AND ")
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
        tracks.push_back(readTrackRow(query));
    }
    return tracks;
}

Track Database::trackForPath(const QString &path) const
{
    QSqlQuery query(m_db);
    query.prepare(trackSelectPrefix() + QStringLiteral("WHERE t.path = ?"));
    query.addBindValue(path);
    if (!query.exec() || !query.next()) {
        return {};
    }
    return readTrackRow(query);
}

void Database::enrichTrackForStatus(Track &track) const
{
    // Fill the rich scanned columns that the queue/album loaders skip for speed,
    // so the status JSON can expose audio props, totals, sort/reading names, the
    // rating source, and the track-level MusicBrainz ids. Leaves already-set
    // fields (title/artist/ratings/…) untouched.
    if (track.path.isEmpty()) {
        return;
    }
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT track_total, disc_total, rating_source, sample_rate_hz, bitrate_kbps, channels, codec, bit_depth, "
        "title_sort, artist_sort, album_artist_sort, album_sort, "
        "musicbrainz_recording_id, musicbrainz_track_id, musicbrainz_release_id "
        "FROM tracks WHERE path = ?"));
    query.addBindValue(track.path);
    if (!query.exec() || !query.next()) {
        return;
    }
    track.trackTotal = query.value(0).toInt();
    track.discTotal = query.value(1).toInt();
    track.ratingSource = sourceFromName(query.value(2).toString());
    track.sampleRateHz = query.value(3).toInt();
    track.bitrateKbps = query.value(4).toInt();
    track.channels = query.value(5).toInt();
    track.codec = query.value(6).toString();
    track.bitDepth = query.value(7).toInt();
    track.titleSort = query.value(8).toString();
    track.artistSort = query.value(9).toString();
    track.albumArtistSort = query.value(10).toString();
    track.albumSort = query.value(11).toString();
    track.musicBrainz.recordingId = query.value(12).toString();
    track.musicBrainz.trackId = query.value(13).toString();
    track.musicBrainz.releaseId = query.value(14).toString();
}

QVector<Track> Database::searchTracksLike(const QString &text, int limit) const
{
    QVector<Track> tracks;
    const QString needle = text.trimmed();
    if (needle.isEmpty() || limit <= 0) {
        return tracks;
    }

    QSqlQuery query(m_db);
    query.prepare(trackSelectPrefix() + QStringLiteral(
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
        tracks.push_back(readTrackRow(query));
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

Database::TrackRatingSnapshot Database::trackRatingSnapshot(const QString &trackPath) const
{
    TrackRatingSnapshot snapshot;
    if (!m_db.isOpen() || trackPath.isEmpty()) {
        return snapshot;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT t.rating_0_100, utr.rating_0_100, p.status, t.musicbrainz_recording_id "
        "FROM tracks t "
        "LEFT JOIN user_track_ratings utr ON utr.track_path = t.path "
        "LEFT JOIN pending_track_rating_writes p ON p.track_path = t.path "
        "WHERE t.path = ?"));
    query.addBindValue(trackPath);
    if (!query.exec() || !query.next()) {
        return snapshot;
    }

    snapshot.found = true;
    const int scannedRating = query.value(0).isNull() ? -1 : query.value(0).toInt();
    snapshot.hasUserRating = !query.value(1).isNull();
    snapshot.userRating0To100 = snapshot.hasUserRating ? query.value(1).toInt() : -1;
    const QString pendingStatus = query.value(2).toString();
    const bool pendingDbRating = pendingStatus == QStringLiteral("pending")
        || pendingStatus == QStringLiteral("failed")
        || pendingStatus == QStringLiteral("blocked_no_writable_path");
    snapshot.effectiveRating0To100 = pendingDbRating && snapshot.hasUserRating
        ? snapshot.userRating0To100
        : (scannedRating >= 0 ? scannedRating : snapshot.userRating0To100);
    snapshot.mbRecordingId = query.value(3).toString();
    return snapshot;
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

bool Database::setTrackFlag(const QString &trackPath, TrackFlag flag, bool on)
{
    if (trackPath.isEmpty()) {
        m_lastError = QStringLiteral("Track path is required");
        return false;
    }
    const QString column = trackFlagColumn(flag);
    if (column.isEmpty()) {
        m_lastError = QStringLiteral("Invalid track flag");
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO user_track_flags(track_path, %1, updated_at) VALUES(?, ?, datetime('now')) "
        "ON CONFLICT(track_path) DO UPDATE SET %1=excluded.%1, updated_at=datetime('now')")
                      .arg(column));
    query.addBindValue(trackPath);
    query.addBindValue(on ? 1 : 0);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool Database::trackFlag(const QString &trackPath, TrackFlag flag) const
{
    const QString column = trackFlagColumn(flag);
    if (trackPath.isEmpty() || column.isEmpty()) {
        return false;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT %1 FROM user_track_flags WHERE track_path = ?").arg(column));
    query.addBindValue(trackPath);
    if (!query.exec() || !query.next()) {
        return false;
    }
    return query.value(0).toBool();
}

QSet<QString> Database::flaggedPaths(TrackFlag flag) const
{
    QSet<QString> paths;
    const QString column = trackFlagColumn(flag);
    if (column.isEmpty()) {
        return paths;
    }

    QSqlQuery query(m_db);
    if (query.exec(QStringLiteral("SELECT track_path FROM user_track_flags WHERE %1 <> 0").arg(column))) {
        while (query.next()) {
            paths.insert(query.value(0).toString());
        }
    }
    return paths;
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
    QString sql = trackSelectPrefix() + QStringLiteral("WHERE t.parent_dir = ? AND t.missing = 0");
    if (hasScanRoots(m_db)) {
        sql += QStringLiteral(" AND %1").arg(enabledLibraryRootPredicate(QStringLiteral("t"), enabledLibraryRoots()));
    }
    sql += QStringLiteral(" ORDER BY t.disc_number, t.track_number, lower(t.filename)");
    query.prepare(sql);
    query.addBindValue(cleanDirectory);
    query.exec();
    while (query.next()) {
        tracks.push_back(readTrackRow(query));
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
    QString sql = trackSelectPrefix() + QStringLiteral("WHERE t.missing = 0");
    if (hasScanRoots(m_db)) {
        sql += QStringLiteral(" AND %1").arg(enabledLibraryRootPredicate(QStringLiteral("t"), enabledLibraryRoots()));
    }
    sql += QStringLiteral(" ORDER BY RANDOM() LIMIT ?");
    query.prepare(sql);
    query.addBindValue(fetch);
    query.exec();
    while (query.next() && tracks.size() < count) {
        if (excludePaths.contains(query.value(0).toString())) {
            continue;
        }
        tracks.push_back(readTrackRow(query));
    }
    return tracks;
}

QVector<RadioCandidateRow> Database::radioCandidates(const QStringList &foldedGenres, int limit) const
{
    QVector<RadioCandidateRow> rows;
    if (foldedGenres.isEmpty() || limit <= 0) {
        return rows;
    }
    const QStringList lookupGenres = radioGenreLookupTerms(foldedGenres, genreAliases());
    if (lookupGenres.isEmpty()) {
        return rows;
    }
    // Genre join selects the pool; the correlated subquery keeps a track that
    // shares ANY seed genre, while the outer GROUP_CONCAT still returns ALL of
    // that track's folded genres (not just the matched ones).
    QString sql = QStringLiteral(
        "SELECT t.path, t.artist_name, t.title, t.album_artist_name, t.album_title, "
        "t.musicbrainz_recording_id, a.musicbrainz_release_group_id, "
        "GROUP_CONCAT(g.genre_folded, char(31)), t.original_date, t.date, "
        "t.rating_0_100, utr.rating_0_100, p.status "
        "FROM tracks t "
        "JOIN track_genres g ON g.track_id = t.id "
        "LEFT JOIN albums a ON a.id = t.album_id "
        "LEFT JOIN user_track_ratings utr ON utr.track_path = t.path "
        "LEFT JOIN pending_track_rating_writes p ON p.track_path = t.path "
        "WHERE t.missing = 0 AND t.metadata_scanned = 1 "
        "AND t.id IN (SELECT track_id FROM track_genres WHERE genre_folded IN (%1))")
        .arg(sqlPlaceholders(lookupGenres.size()));
    if (hasScanRoots(m_db)) {
        sql += QStringLiteral(" AND %1").arg(enabledLibraryRootPredicate(QStringLiteral("t"), enabledLibraryRoots()));
    }
    sql += QStringLiteral(" GROUP BY t.id LIMIT ?");

    QSqlQuery query(m_db);
    query.prepare(sql);
    for (const QString &genre : lookupGenres) {
        query.addBindValue(genre);
    }
    query.addBindValue(limit);
    if (!query.exec()) {
        return rows;
    }
    while (query.next()) {
        rows.push_back(readRadioCandidateRow(query));
    }
    return rows;
}

QVector<RadioCandidateRow> Database::radioFallbackCandidates(int limit) const
{
    QVector<RadioCandidateRow> rows;
    if (limit <= 0) {
        return rows;
    }
    // No genre to match on (seed had none): a random sample of the library, with
    // whatever genres each track does carry still folded in.
    QString sql = QStringLiteral(
        "SELECT t.path, t.artist_name, t.title, t.album_artist_name, t.album_title, "
        "t.musicbrainz_recording_id, a.musicbrainz_release_group_id, "
        "GROUP_CONCAT(g.genre_folded, char(31)), t.original_date, t.date, "
        "t.rating_0_100, utr.rating_0_100, p.status "
        "FROM tracks t "
        "LEFT JOIN track_genres g ON g.track_id = t.id "
        "LEFT JOIN albums a ON a.id = t.album_id "
        "LEFT JOIN user_track_ratings utr ON utr.track_path = t.path "
        "LEFT JOIN pending_track_rating_writes p ON p.track_path = t.path "
        "WHERE t.missing = 0 AND t.metadata_scanned = 1");
    if (hasScanRoots(m_db)) {
        sql += QStringLiteral(" AND %1").arg(enabledLibraryRootPredicate(QStringLiteral("t"), enabledLibraryRoots()));
    }
    sql += QStringLiteral(" GROUP BY t.id ORDER BY RANDOM() LIMIT ?");

    QSqlQuery query(m_db);
    query.prepare(sql);
    query.addBindValue(limit);
    if (!query.exec()) {
        return rows;
    }
    while (query.next()) {
        rows.push_back(readRadioCandidateRow(query));
    }
    return rows;
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

namespace {

// SELECT column order is shared by the bulk readers and the streaming cursor;
// the row->record mappers below index into it positionally, so keep them in
// lockstep.
constexpr char kLocalSearchSelect[] =
    "SELECT t.path, t.filename, t.title, t.artist_name, t.album_artist_name, t.album_title, "
    "t.date, t.duration_ms, t.sample_rate_hz, t.bitrate_kbps, t.channels, t.codec, "
    "COALESCE(utr.rating_0_100, t.rating_0_100), "
    "t.track_number, t.disc_number, t.file_mtime, t.file_size, t.bit_depth, "
    "t.title_sort, t.artist_sort, t.album_artist_sort, t.album_sort "
    "FROM tracks t "
    "LEFT JOIN user_track_ratings utr ON utr.track_path = t.path "
    "WHERE t.missing = 0 AND t.metadata_scanned = 1";

constexpr char kMpdSearchSelect[] =
    "SELECT uri, title, artist_name, album_artist_name, album_title, date, duration_ms, "
    "track_number, disc_number "
    "FROM mpd_tracks";

// `pool` dedups the high-repetition fields (one buffer per distinct value).
Search::SearchRecord localRowToRecord(QSqlQuery &query, QHash<QString, QString> &pool)
{
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
    rec.titleSort         = query.value(18).toString();
    rec.artistSort        = internString(pool, query.value(19).toString());
    rec.albumArtistSort   = internString(pool, query.value(20).toString());
    rec.albumSort         = internString(pool, query.value(21).toString());
    rec.source            = Search::TrackSource::Local;
    Search::foldRecordNorms(rec, &pool);
    return rec;
}

Search::SearchRecord mpdRowToRecord(QSqlQuery &query, QHash<QString, QString> &pool)
{
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
    // MPD tracks carry no sort tags; just fold the display fields.
    Search::foldRecordNorms(rec, &pool);
    return rec;
}

} // namespace

QVector<Search::SearchRecord> Database::allTracksForSearch() const
{
    QVector<Search::SearchRecord> records;
    QSqlQuery query(m_db);
    QString sql = QString::fromLatin1(kLocalSearchSelect);
    if (hasScanRoots(m_db)) {
        sql += QStringLiteral(" AND %1").arg(enabledLibraryRootPredicate(QStringLiteral("t"), enabledLibraryRoots()));
    }
    if (!query.exec(sql)) {
        return records;
    }
    QHash<QString, QString> pool;
    while (query.next()) {
        records.push_back(localRowToRecord(query, pool));
    }
    return records;
}

QVector<Search::SearchRecord> Database::allMpdTracksForSearch() const
{
    QVector<Search::SearchRecord> records;
    QSqlQuery query(m_db);
    if (!query.exec(QString::fromLatin1(kMpdSearchSelect))) {
        return records;
    }
    QHash<QString, QString> pool;
    while (query.next()) {
        records.push_back(mpdRowToRecord(query, pool));
    }
    return records;
}

Database::SearchRowSummary Database::searchRowSummary() const
{
    SearchRowSummary summary;
    {
        QString sql = QStringLiteral(
            "SELECT COUNT(*), COALESCE(MAX(file_mtime), 0) FROM tracks t "
            "WHERE t.missing = 0 AND t.metadata_scanned = 1");
        if (hasScanRoots(m_db)) {
            sql += QStringLiteral(" AND %1").arg(enabledLibraryRootPredicate(QStringLiteral("t"), enabledLibraryRoots()));
        }
        QSqlQuery query(m_db);
        if (query.exec(sql) && query.next()) {
            summary.localCount    = query.value(0).toLongLong();
            summary.localMaxMtime = query.value(1).toLongLong();
        }
    }
    {
        QSqlQuery query(m_db);
        if (query.exec(QStringLiteral("SELECT COUNT(*) FROM mpd_tracks")) && query.next()) {
            summary.mpdCount = query.value(0).toLongLong();
        }
    }
    // Enabled roots change which local tracks the predicate admits, so fold them
    // into the signature. Sort first so the hash is order-independent.
    QStringList rootPaths;
    for (const ScanRoot &root : enabledLibraryRoots()) {
        rootPaths.append(root.path);
    }
    rootPaths.sort();
    summary.rootsHash = static_cast<quint64>(qHash(rootPaths.join(QLatin1Char('\n'))));
    return summary;
}

std::unique_ptr<TrackSearchCursor> Database::beginTrackSearchStream() const
{
    QString localSql = QString::fromLatin1(kLocalSearchSelect);
    if (hasScanRoots(m_db)) {
        localSql += QStringLiteral(" AND %1").arg(enabledLibraryRootPredicate(QStringLiteral("t"), enabledLibraryRoots()));
    }
    return std::unique_ptr<TrackSearchCursor>(
        new TrackSearchCursor(m_db, std::move(localSql), QString::fromLatin1(kMpdSearchSelect)));
}

TrackSearchCursor::TrackSearchCursor(const QSqlDatabase &db, QString localSql, QString mpdSql)
    : m_query(db)
    , m_localSql(std::move(localSql))
    , m_mpdSql(std::move(mpdSql))
{
    m_query.setForwardOnly(true); // stream rows; don't buffer the whole result
}

void TrackSearchCursor::advancePhase()
{
    m_query.finish();
    m_execed = false;
    m_phase = (m_phase == Phase::Local) ? Phase::Mpd : Phase::Done;
}

bool TrackSearchCursor::nextBatch(int maxRows, QVector<Search::SearchRecord> &out)
{
    out.clear();
    if (maxRows < 1) {
        maxRows = 1;
    }
    out.reserve(maxRows);
    while (out.size() < maxRows && m_phase != Phase::Done) {
        if (!m_execed) {
            const QString &sql = (m_phase == Phase::Local) ? m_localSql : m_mpdSql;
            if (sql.isEmpty() || !m_query.exec(sql)) {
                advancePhase();
                continue;
            }
            m_execed = true;
        }
        if (m_query.next()) {
            out.push_back(m_phase == Phase::Local
                              ? localRowToRecord(m_query, m_pool)
                              : mpdRowToRecord(m_query, m_pool));
        } else {
            advancePhase(); // this phase drained; fall through to the next one
        }
    }
    // More may remain unless we've fully drained both phases and produced nothing.
    return m_phase != Phase::Done || !out.isEmpty();
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
