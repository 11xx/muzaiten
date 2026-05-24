#pragma once

#include "playback/PlaybackBackend.h"

class QAudioOutput;
class QMediaPlayer;

class QtPlaybackBackend final : public PlaybackBackend {
    Q_OBJECT

public:
    explicit QtPlaybackBackend(QObject *parent = nullptr);

    void setProfile(const PlaybackProfile &profile) override;
    void play(const QUrl &url) override;
    void prepareNext(const QUrl &url) override;
    void pause() override;
    void resume() override;
    void stop() override;
    void seek(qint64 positionMs) override;
    void setVolume(double volume0To1) override;
    State state() const override;
    bool hasSource() const override;
    qint64 position() const override;
    qint64 duration() const override;

private:
    QAudioOutput *m_audioOutput = nullptr;
    QMediaPlayer *m_player = nullptr;
    State m_state = State::Stopped;
};
