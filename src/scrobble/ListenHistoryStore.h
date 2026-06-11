#pragma once

#include "core/Track.h"

#include <QList>
#include <QSqlDatabase>
#include <QString>

// Always-on local listening history, independent of any scrobbling service.
// Every completed listen is recorded here (full track snapshot, timestamped to
// the second the track started playing); per-service `sent_*` flags then drive
// uploads, so a service can be enabled later and receive the whole backlog
// without ever double-submitting. Lives in the data dir (`history.sqlite`)
// because it is durable, user-owned data like the library itself.
//
// Not thread-safe: each thread (main window, scrobbler workers) opens its own
// instance; SQLite WAL + busy_timeout arbitrate concurrent access.
class ListenHistoryStore final {
public:
    struct Listen {
        qint64 id = 0;
        qint64 listenedAtSecs = 0;
        Track track;
    };

    // Service identifiers for the sent flags.
    static const QString LastFm;
    static const QString ListenBrainz;

    explicit ListenHistoryStore(const QString &path);
    ~ListenHistoryStore();

    ListenHistoryStore(const ListenHistoryStore &) = delete;
    ListenHistoryStore &operator=(const ListenHistoryStore &) = delete;

    bool isOpen() const;

    // Records a completed listen. Duplicate (timestamp, artist, title) rows are
    // ignored so replays of the same event (e.g. session restore) collapse.
    // Returns the row id, or -1 if not inserted.
    qint64 recordListen(const Track &track, qint64 listenedAtSecs);

    // Legacy pending-queue migration: inserts a listen that is still owed to
    // `unsentService` but marked sent for every other service (the old model
    // kept one queue per service, so other services either got it already or
    // were never meant to).
    qint64 importLegacyListen(const Track &track, qint64 listenedAtSecs, const QString &unsentService);

    // Oldest-first unsent listens for a service (uploads must be in order).
    QList<Listen> unsent(const QString &service, int limit) const;
    int unsentCount(const QString &service) const;
    int totalCount() const;
    void markSent(const QString &service, const QList<qint64> &ids);

private:
    qint64 insertListen(const Track &track, qint64 listenedAtSecs, bool sentLastFm, bool sentListenBrainz);

    QString m_connectionName;
    QSqlDatabase m_db;
};
