#pragma once

#include "playback/PlaybackBackend.h"

#include <QMutex>
#include <QTimer>

#include <atomic>

class OutputEndpointMonitor;
class PipeWireStreamWatch;

using GstElement = struct _GstElement;
using GstEvent = struct _GstEvent;
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
    void setGaplessStopPending(bool pending) override;
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
    bool supportsSharedEndpoint() const override { return true; }
    OutputStatus outputStatus() const override;
    QVector<OutputEndpoint> sharedEndpoints(const QString &sinkChoice) const override;
    void setDirectDeviceCheck(DirectDeviceCheck check) override { m_directCheck = std::move(check); }
    void retryDirectDevice() override;

    // The sink factory a shared transport choice ("auto", "pipewire",
    // "pulse", "alsa") resolves to with the installed plugins, or empty.
    static QString sharedSinkFactory(const QString &sinkChoice);
    // The property through which a shared sink factory names the endpoint
    // its stream asks for, or nullptr for sinks without one.
    static const char *streamTargetProperty(const QString &factory);

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
    void releaseAudioSink();
    // The endpoint the next shared stream must request, or empty to leave the
    // choice to the session manager. Never derived from the sink element's own
    // memory of where its previous stream ended up.
    QString streamTarget() const;
    // Write streamTarget() into the sink while it has no open stream: setting
    // the target on a live PulseAudio stream would move it instead.
    void applyStreamTarget();
    // Streaming-thread half of applyStreamTarget(): a format change reopens
    // the PulseAudio stream inside the sink, so release it one step early and
    // retarget before the sink reconnects.
    void retargetBeforeFormatChange(GstElement *sink, GstEvent *capsEvent);
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
    void queuePositionRestore(qint64 positionMs);
    void clearNativePositionRestore();
    // Terminal failure of a held-position restore (the transition deadline
    // passed, the source refused the seek, or the pipeline errored). The stream
    // is released like a soft pause at the latest requested position and any
    // output wait is dropped. A direct resume() retries the restore there; the
    // player's Play and Toggle in Error restart the current track from its
    // beginning instead (PlayerCore::playAt). Publishes Error and returns; the
    // caller must not publish the state it was handling.
    void failPositionRestore(const QString &message);
    // The state a terminal restore failure leaves: soft-paused in Error. Loads,
    // resumes, stops and profile rebuilds leave it.
    bool softPausedInError() const { return m_softPaused && m_state == State::Error; }
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
    // --- Preferred shared endpoint -----------------------------------------
    // Whether the profile names an endpoint this transport can request.
    bool preferredEndpointRoutable() const;
    // Present per the endpoint monitor; assumed present when nothing watches.
    bool endpointAvailable(const QString &name) const;
    // The preferred endpoint is missing and the profile waits for it.
    bool mustWaitForOutput() const;
    void updateEndpointMonitor();
    void handleEndpointAdded(const QString &name);
    void handleEndpointRemoved(const QString &name);
    // Hold the loaded source, released, until the preferred endpoint returns.
    void waitForPreferredEndpoint();
    // Where the open stream is linked right now, or empty when none is open
    // or the transport cannot tell.
    QString currentRouteNow() const;
    // Re-read the route after the sink announced a change to its stream.
    void checkCurrentRoute();
    // Account for the route the stream was seen on. A pinned stream found
    // somewhere other than where it was asked to go was moved outside the
    // app: the following streams ask for that sink at once, and the move
    // becomes the preference once confirmExternalMove() finds it deliberate.
    void noteObservedRoute(const QString &current);
    // Read the route of a stream about to close, so a move made just before
    // carries into the stream that replaces it.
    void observeRouteBeforeClose();
    void confirmExternalMove();
    void clearRouteOverride();
    // Put an open stream back on the available preferred endpoint.
    void returnToPreferred();
    // Move the open stream to |sink| without reopening it when the transport
    // allows; otherwise reopen it there at its position.
    void moveOpenStreamTo(const QString &sink);
    void setSinkTarget(const QByteArray &target);
    // m_directCheck for the profile's card; |savedDevice| is its last known
    // number. Fills in the saved label for an absent card.
    DirectDevice checkDirectDevice(const QString &savedDevice) const;
    // Re-resolve the direct card before opening it; false when it is absent.
    // A card found under another number is written into the sink, which is
    // reopened since alsasink holds its PCM from READY.
    bool prepareDirectOpen();
    // Hold a native DSD start until its device is present.
    void deferDsdStart(const QUrl &url, State targetState, const QString &label);
    void publishOutputStatus();
    void pauseInternal(bool release);
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
    bool m_gaplessStopPending = false;
    quint64 m_gaplessGeneration = 0;
    GstPad *m_audioSinkPad = nullptr;
    unsigned long m_audioSinkProbeId = 0;
    // The playbin's audio sink (reference held) and the factory it came from.
    GstElement *m_audioSink = nullptr;
    QString m_sinkFactory;
    // streamTarget() as the streaming thread reads it; guarded by m_mutex.
    QByteArray m_streamTargetUtf8;
    // Serializes this backend's writes of the sink's target, which happen on
    // the GUI thread and, for format reopens, on the streaming thread.
    QMutex m_sinkTargetMutex;
    // A card returned from exclusive use is pinned for the first stream only:
    // it dodges a transient default while the card's sink is rebuilt, and must
    // not outlive that into a lasting choice.
    QString m_handoffTarget;

    OutputEndpointMonitor *m_endpoints = nullptr;
    QString m_monitoredFactory;
    // Follows a native PipeWire stream's links; pulsesink reports its own.
    // The node name is this backend's, kept across reconnects.
    PipeWireStreamWatch *m_streamWatch = nullptr;
    QString m_nativeNodeName;
    // Playback is held until the preferred endpoint returns, resuming then
    // only when the last transport intent was to play.
    bool m_waitingForOutput = false;
    bool m_resumeOnOutput = false;
    // The wait is for the direct card rather than a shared endpoint.
    bool m_waitingForDirect = false;
    DirectDeviceCheck m_directCheck;
    QString m_directLabel;
    QString m_deferredDsdUri;
    // Where the open stream is linked, as last read.
    QString m_currentEndpoint;
    unsigned long m_currentDeviceHandler = 0;
    QTimer m_routeCheckTimer;
    // The sink a mixer moved a pinned stream to, requested by every stream
    // until it is adopted as the preference or found to be a fallback; and
    // the sink the stream left. A move is deliberate only if that sink is
    // still present a second later: a sink that vanished forced the move.
    QString m_routeOverride;
    QString m_overrideFrom;
    QTimer m_externalMoveTimer;
    // The last route seen for the current pinned stream.
    QString m_lastRoute;
    // Bumped whenever the sink is created or released. A route the streaming
    // thread queued for an older sink is dropped on delivery.
    std::atomic<quint64> m_sinkRun{0};
    OutputStatus m_publishedStatus;
    double m_volume = 1.0;
    QTimer m_transitionTimer;

    // Soft-pause: the pipeline is driven to READY (releasing the audio device)
    // while we remember the position for a seamless seek on resume.
    bool m_softPaused = false;
    qint64 m_resumePositionMs = 0;
    // Pending seek to apply once the pipeline prerolls after a soft-pause resume.
    qint64 m_pendingSeekMs = -1;
    // Native reconnects activate the buffer pool in PLAYING before seeking.
    // Keep that activation silent until the held position has prerolled.
    bool m_nativePositionRestore = false;
    bool m_nativeRestoreRelease = false;

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
