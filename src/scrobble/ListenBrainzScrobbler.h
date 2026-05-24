#pragma once

#include "core/Track.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>

class QNetworkReply;

class ListenBrainzScrobbler final : public QObject {
    Q_OBJECT

public:
    explicit ListenBrainzScrobbler(QObject *parent = nullptr);

public slots:
    void configure(bool enabled, const QString &token, const QString &cachePath);
    void trackStarted(const Track &track);
    void playbackStateChanged(bool playing);

private slots:
    void checkListenProgress();
    void retryPending();

private:
    enum class SubmissionKind {
        PlayingNow,
        Listen,
    };

    void submitPlayingNow(const Track &track);
    void submitCompletedListen();
    void submitPayload(const QJsonObject &payload, SubmissionKind kind);
    QJsonObject listenObject(const Track &track, qint64 listenedAt) const;
    QJsonObject metadataObject(const Track &track) const;
    QJsonObject additionalInfoObject(const Track &track) const;
    bool hasMinimumMetadata(const Track &track) const;
    qint64 requiredListenMs(const Track &track) const;
    qint64 playedMs() const;
    void loadPending();
    void savePending() const;
    void cachePendingListen(const QJsonObject &listen);
    void handleSubmissionFinished(QNetworkReply *reply, SubmissionKind kind, QList<QJsonObject> submittedListens);

    QNetworkAccessManager *m_network = nullptr;
    QTimer *m_progressTimer = nullptr;
    QTimer *m_retryTimer = nullptr;
    bool m_enabled = false;
    QString m_token;
    QString m_cachePath;
    Track m_currentTrack;
    bool m_hasCurrentTrack = false;
    bool m_playing = false;
    bool m_listenSubmitted = false;
    bool m_listenSubmissionInFlight = false;
    qint64 m_listenTimestampSecs = 0;
    qint64 m_requiredMs = 0;
    qint64 m_accumulatedMs = 0;
    QElapsedTimer m_segmentTimer;
    QList<QJsonObject> m_pendingListens;
};
