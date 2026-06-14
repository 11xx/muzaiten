#pragma once

#include "core/MetadataBlob.h"
#include "core/Track.h"
#include "playback/PlaybackBackend.h"
#include "player/PlayerCore.h"

#include <QObject>
#include <QVariantMap>

class Database;

class MprisService final : public QObject {
    Q_OBJECT

public:
    explicit MprisService(QObject *parent = nullptr);

    // Optional: lets the service enrich the current track with the full scanned
    // record (audio props, MusicBrainz, sort/reading names, blob tags) for the
    // custom CurrentTrackJson. Read-only; not owned.
    void setDatabase(const Database *database);

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
    // MPRIS LoopStatus: "None" / "Track" / "Playlist".
    QString loopStatus() const;
    bool shuffle() const;

    void setTrack(const Track &track);
    void setPlaybackState(PlaybackBackend::State state);
    void setPositionMs(qint64 positionMs);
    void setDurationMs(qint64 durationMs);
    void setVolume(double volume0To1);
    void setQueueCapabilities(bool canGoPrevious, bool canGoNext, bool canPlay);
    void setRepeatMode(RepeatMode mode);
    void setShuffleMode(ShuffleMode mode);

signals:
    void raiseRequested();
    void repeatModeRequested(RepeatMode mode);
    void shuffleModeRequested(ShuffleMode mode);
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

    const Database *m_database = nullptr;
    QString m_serviceName;
    bool m_registered = false;
    Track m_track;
    MetadataBlob::FullMetadata m_currentMeta;  // decoded blob for m_track (rich tags)
    PlaybackBackend::State m_state = PlaybackBackend::State::Stopped;
    qint64 m_positionMs = 0;
    qint64 m_durationMs = 0;
    double m_volume = 1.0;
    bool m_canGoPrevious = false;
    bool m_canGoNext = false;
    bool m_canPlay = false;
    RepeatMode m_repeatMode = RepeatMode::Off;
    ShuffleMode m_shuffleMode = ShuffleMode::Off;
};
