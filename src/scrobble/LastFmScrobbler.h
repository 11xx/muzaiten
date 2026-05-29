#pragma once

#include "core/Track.h"
#include "scrobble/LastFmApi.h"

#include <QElapsedTimer>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QUrl>

class QNetworkReply;

class LastFmScrobbler final : public QObject {
    Q_OBJECT

public:
    explicit LastFmScrobbler(QObject *parent = nullptr);

public slots:
    void configure(bool enabled, const QString &apiKey, const QString &sharedSecret, const QString &sessionKey, const QString &cachePath);
    void trackStarted(const Track &track);
    void playbackStateChanged(bool playing);
    void startAuthentication(const QString &apiKey, const QString &sharedSecret);
    void finishAuthentication();

signals:
    void submissionFailed(QString message);
    void disabledAfterFailures(QString message);
    void authenticationUrlReady(QUrl url);
    void authenticationSucceeded(QString username, QString sessionKey);
    void authenticationFailed(QString message);

private slots:
    void checkListenProgress();
    void retryPending();

private:
    enum class RequestKind {
        AuthToken,
        AuthSession,
        NowPlaying,
        Scrobble,
    };

    void submitNowPlaying(const Track &track);
    void submitCompletedScrobble();
    void submitScrobbleBatch(const QList<LastFmApi::Scrobble> &submitted);
    void postParams(LastFmApi::Params params, RequestKind kind, int submittedScrobbleCount = 0);
    void handleRequestFinished(QNetworkReply *reply, RequestKind kind, int submittedScrobbleCount);
    void handleAuthTokenResponse(const LastFmApi::Response &response);
    void handleAuthSessionResponse(const LastFmApi::Response &response);
    void handleNowPlayingResponse(const LastFmApi::Response &response, const QString &transportMessage);
    void handleScrobbleResponse(const LastFmApi::Response &response, bool networkError, const QString &transportMessage, int submittedScrobbleCount);
    qint64 requiredListenMs(const Track &track) const;
    qint64 playedMs() const;
    void loadPending();
    void savePending() const;
    void cachePendingScrobble(const LastFmApi::Scrobble &scrobble);
    void removeSubmittedPrefix(int count);
    void disableScrobbling(const QString &message);
    bool hasCredentials() const;

    QNetworkAccessManager *m_network = nullptr;
    QTimer *m_progressTimer = nullptr;
    QTimer *m_retryTimer = nullptr;
    bool m_enabled = false;
    QString m_apiKey;
    QString m_sharedSecret;
    QString m_sessionKey;
    QString m_cachePath;
    Track m_currentTrack;
    bool m_hasCurrentTrack = false;
    bool m_playing = false;
    bool m_scrobbleSubmitted = false;
    bool m_scrobbleSubmissionInFlight = false;
    qint64 m_scrobbleTimestampSecs = 0;
    qint64 m_requiredMs = 0;
    qint64 m_accumulatedMs = 0;
    QElapsedTimer m_segmentTimer;
    QList<LastFmApi::Scrobble> m_pendingScrobbles;
    int m_consecutiveFailures = 0;
    QString m_pendingAuthToken;
    QString m_pendingAuthApiKey;
    QString m_pendingAuthSecret;
};
