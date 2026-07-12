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
    void stabilizeGaplessHandoff() override;
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
    // Queue/commit the prepared-track transition when the serialized
    // STREAM_START event reaches the actual audio sink. Unlike playbin's
    // upstream discovery messages, this event sits directly in front of the
    // first audible buffer.
    void queueGaplessAdvanceFromSink();
    void commitGaplessAdvance(quint64 generation);
    void commitSinkStartedGaplessAdvanceIfNeeded();
    void invalidateGaplessAdvanceLocked();
    void removeAudioSinkProbe();
    void loadUri(const QString &uri, State targetState, qint64 positionMs = -1);
    // Issue a flushing seek and arm the in-flight bookkeeping (watchdog +
    // coalescing). Callers must have ruled out the soft-pause/handoff cases.
    void issueSeek(qint64 positionMs);
    // Deterministic recovery: drop the pipeline to READY and reload the audible
    // uri at |positionMs|, preserving the prepared gapless next. Used when a
    // seek lands in the undefined about-to-finish window or never completes.
    void reloadCurrentAtPosition(qint64 positionMs, State targetState,
                                 bool preservePreparedNext = true);
    // One bounded attempt to survive a pipeline error (bad frame near EOS,
    // transient decode failure) by reloading the current source in place.
    // Returns true when a recovery was started and the error should not be
    // published; a repeat error in the same neighbourhood gives up.
    bool tryRecoverFromStreamError();
    void handleSeekWatchdogTimeout();
    void clearSeekInFlight();
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
    // The uri actually audible right now. Lags m_currentUri between
    // about-to-finish (which swaps m_currentUri to the queued next track) and
    // the audible switch committed in pollPosition; equal otherwise.
    QString m_playingUri;
    QString m_preparedUri;
    State m_state = State::Stopped;
    qint64 m_positionMs = 0;
    qint64 m_durationMs = 0;
    State m_targetState = State::Stopped;
    bool m_waitingForTargetState = false;
    bool m_gaplessAdvancePending = false;
    bool m_gaplessStartQueued = false;
    quint64 m_gaplessGeneration = 0;
    GstPad *m_audioSinkPad = nullptr;
    unsigned long m_audioSinkProbeId = 0;
    double m_volume = 1.0;
    QTimer m_transitionTimer;

    // Soft-pause: the pipeline is driven to READY (releasing the audio device)
    // while we remember the position for a seamless seek on resume.
    bool m_softPaused = false;
    qint64 m_resumePositionMs = 0;
    // Pending seek to apply once the pipeline prerolls after a soft-pause resume.
    qint64 m_pendingSeekMs = -1;

    // Seek coalescing: flushing seeks must not stack — a burst (slider scrub)
    // issued into a pipeline that hasn't finished the previous flush can wedge
    // playbin around a source switch. Only one seek is in flight at a time; the
    // latest requested target waits in m_queuedSeekMs until ASYNC_DONE, and the
    // watchdog reloads the source if ASYNC_DONE never arrives.
    bool m_seekInFlight = false;
    qint64 m_queuedSeekMs = -1;
    qint64 m_lastSeekMs = -1;
    QTimer m_seekWatchdog;

    // Error-recovery budget: where the last in-place reload happened. A second
    // pipeline error near the same spot on the same uri means the file is
    // genuinely unplayable there — publish the error instead of looping.
    // Cleared on every explicit play()/loadPaused()/stop().
    QString m_lastRecoveryUri;
    qint64 m_lastRecoveryPositionMs = -1;

    // Read-ahead state: a private fd on the current file plus the byte size and
    // the furthest offset we've already advised (the sliding watermark).
    int m_readAheadFd = -1;
    qint64 m_readAheadSize = 0;
    qint64 m_readAheadAdvised = 0;
};
