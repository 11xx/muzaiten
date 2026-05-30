#pragma once

#include "playback/PlaybackTypes.h"

#include <QObject>
#include <QUrl>

class PlaybackBackend : public QObject {
    Q_OBJECT

public:
    enum class State {
        Stopped,
        Playing,
        Paused,
        Buffering,
        Error
    };
    Q_ENUM(State)

    explicit PlaybackBackend(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    ~PlaybackBackend() override = default;

    virtual void setProfile(const PlaybackProfile &profile) = 0;
    virtual void play(const QUrl &url) = 0;
    virtual void prepareNext(const QUrl &url) = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void stop() = 0;
    virtual void seek(qint64 positionMs) = 0;
    virtual void setVolume(double volume0To1) = 0;
    virtual State state() const = 0;
    virtual bool hasSource() const = 0;
    virtual qint64 position() const = 0;
    virtual qint64 duration() const = 0;
    // Called after a gapless track advance so the backend can promote the
    // prepared-track preload buffer to current and discard the old one.
    virtual void onGaplessTrackAdvanced() {}

signals:
    void stateChanged(PlaybackBackend::State state);
    void positionChanged(qint64 positionMs);
    void durationChanged(qint64 durationMs);
    void aboutToNeedNext();
    void preparedTrackStarted();
    void finished();
    void errorOccurred(QString message);
    void technicalInfoChanged(QString text);
};
