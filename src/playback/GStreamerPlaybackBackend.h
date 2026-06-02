#pragma once

#include "playback/PlaybackBackend.h"

#include <QMutex>
#include <QTimer>

#include <unistd.h> // ::close

using GstElement = struct _GstElement;
using GstMessage = struct _GstMessage;

// RAII wrapper for a memfd file descriptor used to hold a preloaded track
// in RAM.  GStreamer opens the memfd via /proc/self/fd/<fd> and holds its
// own fd; closing ours here doesn't affect GStreamer's ongoing read.
struct MemFdBuffer {
    int fd = -1;

    MemFdBuffer() = default;
    ~MemFdBuffer() { clear(); }
    MemFdBuffer(const MemFdBuffer &) = delete;
    MemFdBuffer &operator=(const MemFdBuffer &) = delete;
    MemFdBuffer(MemFdBuffer &&o) noexcept : fd(o.fd) { o.fd = -1; }
    MemFdBuffer &operator=(MemFdBuffer &&o) noexcept
    {
        if (this != &o) {
            clear();
            fd = o.fd;
            o.fd = -1;
        }
        return *this;
    }

    void clear()
    {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }
};

class GStreamerPlaybackBackend final : public PlaybackBackend {
    Q_OBJECT

public:
    explicit GStreamerPlaybackBackend(QObject *parent = nullptr);
    ~GStreamerPlaybackBackend() override;

    void setProfile(const PlaybackProfile &profile) override;
    void play(const QUrl &url) override;
    void loadPaused(const QUrl &url) override;
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
    void onGaplessTrackAdvanced() override;

private:
    static void aboutToFinishCallback(GstElement *playbin, void *userData);

    void rebuildPipeline();
    bool configureSink();
    void poll();
    void pollBus();
    void pollPosition();
    void handleMessage(GstMessage *message);
    void updateState(State state);
    QString uriForUrl(const QUrl &url) const;
    // Returns a URI to use for GStreamer: either a plain file:// URI (preload
    // off) or a file:///proc/self/fd/<n> pointing at a RAM-backed memfd.
    // Fills |buf| with the memfd handle (pass m_currentPreload or
    // m_preparedPreload as appropriate).
    QString sourceUriForPlayback(const QUrl &url, MemFdBuffer &buf);

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

    // Soft-pause: the pipeline is driven to READY (releasing the audio device)
    // while we remember the position for a seamless seek on resume.
    bool m_softPaused = false;
    qint64 m_resumePositionMs = 0;
    // Pending seek to apply once the pipeline prerolls after a soft-pause resume.
    qint64 m_pendingSeekMs = -1;

    // In-RAM preload buffers: current track and the gaplessly-prepared next
    // track.  Each holds at most one open memfd; cleared on stop/play/advance.
    MemFdBuffer m_currentPreload;
    MemFdBuffer m_preparedPreload;
};
