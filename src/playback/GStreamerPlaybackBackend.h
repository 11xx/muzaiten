#pragma once

#include "playback/PlaybackBackend.h"

#include <QMutex>
#include <QTimer>

using GstElement = struct _GstElement;
using GstMessage = struct _GstMessage;
using GstPad = struct _GstPad;

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
    void setOutputMode(OutputMode mode, const QString &dsdDevice = QString()) override;
    DsdSupport dsdSupport() const override;

private:
    static void aboutToFinishCallback(GstElement *playbin, void *userData);
    // Links avdemux_dsf's dynamically-added src pad to dsdconvert (userData).
    static void dsdPadAddedCallback(GstElement *demux, GstPad *pad, void *userData);
    // Builds (and makes m_playbin point at) a native DSD pipeline:
    //   filesrc ! avdemux_dsf ! dsdconvert ! alsasink device=<dsdDevice>
    // dsdconvert negotiates whatever DSD layout the DAC advertises (e.g. the
    // K5 Pro's DSDU32BE), so this is true bit-perfect passthrough. Returns false
    // (and reports via errorOccurred) if a required element is unavailable.
    bool buildDsdPipeline(const QString &filePath, QString *error);
    // Native-DSD analogue of play()/loadPaused(): tears down whatever pipeline is
    // live, builds the DSD passthrough graph for |url|, and drives it to target.
    void playDsd(const QUrl &url, State targetState);

    void rebuildPipeline();
    bool configureSink();
    void poll();
    void pollBus();
    void pollPosition();
    void handleMessage(GstMessage *message);
    void loadUri(const QString &uri, State targetState, qint64 positionMs = -1);
    void beginTargetTransition(State targetState);
    void finishTargetTransition();
    void handleTargetTransitionTimeout();
    void publishObservedState(State observedState);
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
    // The active top-level pipeline. Normally a playbin; in native-DSD mode it is
    // instead the hand-built DSD passthrough GstPipeline. Keeping the single
    // handle lets every generic operation (state, seek, bus, position) stay
    // mode-agnostic — only the few playbin-specific property pokes are guarded by
    // m_dsdActive.
    GstElement *m_playbin = nullptr;
    // True while m_playbin holds the native DSD pipeline rather than a playbin.
    bool m_dsdActive = false;
    // Output mode selected for the next play()/loadPaused(), and the ALSA device
    // to open for native DSD (empty → fall back to the profile device).
    OutputMode m_pendingOutputMode = OutputMode::Normal;
    QString m_dsdDevice;
    QTimer m_pollTimer;
    mutable QMutex m_mutex;
    QString m_currentUri;
    QString m_preparedUri;
    State m_state = State::Stopped;
    qint64 m_positionMs = 0;
    qint64 m_durationMs = 0;
    State m_targetState = State::Stopped;
    bool m_waitingForTargetState = false;
    bool m_gaplessAdvancePending = false;
    double m_volume = 1.0;
    QTimer m_transitionTimer;

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
