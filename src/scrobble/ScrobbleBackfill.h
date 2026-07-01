#pragma once

#include "scrobble/ListenHistoryStore.h"

#include <QHash>
#include <QObject>
#include <QString>

#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// Stage 0b scrobbler backfill worker. Pulls the user's historical listening
// data from ListenBrainz (full timestamped listen import) and Last.fm (per-track
// playcount sync via user.getTopTracks) and stores it via ListenHistoryStore for
// the future recommendation engine.
//
// Designed to live on its own worker thread (see AppCore), mirroring the
// scrobblers: it owns its QNetworkAccessManager and creates its own
// ListenHistoryStore from the history path handed to each start slot — the store
// documents one-instance-per-thread. All work is driven by network replies and
// single-shot timers; nothing blocks.
class ScrobbleBackfill final : public QObject {
    Q_OBJECT

public:
    // Immutable library snapshot for matching service listens to library tracks.
    // Built on the main thread and passed by value so the worker never touches
    // the library DB across threads.
    struct LibraryIndex {
        QHash<QString, QString> byRecordingMbid;   // recording mbid -> track path
        QHash<QString, QString> byArtistTitle;     // folded key -> track path
    };

    explicit ScrobbleBackfill(QObject *parent = nullptr);
    ~ScrobbleBackfill() override;

    // Folded "artist\ntitle" match key. Public + static so LibraryIndex builders
    // and tests construct keys identically to the matcher. Each part is
    // simplified() and case-folded.
    static QString foldedArtistTitleKey(const QString &artist, const QString &title);

    // Resolve a service listen to a library track path: recording MBID first,
    // then folded artist+title, else empty (unmatched). Static + testable.
    static QString matchTrackPath(const LibraryIndex &index, const QString &mbRecordingId,
                                  const QString &artist, const QString &title);

public slots:
    // Import full ListenBrainz history: validate token -> username, then page
    // /1/user/{name}/listens downward by max_ts from the persisted cursor (or
    // now). Cheap to re-run: INSERT OR IGNORE + cross-dedup make it incremental.
    void startListenBrainzImport(const QString &token, const QString &historyPath,
                                 const ScrobbleBackfill::LibraryIndex &index);
    // Sync Last.fm play counts: page user.getTopTracks(period=overall) and upsert
    // playcount baselines.
    void startLastFmCountSync(const QString &apiKey, const QString &username,
                              const QString &historyPath, const ScrobbleBackfill::LibraryIndex &index);
    // Cancel the running import between requests; aborts any in-flight reply and
    // emits a failed finish. No-op when idle.
    void abort();

signals:
    void progress(QString source, int processed, int inserted);
    void finished(QString source, int processed, int inserted, QString message);
    void failed(QString source, QString message);

private:
    enum class Job { None, ListenBrainz, LastFm };

    bool beginJob(Job job, const QString &historyPath);
    void endJob();
    void finishFailed(const QString &message);

    // ListenBrainz flow.
    void validateListenBrainzToken();
    void requestListenBrainzPage();
    void handleListenBrainzPage();

    // Last.fm flow.
    void requestLastFmPage();
    void handleLastFmPage();

    // Rate-limit gap (ms) before the next request, honoring LB response headers
    // with a floor; the reply is read for headers before it is deleted.
    int rateLimitDelayMs(QNetworkReply *reply) const;
    // Backoff for the Nth consecutive page failure (0-based): 10s, 60s, 5min.
    static int retryDelayMs(int attempt);
    QString sourceName() const;

    QNetworkAccessManager *m_network = nullptr;
    QTimer *m_timer = nullptr;                // single-shot; chains the next page
    QNetworkReply *m_reply = nullptr;         // in-flight request, if any
    std::unique_ptr<ListenHistoryStore> m_history;

    Job m_job = Job::None;
    bool m_aborting = false;

    QString m_token;                          // ListenBrainz
    QString m_apiKey;                         // Last.fm
    QString m_username;
    LibraryIndex m_index;

    int m_processed = 0;
    int m_inserted = 0;
    int m_pageRetries = 0;

    qint64 m_lbCursor = 0;                    // current max_ts (0 = from now)
    bool m_lbEarlyStopAllowed = false;        // re-run after completion may stop early

    int m_lfPage = 0;
    int m_lfTotalPages = 0;
};

// Passed across the worker-thread boundary via QueuedConnection.
Q_DECLARE_METATYPE(ScrobbleBackfill::LibraryIndex)
