#pragma once

#include "core/Track.h"
#include "scrobble/ListenHistoryStore.h"

#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>

#include <memory>

class QNetworkReply;

// Uploads the shared local listen history (ListenHistoryStore) to one
// ListenBrainz-compatible destination and sends it rate-limited "playing now"
// updates. Listen detection itself lives in ListenTracker; this class only
// drains that destination's unsent backlog, oldest first, marking rows sent on
// acceptance so nothing is ever double-submitted.
//
// One instance per destination. Everything that can stall or fail is per
// instance (in-flight state, retry timer, backoff, failure count), so a
// destination that is rate limited, unreachable, or rejecting its token holds
// up only itself. Instances share a worker thread; they never share state.
class ListenBrainzScrobbler final : public QObject {
    Q_OBJECT

public:
    explicit ListenBrainzScrobbler(QObject *parent = nullptr);
    ~ListenBrainzScrobbler() override;

    QString destinationId() const { return m_destinationId; }

public slots:
    // Binds this scrobbler to one destination and its credentials.
    // uploadAllowed=false is the offline buffer: credentials stay configured
    // and history keeps accumulating, but nothing is sent (no listens, no
    // playing-now) until uploads are re-allowed.
    void configure(const QString &destinationId, const QString &displayName, const QString &apiRoot,
                   bool enabled, bool uploadAllowed, const QString &token, const QString &historyPath);
    void trackStarted(const Track &track);
    void resumeTrack(const Track &track, qint64 elapsedMs, bool playing);
    void playbackStateChanged(bool playing);
    // Re-send a rate-limited "playing now" for the current track if it is still
    // playing. Used when uploads are re-enabled (leaving offline mode) so the
    // service reflects what is playing now without waiting for the next track.
    void resendNowPlaying();
    // Drain unsent history listens in batches, respecting service limits.
    void uploadBacklog();
    // Check a token against a server's validate-token endpoint and report the
    // username. Destination, URL, and token are all explicit so a single
    // instance can test a destination that is not configured yet, or a URL the
    // user has typed but not saved.
    void validateToken(const QString &destinationId, const QString &apiRoot, const QString &token);

signals:
    // Every signal names its destination: with several configured, an
    // unattributed failure tells the user nothing about which one to fix.
    void submissionFailed(QString destinationId, QString message);
    void backlogProcessed(QString destinationId, int sentCount, int skippedCount, int remainingCount);
    void disabledAfterFailures(QString destinationId, QString message);
    void tokenValidated(QString destinationId, bool valid, QString username);

private slots:
    void submitPendingTrackStartPlayingNow();

private:
    enum class SubmissionKind {
        PlayingNow,
        Listen,
    };

    bool canUpload() const;
    void submitPlayingNow(const Track &track);
    void submitPlayingNowForTrackStart(const Track &track);
    void submitPayload(const QJsonObject &payload, SubmissionKind kind, const QList<qint64> &submittedIds = {});
    QJsonObject listenObject(const Track &track, qint64 listenedAt) const;
    QJsonObject metadataObject(const Track &track) const;
    QJsonObject additionalInfoObject(const Track &track) const;
    bool hasMinimumMetadata(const Track &track, bool warn = true) const;
    void handleSubmissionFinished(QNetworkReply *reply, SubmissionKind kind, QList<qint64> submittedIds);
    void disableScrobbling(const QString &message);

    QNetworkAccessManager *m_network = nullptr;
    QTimer *m_retryTimer = nullptr;
    QTimer *m_trackStartPlayingNowTimer = nullptr;
    std::unique_ptr<ListenHistoryStore> m_history;
    QString m_destinationId;
    QString m_displayName;
    QString m_apiRoot;
    bool m_enabled = false;
    bool m_uploadAllowed = true;
    QString m_token;
    QString m_historyPath;
    Track m_currentTrack;
    bool m_hasCurrentTrack = false;
    bool m_playing = false;
    bool m_listenSubmissionInFlight = false;
    qint64 m_lastPlayingNowSecs = 0;
    Track m_pendingTrackStartPlayingNow;
    bool m_hasPendingTrackStartPlayingNow = false;
    int m_consecutiveFailures = 0;
};
