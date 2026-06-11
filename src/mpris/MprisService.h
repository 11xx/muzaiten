#pragma once

#include "core/Track.h"
#include "playback/PlaybackBackend.h"

#include <QObject>
#include <QVariantMap>

class MprisService final : public QObject {
    Q_OBJECT

public:
    explicit MprisService(QObject *parent = nullptr);

    bool isRegistered() const;
    QString serviceName() const;

    QString playbackStatus() const;
    QVariantMap metadata() const;
    QString currentTrackJson() const;
    double volume() const;
    qlonglong positionUsec() const;
    qlonglong durationUsec() const;
    bool canGoNext() const;
    bool canGoPrevious() const;
    bool canPlay() const;
    bool canPause() const;
    bool canSeek() const;

    void setTrack(const Track &track);
    void setPlaybackState(PlaybackBackend::State state);
    void setPositionMs(qint64 positionMs);
    void setDurationMs(qint64 durationMs);
    void setVolume(double volume0To1);
    void setQueueCapabilities(bool canGoPrevious, bool canGoNext, bool canPlay);

signals:
    void raiseRequested();
    void nextRequested();
    void previousRequested();
    void pauseRequested();
    void playPauseRequested();
    void stopRequested();
    void playRequested();
    void seekRequested(qint64 positionMs);
    void relativeSeekRequested(qint64 offsetMs);
    void volumeRequested(double volume0To1);

private:
    void emitPropertiesChanged(const QString &interfaceName, const QVariantMap &changedProperties);
    QVariantMap buildMetadata(const Track &track) const;
    QString buildCurrentTrackJson(const Track &track) const;
    QString trackObjectPath(const Track &track) const;

    QString m_serviceName;
    bool m_registered = false;
    Track m_track;
    PlaybackBackend::State m_state = PlaybackBackend::State::Stopped;
    qint64 m_positionMs = 0;
    qint64 m_durationMs = 0;
    double m_volume = 1.0;
    bool m_canGoPrevious = false;
    bool m_canGoNext = false;
    bool m_canPlay = false;
};
