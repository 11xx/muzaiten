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
    void resetTimeline(qint64 positionMs = 0, qint64 durationMs = 0);
    QString uriForUrl(const QUrl &url) const;
    // Whether two profiles differ in any field that requires tearing down and
    // rebuilding the pipeline (sink/device/mode/...).  Read-ahead is not such a
    // field — it can change live without interrupting playback.
    static bool outputConfigDiffers(const PlaybackProfile &a, const PlaybackProfile &b);

    // Read-ahead: warm the OS page cache ahead of the playhead via
    // posix_fadvise so reads from slow/network mounts don't stutter.  Driven by
    // the existing poll timer; holds a private O_RDONLY fd on the current file
    // purely to issue advice (the kernel caches per-inode, so this also benefits
    // playbin's own filesrc reads).  No file data is copied into app memory.
    void startReadAhead(const QUrl &url);
    void pumpReadAhead(qint64 positionMs);
    void stopReadAhead();
    // Best-effort: pull the first read-ahead window of |url| into the page cache
    // now, then drop the fd.  Used to warm the gapless-next track's head.
    void warmFileHead(const QUrl &url) const;

    PlaybackProfile m_profile;
    GstElement *m_playbin = nullptr;
    QTimer m_pollTimer;
    mutable QMutex m_mutex;
    QString m_currentUri;
    QString m_preparedUri;
    State m_state = State::Stopped;
    qint64 m_positionMs = 0;
    qint64 m_durationMs = 0;
    State m_targetState = State::Stopped;
    bool m_gaplessAdvancePending = false;
    double m_volume = 1.0;

    // Soft-pause: the pipeline is driven to READY (releasing the audio device)
    // while we remember the position for a seamless seek on resume.
    bool m_softPaused = false;
    qint64 m_resumePositionMs = 0;
    // Pending seek to apply once the pipeline prerolls after a soft-pause resume.
    qint64 m_pendingSeekMs = -1;

    // Read-ahead state: a private fd on the current file plus the byte size and
    // the furthest offset we've already advised (the sliding watermark).
    int m_readAheadFd = -1;
    qint64 m_readAheadSize = 0;
    qint64 m_readAheadAdvised = 0;
};
