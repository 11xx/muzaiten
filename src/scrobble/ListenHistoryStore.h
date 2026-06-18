#pragma once

#include "core/Track.h"

#include <QList>
#include <QSqlDatabase>
#include <QString>

// Always-on local listening history, independent of any scrobbling service.
// Every completed listen is recorded here (full track snapshot, timestamped to
// the second the track started playing). Per-service `owed_*` flags capture
// which scrobblers were enabled when the listen happened, and `sent_*` flags
// record which services have already received it. Lives in the data dir
// (`history.sqlite`) because it is durable, user-owned data like the library.
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

    struct HistoryRow {
        qint64 id = 0;
        qint64 listenedAtSecs = 0;
        Track track;
        bool owedLastFm = false;
        bool sentLastFm = false;
        bool owedListenBrainz = false;
        bool sentListenBrainz = false;
    };

    // Service identifiers for the sent flags.
    static const QString LastFm;
    static const QString ListenBrainz;

    explicit ListenHistoryStore(const QString &path);
    ~ListenHistoryStore();

    ListenHistoryStore(const ListenHistoryStore &) = delete;
    ListenHistoryStore &operator=(const ListenHistoryStore &) = delete;

    bool isOpen() const;
    void releaseCacheMemory();

    // Records a completed listen. Duplicate (timestamp, artist, title) rows are
    // ignored so replays of the same event (e.g. session restore) collapse.
    // Per-service owed flags capture which scrobblers were enabled when the
    // listen happened; disabled services must not claim old rows later.
    // Returns the row id, or -1 if not inserted.
    qint64 recordListen(const Track &track, qint64 listenedAtSecs, bool oweLastFm, bool oweListenBrainz);

    // Oldest-first unsent listens for a service (uploads must be in order).
    QList<Listen> unsent(const QString &service, int limit) const;
    int unsentCount(const QString &service) const;
    int pendingCount(const QString &service) const;
    int totalCount() const;
    void markSent(const QString &service, const QList<qint64> &ids);
    int clearPending(const QString &service);
    int markOwed(const QString &service, const QList<qint64> &ids);
    QList<HistoryRow> historyRows(int limit, int offset = 0) const;

private:

    QString m_connectionName;
    QSqlDatabase m_db;
};
