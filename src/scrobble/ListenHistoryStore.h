#pragma once

#include "core/Track.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVector>

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

    // Append-only local telemetry for explicit user track-rating edits. This is
    // record-only provenance data; recommendation affinity deliberately ignores it.
    struct RatingEvent {
        qint64 id = 0;
        qint64 occurredAtSecs = 0;
        Track track;                // persisted as track_path/mb_recording_id/track_json
        bool hasOldUserRating = false;
        int oldUserRating0To100 = -1;
        int oldEffectiveRating0To100 = -1;
        int newRating0To100 = -1;   // <0 = rating cleared, stored as NULL
        QString sourceSurface;
        QString playingTrackPath;
        QString playingSource;
        bool radioActive = false;
    };

    // Append-only telemetry for explicit queue row removals. These rows are
    // record-only provenance; recommendation affinity deliberately ignores them.
    struct QueueRemovalEvent {
        qint64 id = 0;
        qint64 occurredAtSecs = 0;
        Track track;                // persisted as track_path/mb_recording_id
        bool wasRadioPick = false;
        bool wasUnheard = false;
        bool radioActive = false;
    };

    struct RadioPickComponent {
        QString name;
        double value = 0.0;
    };

    // Append-only telemetry for generated radio picks. These rows are
    // record-only training data for offline analysis; recommendation affinity
    // deliberately ignores them.
    struct RadioPickEvent {
        qint64 id = 0;
        qint64 occurredAtSecs = 0;
        Track track;                // persisted as track_path/mb_recording_id
        QString sessionKind;
        int exploration0To100 = 0;
        QByteArray weightsJson;
        QVector<RadioPickComponent> components;
        double score = 0.0;
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

    // Per-track listening affinity aggregated across every history source, for
    // the radio recommender (Stage 1). Fields mirror TrackScorer::Affinity so the
    // caller can copy them across without a Qt-SQL dependency leaking into reco/.
    struct TrackAffinityRow {
        int playEvents = 0;
        int finished = 0;
        int skipped = 0;
        qint64 lastPlayedAtSecs = 0;
        int listenCount = 0;   // local listens + imported listens
        int baselineMax = 0;   // max playcount baseline across services
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

    bool recordRatingEvent(const RatingEvent &event);
    QVector<RatingEvent> ratingEvents(int limit = -1) const;
    bool recordQueueRemoval(const QueueRemovalEvent &event);
    QVector<QueueRemovalEvent> queueRemovalEvents(int limit = -1) const;
    bool recordRadioPick(const RadioPickEvent &event);
    QVector<RadioPickEvent> radioPickEvents(int limit = -1) const;

    int forgetTrackBehavior(const QStringList &paths, bool includeImportedListens = false);

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

    // Per-track affinity across play_events, listens, imported_listens and
    // playcount_baselines, keyed by library track path. Four GROUP BY queries
    // merged into one hash; tracks with no history at all are simply absent.
    QHash<QString, TrackAffinityRow> trackAffinities() const;

    // Backfill cursor persistence, stored in the existing `meta` table (see the
    // constructor). Empty string when the key is absent.
    QString metaValue(const QString &key) const;
    void setMetaValue(const QString &key, const QString &value);

private:

    QString m_connectionName;
    QSqlDatabase m_db;
};
