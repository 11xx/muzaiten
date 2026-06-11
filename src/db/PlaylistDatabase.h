#pragma once

#include "core/Playlist.h"

#include <QSqlDatabase>
#include <QString>
#include <QVector>

// CRUD over playlists.sqlite — a standalone store for user playlists, kept apart
// from the scanned library so it survives library resets/rescans. Schema is
// versioned independently of the library (its own schema_migrations table).
class PlaylistDatabase final {
public:
    static constexpr int currentSchemaVersion = 2;

    explicit PlaylistDatabase(QString connectionName);
    ~PlaylistDatabase();

    PlaylistDatabase(const PlaylistDatabase &) = delete;
    PlaylistDatabase &operator=(const PlaylistDatabase &) = delete;

    bool open(const QString &path);
    QString lastError() const { return m_lastError; }

    // Playlists. createPlaylist returns the new row id (0 on failure).
    QVector<Playlist> playlists() const;
    Playlist playlist(qint64 id) const;
    qint64 createPlaylist(const QString &name, const QString &comment = {});
    bool renamePlaylist(qint64 id, const QString &name);
    bool setPlaylistComment(qint64 id, const QString &comment);
    bool deletePlaylist(qint64 id);

    // Items, always returned ordered by ordinal. addItem appends at the end and
    // returns the new item id; the playlist's updated_at is bumped on any change.
    QVector<PlaylistItem> items(qint64 playlistId) const;
    qint64 addItem(qint64 playlistId, const PlaylistItem &item);
    bool updateItem(const PlaylistItem &item);
    bool removeItem(qint64 itemId);
    // Reassigns ordinals 0..n-1 to itemIds in the given order (full reorder).
    bool reorderItems(qint64 playlistId, const QVector<qint64> &orderedItemIds);

private:
    bool migrate();
    void touchPlaylist(qint64 playlistId);

    QString m_connectionName;
    QSqlDatabase m_db;
    mutable QString m_lastError;
};
