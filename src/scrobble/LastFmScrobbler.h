#pragma once

#include "core/Track.h"
#include "scrobble/LastFmApi.h"
#include "scrobble/ListenHistoryStore.h"

#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QUrl>

#include <memory>

class QNetworkReply;

// Uploads the shared local listen history (ListenHistoryStore) to Last.fm and
// sends rate-limited "now playing" updates, plus the browser-token auth flow.
// Listen detection itself lives in ListenTracker; this class only drains the
// unsent backlog, oldest first, marking rows sent on acceptance so nothing is
// ever double-submitted.
class LastFmScrobbler final : public QObject {
    Q_OBJECT

public:
    explicit LastFmScrobbler(QObject *parent = nullptr);
    ~LastFmScrobbler() override;

public slots:
    // uploadAllowed=false is the offline buffer: credentials stay configured
    // and history keeps accumulating, but nothing is sent (no scrobbles, no
    // now-playing) until uploads are re-allowed.
    void configure(bool enabled, bool uploadAllowed, const QString &apiKey, const QString &sharedSecret, const QString &sessionKey, const QString &historyPath, const QString &legacyCachePath);
    void trackStarted(const Track &track);
    void resumeTrack(const Track &track, qint64 elapsedMs, bool playing);
    void playbackStateChanged(bool playing);
    void startAuthentication(const QString &apiKey, const QString &sharedSecret);
    void cancelAuthentication();
    // Drain unsent history listens in batches, respecting service limits.
    void uploadBacklog();

signals:
    void submissionFailed(QString message);
    void disabledAfterFailures(QString message);
    void authenticationUrlReady(QUrl url);
    void authenticationSucceeded(QString username, QString sessionKey);
    void authenticationFailed(QString message);

private slots:
    void pollAuthSession();
    void submitPendingTrackStartNowPlaying();

private:
    enum class RequestKind {
        AuthToken,
        AuthSession,
        NowPlaying,
        Scrobble,
    };

    bool canUpload() const;
    void migrateLegacyPending(const QString &legacyCachePath);
    void submitNowPlaying(const Track &track);
    void submitNowPlayingForTrackStart(const Track &track);
    void postParams(LastFmApi::Params params, RequestKind kind, const QList<qint64> &submittedIds = {});
    void handleRequestFinished(QNetworkReply *reply, RequestKind kind, const QList<qint64> &submittedIds);
    void handleAuthTokenResponse(const LastFmApi::Response &response);
    void handleAuthSessionResponse(const LastFmApi::Response &response);
    void handleNowPlayingResponse(const LastFmApi::Response &response, const QString &transportMessage);
    void handleScrobbleResponse(const LastFmApi::Response &response, bool networkError, const QString &transportMessage, const QList<qint64> &submittedIds);
    void disableScrobbling(const QString &message);
    bool hasCredentials() const;

    QNetworkAccessManager *m_network = nullptr;
    QTimer *m_retryTimer = nullptr;
    QTimer *m_authPollTimer = nullptr;
    QTimer *m_trackStartNowPlayingTimer = nullptr;
    std::unique_ptr<ListenHistoryStore> m_history;
    bool m_enabled = false;
    bool m_uploadAllowed = true;
    QString m_apiKey;
    QString m_sharedSecret;
    QString m_sessionKey;
    QString m_historyPath;
    Track m_currentTrack;
    bool m_hasCurrentTrack = false;
    bool m_playing = false;
    bool m_scrobbleSubmissionInFlight = false;
    qint64 m_lastNowPlayingSecs = 0;
    Track m_pendingTrackStartNowPlaying;
    bool m_hasPendingTrackStartNowPlaying = false;
    int m_consecutiveFailures = 0;
    QString m_pendingAuthToken;
    QString m_pendingAuthApiKey;
    QString m_pendingAuthSecret;
    int m_authPollAttempts = 0;
    bool m_authRequestInFlight = false;
};
