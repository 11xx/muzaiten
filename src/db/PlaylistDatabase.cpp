#include "db/PlaylistDatabase.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <utility>

namespace {

QString statusToString(PlaylistItemStatus status)
{
    switch (status) {
    case PlaylistItemStatus::Matched:     return QStringLiteral("matched");
    case PlaylistItemStatus::Missing:     return QStringLiteral("missing");
    case PlaylistItemStatus::Pending:     return QStringLiteral("pending");
    case PlaylistItemStatus::MultiMatch:  return QStringLiteral("multi");
    case PlaylistItemStatus::Approximate: return QStringLiteral("approx");
    }
    return QStringLiteral("matched");
}

PlaylistItemStatus statusFromString(const QString &value)
{
    if (value == QStringLiteral("missing")) return PlaylistItemStatus::Missing;
    if (value == QStringLiteral("pending")) return PlaylistItemStatus::Pending;
    if (value == QStringLiteral("multi"))   return PlaylistItemStatus::MultiMatch;
    if (value == QStringLiteral("approx"))  return PlaylistItemStatus::Approximate;
    return PlaylistItemStatus::Matched;
}

qint64 nowSecs()
{
    return QDateTime::currentSecsSinceEpoch();
}

// candidates column ↔ QStringList. Stored as a JSON array of paths.
QString candidatesToJson(const QStringList &paths)
{
    if (paths.isEmpty()) {
        return QString();
    }
    return QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(paths))
                                 .toJson(QJsonDocument::Compact));
}

QStringList candidatesFromJson(const QString &json)
{
    if (json.isEmpty()) {
        return {};
    }
    QStringList paths;
    const QJsonArray array = QJsonDocument::fromJson(json.toUtf8()).array();
    for (const QJsonValue &value : array) {
        const QString path = value.toString();
        if (!path.isEmpty()) {
            paths.append(path);
        }
    }
    return paths;
}

} // namespace

PlaylistDatabase::PlaylistDatabase(QString connectionName)
    : m_connectionName(std::move(connectionName))
{
}

PlaylistDatabase::~PlaylistDatabase()
{
    if (m_db.isValid()) {
        m_db.close();
    }
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool PlaylistDatabase::open(const QString &path)
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

    return migrate();
}

void PlaylistDatabase::releaseCacheMemory()
{
    if (!m_db.isOpen()) {
        return;
    }

    QSqlQuery pragma(m_db);
    pragma.exec(QStringLiteral("PRAGMA shrink_memory"));
}

bool PlaylistDatabase::migrate()
{
    QSqlQuery query(m_db);
    const QStringList statements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS playlists ("
                       "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                       "name TEXT NOT NULL, "
                       "comment TEXT, "
                       "created_at INTEGER NOT NULL, "
                       "updated_at INTEGER NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS playlist_items ("
                       "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                       "playlist_id INTEGER NOT NULL, "
                       "ordinal INTEGER NOT NULL, "
                       "track_path TEXT, "
                       "title_snapshot TEXT, "
                       "artist_snapshot TEXT, "
                       "album_snapshot TEXT, "
                       "duration_ms INTEGER NOT NULL DEFAULT 0, "
                       "added_at INTEGER NOT NULL, "
                       "modified_at INTEGER NOT NULL, "
                       "comment TEXT, "
                       "query TEXT, "
                       "status TEXT NOT NULL DEFAULT 'matched', "
                       "candidates TEXT, "
                       "external_id TEXT, "
                       "FOREIGN KEY(playlist_id) REFERENCES playlists(id) ON DELETE CASCADE)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_playlist_items_playlist ON playlist_items(playlist_id, ordinal)"),
        QStringLiteral("INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(1, datetime('now'))"),
    };
    for (const QString &statement : statements) {
        if (!query.exec(statement)) {
            m_lastError = query.lastError().text();
            return false;
        }
    }

    // v2: candidates column (JSON array of paths) for MultiMatch import items.
    // Fresh databases get it from the CREATE above; v1 databases need the ALTER.
    bool hasCandidates = false;
    if (query.exec(QStringLiteral("PRAGMA table_info(playlist_items)"))) {
        while (query.next()) {
            if (query.value(1).toString() == QStringLiteral("candidates")) {
                hasCandidates = true;
                break;
            }
        }
    }
    if (!hasCandidates
        && !query.exec(QStringLiteral("ALTER TABLE playlist_items ADD COLUMN candidates TEXT"))) {
        m_lastError = query.lastError().text();
        return false;
    }
    if (!query.exec(QStringLiteral(
            "INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(2, datetime('now'))"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    // v3: external_id column (import source id, e.g. "youtube:ID") for link-back
    // and idempotent re-import. Same column-presence self-heal as v2.
    bool hasExternalId = false;
    if (query.exec(QStringLiteral("PRAGMA table_info(playlist_items)"))) {
        while (query.next()) {
            if (query.value(1).toString() == QStringLiteral("external_id")) {
                hasExternalId = true;
                break;
            }
        }
    }
    if (!hasExternalId
        && !query.exec(QStringLiteral("ALTER TABLE playlist_items ADD COLUMN external_id TEXT"))) {
        m_lastError = query.lastError().text();
        return false;
    }
    if (!query.exec(QStringLiteral(
            "INSERT OR IGNORE INTO schema_migrations(version, applied_at) VALUES(3, datetime('now'))"))) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

void PlaylistDatabase::touchPlaylist(qint64 playlistId)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("UPDATE playlists SET updated_at = ? WHERE id = ?"));
    query.addBindValue(nowSecs());
    query.addBindValue(playlistId);
    query.exec();
}

QVector<Playlist> PlaylistDatabase::playlists() const
{
    QVector<Playlist> result;
    QSqlQuery query(m_db);
    if (!query.exec(QStringLiteral(
            "SELECT p.id, p.name, p.comment, p.created_at, p.updated_at, "
            "(SELECT COUNT(*) FROM playlist_items i WHERE i.playlist_id = p.id) "
            "FROM playlists p ORDER BY p.name COLLATE NOCASE"))) {
        m_lastError = query.lastError().text();
        return result;
    }
    while (query.next()) {
        Playlist playlist;
        playlist.id = query.value(0).toLongLong();
        playlist.name = query.value(1).toString();
        playlist.comment = query.value(2).toString();
        playlist.createdAt = query.value(3).toLongLong();
        playlist.updatedAt = query.value(4).toLongLong();
        playlist.itemCount = query.value(5).toInt();
        result.push_back(playlist);
    }
    return result;
}

Playlist PlaylistDatabase::playlist(qint64 id) const
{
    Playlist playlist;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT id, name, comment, created_at, updated_at FROM playlists WHERE id = ?"));
    query.addBindValue(id);
    if (query.exec() && query.next()) {
        playlist.id = query.value(0).toLongLong();
        playlist.name = query.value(1).toString();
        playlist.comment = query.value(2).toString();
        playlist.createdAt = query.value(3).toLongLong();
        playlist.updatedAt = query.value(4).toLongLong();
    }
    return playlist;
}

qint64 PlaylistDatabase::createPlaylist(const QString &name, const QString &comment)
{
    const qint64 now = nowSecs();
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO playlists(name, comment, created_at, updated_at) VALUES(?, ?, ?, ?)"));
    query.addBindValue(name);
    query.addBindValue(comment);
    query.addBindValue(now);
    query.addBindValue(now);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return 0;
    }
    return query.lastInsertId().toLongLong();
}

bool PlaylistDatabase::renamePlaylist(qint64 id, const QString &name)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("UPDATE playlists SET name = ?, updated_at = ? WHERE id = ?"));
    query.addBindValue(name);
    query.addBindValue(nowSecs());
    query.addBindValue(id);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool PlaylistDatabase::setPlaylistComment(qint64 id, const QString &comment)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("UPDATE playlists SET comment = ?, updated_at = ? WHERE id = ?"));
    query.addBindValue(comment);
    query.addBindValue(nowSecs());
    query.addBindValue(id);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    return true;
}

bool PlaylistDatabase::deletePlaylist(qint64 id)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM playlists WHERE id = ?"));
    query.addBindValue(id);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    // ON DELETE CASCADE removes the items (foreign_keys pragma is on).
    return true;
}

QVector<PlaylistItem> PlaylistDatabase::items(qint64 playlistId) const
{
    QVector<PlaylistItem> result;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT id, playlist_id, ordinal, track_path, title_snapshot, artist_snapshot, "
        "album_snapshot, duration_ms, added_at, modified_at, comment, query, status, candidates, external_id "
        "FROM playlist_items WHERE playlist_id = ? ORDER BY ordinal"));
    query.addBindValue(playlistId);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return result;
    }
    while (query.next()) {
        PlaylistItem item;
        item.id = query.value(0).toLongLong();
        item.playlistId = query.value(1).toLongLong();
        item.ordinal = query.value(2).toInt();
        item.trackPath = query.value(3).toString();
        item.titleSnapshot = query.value(4).toString();
        item.artistSnapshot = query.value(5).toString();
        item.albumSnapshot = query.value(6).toString();
        item.durationMs = query.value(7).toLongLong();
        item.addedAt = query.value(8).toLongLong();
        item.modifiedAt = query.value(9).toLongLong();
        item.comment = query.value(10).toString();
        item.query = query.value(11).toString();
        item.status = statusFromString(query.value(12).toString());
        item.candidatePaths = candidatesFromJson(query.value(13).toString());
        item.externalId = query.value(14).toString();
        result.push_back(item);
    }
    return result;
}

qint64 PlaylistDatabase::addItem(qint64 playlistId, const PlaylistItem &item)
{
    QSqlQuery maxQuery(m_db);
    maxQuery.prepare(QStringLiteral("SELECT COALESCE(MAX(ordinal) + 1, 0) FROM playlist_items WHERE playlist_id = ?"));
    maxQuery.addBindValue(playlistId);
    int ordinal = 0;
    if (maxQuery.exec() && maxQuery.next()) {
        ordinal = maxQuery.value(0).toInt();
    }

    const qint64 now = nowSecs();
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO playlist_items(playlist_id, ordinal, track_path, title_snapshot, "
        "artist_snapshot, album_snapshot, duration_ms, added_at, modified_at, comment, query, status, candidates, external_id) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(playlistId);
    query.addBindValue(ordinal);
    query.addBindValue(item.trackPath);
    query.addBindValue(item.titleSnapshot);
    query.addBindValue(item.artistSnapshot);
    query.addBindValue(item.albumSnapshot);
    query.addBindValue(item.durationMs);
    query.addBindValue(item.addedAt > 0 ? item.addedAt : now);
    query.addBindValue(now);
    query.addBindValue(item.comment);
    query.addBindValue(item.query);
    query.addBindValue(statusToString(item.status));
    query.addBindValue(candidatesToJson(item.candidatePaths));
    query.addBindValue(item.externalId);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return 0;
    }
    touchPlaylist(playlistId);
    return query.lastInsertId().toLongLong();
}

bool PlaylistDatabase::updateItem(const PlaylistItem &item)
{
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "UPDATE playlist_items SET track_path = ?, title_snapshot = ?, artist_snapshot = ?, "
        "album_snapshot = ?, duration_ms = ?, modified_at = ?, comment = ?, query = ?, status = ?, "
        "candidates = ?, external_id = ? WHERE id = ?"));
    query.addBindValue(item.trackPath);
    query.addBindValue(item.titleSnapshot);
    query.addBindValue(item.artistSnapshot);
    query.addBindValue(item.albumSnapshot);
    query.addBindValue(item.durationMs);
    query.addBindValue(nowSecs());
    query.addBindValue(item.comment);
    query.addBindValue(item.query);
    query.addBindValue(statusToString(item.status));
    query.addBindValue(candidatesToJson(item.candidatePaths));
    query.addBindValue(item.externalId);
    query.addBindValue(item.id);
    if (!query.exec()) {
        m_lastError = query.lastError().text();
        return false;
    }
    if (item.playlistId > 0) {
        touchPlaylist(item.playlistId);
    }
    return true;
}

bool PlaylistDatabase::removeItem(qint64 itemId)
{
    // Find the owning playlist first so its ordinals can be compacted and its
    // updated_at bumped.
    qint64 playlistId = 0;
    int ordinal = 0;
    {
        QSqlQuery lookup(m_db);
        lookup.prepare(QStringLiteral("SELECT playlist_id, ordinal FROM playlist_items WHERE id = ?"));
        lookup.addBindValue(itemId);
        if (lookup.exec() && lookup.next()) {
            playlistId = lookup.value(0).toLongLong();
            ordinal = lookup.value(1).toInt();
        } else {
            return false;
        }
    }

    QSqlQuery del(m_db);
    del.prepare(QStringLiteral("DELETE FROM playlist_items WHERE id = ?"));
    del.addBindValue(itemId);
    if (!del.exec()) {
        m_lastError = del.lastError().text();
        return false;
    }
    // Close the gap so ordinals stay contiguous.
    QSqlQuery compact(m_db);
    compact.prepare(QStringLiteral(
        "UPDATE playlist_items SET ordinal = ordinal - 1 WHERE playlist_id = ? AND ordinal > ?"));
    compact.addBindValue(playlistId);
    compact.addBindValue(ordinal);
    compact.exec();
    touchPlaylist(playlistId);
    return true;
}

int PlaylistDatabase::markItemsMissing(const QStringList &paths)
{
    if (paths.isEmpty()) {
        return 0;
    }

    constexpr int kChunk = 500;
    int changed = 0;
    const bool ownTransaction = m_db.transaction();
    for (qsizetype start = 0; start < paths.size(); start += kChunk) {
        const qsizetype count = std::min<qsizetype>(kChunk, paths.size() - start);
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral(
            "UPDATE playlist_items "
            "SET status = 'missing', modified_at = ? "
            "WHERE track_path IN (%1) AND status = 'matched'")
            .arg(QStringList(count, QStringLiteral("?")).join(QLatin1Char(','))));
        query.addBindValue(nowSecs());
        for (qsizetype i = 0; i < count; ++i) {
            query.addBindValue(paths.at(start + i));
        }
        if (!query.exec()) {
            m_lastError = query.lastError().text();
            if (ownTransaction) {
                m_db.rollback();
            }
            return changed;
        }
        changed += query.numRowsAffected();
    }
    if (ownTransaction) {
        m_db.commit();
    }
    return changed;
}

bool PlaylistDatabase::reorderItems(qint64 playlistId, const QVector<qint64> &orderedItemIds)
{
    if (!m_db.transaction()) {
        m_lastError = m_db.lastError().text();
        return false;
    }
    int ordinal = 0;
    for (const qint64 itemId : orderedItemIds) {
        QSqlQuery query(m_db);
        query.prepare(QStringLiteral(
            "UPDATE playlist_items SET ordinal = ? WHERE id = ? AND playlist_id = ?"));
        query.addBindValue(ordinal++);
        query.addBindValue(itemId);
        query.addBindValue(playlistId);
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
    touchPlaylist(playlistId);
    return true;
}
