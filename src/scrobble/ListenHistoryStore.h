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

    // A single track playback ("spin"): how it ended, how much was actually
    // heard, where it came from, and which listening session it belonged to.
    // Recorded for every spin regardless of outcome — skips and stops are as
    // much signal as completed listens. Groundwork for the recommendation
    // engine; never leaves the machine.
    struct PlayEvent {
        qint64 id = 0;
        qint64 startedAtSecs = 0;
        qint64 endedAtSecs = 0;
        qint64 playedMs = 0;
        qint64 durationMs = 0;
        double completion = -1.0;   // <0 = unknown, stored as NULL
        QString outcome;
        bool userInitiated = false;
        QString source;
        QString shuffleMode;
        Track track;                // persisted as track_path/mb_recording_id/track_json
        QString previousTrackPath;
        QString sessionId;
    };

    // One historical listen pulled from a scrobbler service (ListenBrainz's
    // full listen export, or a Last.fm import), destined for `imported_listens`.
    // Deliberately a separate table from `listens`: imported rows carry no
    // `owed_*`/`sent_*` flags and so can never enter the scrobble-backlog drain
    // — muzaiten must never re-scrobble history it merely mirrored back.
    struct ImportedListen {
        QString source;              // 'listenbrainz' | 'lastfm'
        qint64 listenedAtSecs = 0;
        QString title;
        QString artist;
        QString album;
        QString mbRecordingId;
        QString matchedTrackPath;    // resolved library track; empty when unmatched
    };

    // One per-service, per-track playcount snapshot (MusicBee-style count sync),
    // stored in `playcount_baselines`. Artist/title are the service-side identity
    // kept verbatim so the row can be re-matched after later library rescans.
    struct PlaycountBaseline {
        QString source;
        QString artist;
        QString title;
        QString mbRecordingId;
        QString matchedTrackPath;
        qint64 count = 0;
        qint64 syncedAtSecs = 0;
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

    // Records one finalized play event. Rejected (returns -1) if the store is
    // closed, the outcome/sessionId is empty, or the track has no path.
    qint64 recordPlayEvent(const PlayEvent &event);
    // Newest-first play events, for inspection and analysis.
    QList<PlayEvent> recentPlayEvents(int limit, int offset = 0) const;
    int playEventCount() const;

    // Scrobbler backfill (Stage 0b). Imported history and per-service playcount
    // baselines live in their own tables and never touch the scrobble backlog.

    // Insert imported listens in a single transaction (INSERT OR IGNORE against
    // the UNIQUE(source, listened_at, artist, title) key). Rows whose identical
    // (listened_at, artist, title) already exists in `listens` are skipped:
    // those are the user's own scrobbles that muzaiten itself submitted to the
    // service, so re-importing them would double-count. The cross-dedup is a
    // best-effort exact match on those three columns. Returns rows inserted.
    int recordImportedListens(const QList<ImportedListen> &rows);

    // Upsert one playcount baseline keyed by (source, artist, title); an
    // existing row's count/synced_at/mbid/matched path are overwritten.
    bool upsertPlaycountBaseline(const PlaycountBaseline &row);

    QList<PlaycountBaseline> playcountBaselines(const QString &source) const;
    int importedListenCount(const QString &source) const;

    // Backfill cursor persistence, stored in the existing `meta` table (see the
    // constructor). Empty string when the key is absent.
    QString metaValue(const QString &key) const;
    void setMetaValue(const QString &key, const QString &value);

private:

    QString m_connectionName;
    QSqlDatabase m_db;
};
