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

// Uploads the shared local listen history (ListenHistoryStore) to ListenBrainz
// and sends rate-limited "playing now" updates. Listen detection itself lives
// in ListenTracker; this class only drains the unsent backlog, oldest first,
// marking rows sent on acceptance so nothing is ever double-submitted.
class ListenBrainzScrobbler final : public QObject {
    Q_OBJECT

public:
    explicit ListenBrainzScrobbler(QObject *parent = nullptr);
    ~ListenBrainzScrobbler() override;

public slots:
    // uploadAllowed=false is the offline buffer: credentials stay configured
    // and history keeps accumulating, but nothing is sent (no listens, no
    // playing-now) until uploads are re-allowed.
    void configure(bool enabled, bool uploadAllowed, const QString &token, const QString &historyPath, const QString &legacyCachePath);
    void trackStarted(const Track &track);
    void resumeTrack(const Track &track, qint64 elapsedMs, bool playing);
    void playbackStateChanged(bool playing);
    // Drain unsent history listens in batches, respecting service limits.
    void uploadBacklog();
    // Check a user token against /1/validate-token and report the username.
    void validateToken(const QString &token);

signals:
    void submissionFailed(QString message);
    void disabledAfterFailures(QString message);
    void tokenValidated(bool valid, QString username);

private slots:
    void submitPendingTrackStartPlayingNow();

private:
    enum class SubmissionKind {
        PlayingNow,
        Listen,
    };

    bool canUpload() const;
    void migrateLegacyPending(const QString &legacyCachePath);
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
