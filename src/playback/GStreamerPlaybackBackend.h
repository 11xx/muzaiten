#pragma once

#include "playback/PlaybackBackend.h"

#include <QMutex>
#include <QTimer>

using GstElement = struct _GstElement;
using GstMessage = struct _GstMessage;

class GStreamerPlaybackBackend final : public PlaybackBackend {
    Q_OBJECT

public:
    explicit GStreamerPlaybackBackend(QObject *parent = nullptr);
    ~GStreamerPlaybackBackend() override;

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
    static void aboutToFinishCallback(GstElement *playbin, void *userData);

    void rebuildPipeline();
    void configureSink();
    void poll();
    void pollBus();
    void pollPosition();
    void handleMessage(GstMessage *message);
    void updateState(State state);
    QString uriForUrl(const QUrl &url) const;

    PlaybackProfile m_profile;
    GstElement *m_playbin = nullptr;
    QTimer m_pollTimer;
    mutable QMutex m_mutex;
    QString m_currentUri;
    QString m_preparedUri;
    State m_state = State::Stopped;
    qint64 m_positionMs = 0;
    qint64 m_durationMs = 0;
    bool m_gaplessAdvancePending = false;
    double m_volume = 1.0;
};
