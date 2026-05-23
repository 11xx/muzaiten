#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QVector>

#include "core/Album.h"
#include "core/Artist.h"
#include "core/Track.h"

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
    QVector<Artist> albumArtists() const;
    QVector<Album> albumsForArtist(const QString &albumArtist) const;
    QVector<Track> tracksForArtist(const QString &albumArtist) const;

private:
    qint64 upsertArtist(const QString &name, const QString &sortName = {});
    qint64 upsertAlbum(const Track &track, qint64 albumArtistId);

    QString m_connectionName;
    QSqlDatabase m_db;
    QString m_lastError;
};
