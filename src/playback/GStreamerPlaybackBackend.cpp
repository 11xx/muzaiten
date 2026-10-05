#include "playback/GStreamerPlaybackBackend.h"

#include "playback/AudioDeviceControl.h"
#include "playback/OutputEndpointMonitor.h"

#include <QMutexLocker>
#include <QtGlobal>

#include <gst/audio/audio.h>
#include <gst/gst.h>

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

void initializeGStreamer()
{
    static std::once_flag once;
    std::call_once(once, []() {
        gst_init(nullptr, nullptr);
    });
}

bool factoryExists(const char *name)
{
    GstElementFactory *factory = gst_element_factory_find(name);
    if (factory == nullptr) {
        return false;
    }
    gst_object_unref(factory);
    return true;
}

// The node.name exposed by PipeWire is also exported as the PulseAudio sink
// name.  Targeting it bypasses WirePlumber's default-sink selection, which can
// briefly pick an unrelated loopback while a released card is being rebuilt.
QString sharedSinkTarget(const PlaybackProfile &profile)
{
    if (profile.mode != QStringLiteral("shared") || profile.device.isEmpty()) {
        return {};
    }
    return AudioDeviceControl::sinkNodeName(profile.device);
}

// The property through which a shared sink names the endpoint its stream asks
// for, or nullptr for sinks without one.
const char *streamTargetProperty(const QString &factory)
{
    if (factory == QStringLiteral("pulsesink")) {
        return "device";
    }
    if (factory == QStringLiteral("pipewiresink")) {
        return "target-object";
    }
    return nullptr;
}

QString transportLabel(const QString &factory)
{
    if (factory == QStringLiteral("pipewiresink")) {
        return QStringLiteral("PipeWire");
    }
    if (factory == QStringLiteral("pulsesink")) {
        return QStringLiteral("PulseAudio");
    }
    if (factory == QStringLiteral("alsasink")) {
        return QStringLiteral("ALSA");
    }
    if (factory == QStringLiteral("autoaudiosink")) {
        return QStringLiteral("automatic sink");
    }
    if (factory == QStringLiteral("fakesink")) {
        return QStringLiteral("silent");
    }
    return factory;
}

GstElement *makeSink(const PlaybackProfile &profile, QString *factory)
{
    const auto make = [factory](const char *name) -> GstElement * {
        if (!factoryExists(name)) {
            return nullptr;
        }
        GstElement *sink = gst_element_factory_make(name, nullptr);
        if (sink != nullptr) {
            *factory = QString::fromLatin1(name);
        }
        return sink;
    };

    // Demo-screen capture only emulates playback; never open a real device (it
    // would surface as a phantom stream in the user's mixer). A clock-synced
    // fakesink swallows the audio while keeping pipeline timing intact.
    if (qEnvironmentVariableIsSet("MUZAITEN_DEMO_SILENT_AUDIO")) {
        GstElement *sink = make("fakesink");
        if (sink != nullptr) {
            g_object_set(G_OBJECT(sink), "sync", TRUE, nullptr);
        }
        return sink;
    }

    // Bit-perfect always goes direct to ALSA hw: regardless of the sink field.
    if (profile.mode == QStringLiteral("bit-perfect")) {
        GstElement *sink = make("alsasink");
        if (sink != nullptr && !profile.device.isEmpty()) {
            g_object_set(G_OBJECT(sink), "device", profile.device.toUtf8().constData(), nullptr);
        }
        return sink;
    }

    // Shared sinks get their endpoint from applyStreamTarget() at each open.
    if (profile.sink == QStringLiteral("alsa")) {
        GstElement *sink = make("alsasink");
        if (sink != nullptr && !profile.device.isEmpty()) {
            g_object_set(G_OBJECT(sink), "device", profile.device.toUtf8().constData(), nullptr);
        }
        return sink;
    }
    if (profile.sink == QStringLiteral("pipewire")) {
        return make("pipewiresink");
    }
    if (profile.sink == QStringLiteral("pulse")) {
        return make("pulsesink");
    }
    if (GstElement *sink = make("pipewiresink")) {
        return sink;
    }
    if (GstElement *sink = make("pulsesink")) {
        return sink;
    }
    return make("autoaudiosink");
}

PlaybackBackend::State stateFromGst(GstState state)
{
    switch (state) {
    case GST_STATE_VOID_PENDING:
    case GST_STATE_NULL:
    case GST_STATE_READY:
        return PlaybackBackend::State::Stopped;
    case GST_STATE_PAUSED:
        return PlaybackBackend::State::Paused;
    case GST_STATE_PLAYING:
        return PlaybackBackend::State::Playing;
    }
    return PlaybackBackend::State::Stopped;
}

qint64 clockTimeToMs(gint64 value)
{
    if (value < 0 || value == static_cast<gint64>(GST_CLOCK_TIME_NONE)) {
        return 0;
    }
    return static_cast<qint64>(value / GST_MSECOND);
}

// ACCURATE, not KEY_UNIT: sample-accurate seeks land where the user dropped
// the needle, and — verified empirically — KEY_UNIT's frame-boundary estimate
// makes flacparse hit an "Internal data stream error" near the end of FLACs
// without a seektable (ffmpeg-muxed rips), where ACCURATE seeks stay clean.
// FLAC frames are all sync points, so the accuracy costs nothing there.
constexpr GstSeekFlags kSeekFlags
    = static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE);

} // namespace

GStreamerPlaybackBackend::GStreamerPlaybackBackend(QObject *parent)
    : PlaybackBackend(parent)
{
    initializeGStreamer();
    m_profile.backend = QStringLiteral("gstreamer");
    m_profile.sink = QStringLiteral("auto");
    rebuildPipeline();

    // The poll timer only runs while the pipeline is live (see updateState);
    // an idle player should not wake the process at 10 Hz.
    m_pollTimer.setInterval(100);
    connect(&m_pollTimer, &QTimer::timeout, this, &GStreamerPlaybackBackend::poll);
    m_transitionTimer.setSingleShot(true);
    m_transitionTimer.setInterval(15000);
    connect(&m_transitionTimer,
            &QTimer::timeout,
            this,
            &GStreamerPlaybackBackend::handleTargetTransitionTimeout);
    // Generous for local files, short enough that a wedged flushing seek
    // (possible around a gapless source switch) self-heals instead of leaving
    // the user staring at a frozen 0:00.
    m_seekWatchdog.setSingleShot(true);
    m_seekWatchdog.setInterval(4000);
    connect(&m_seekWatchdog,
            &QTimer::timeout,
            this,
            &GStreamerPlaybackBackend::handleSeekWatchdogTimeout);
    // A move produces a burst of stream notifications; read the route once
    // they settle.
    m_routeCheckTimer.setSingleShot(true);
    m_routeCheckTimer.setInterval(250);
    connect(&m_routeCheckTimer, &QTimer::timeout, this, &GStreamerPlaybackBackend::checkCurrentRoute);
    // Long enough for the endpoint monitor to report a removal that caused
    // the move, so an automatic fallback is never taken for a choice.
    m_externalMoveTimer.setSingleShot(true);
    m_externalMoveTimer.setInterval(1000);
    connect(&m_externalMoveTimer, &QTimer::timeout, this, &GStreamerPlaybackBackend::confirmExternalMove);
}

GStreamerPlaybackBackend::~GStreamerPlaybackBackend()
{
    removeAudioSinkProbe();
    if (m_playbin != nullptr) {
        gst_element_set_state(m_playbin, GST_STATE_NULL);
        gst_object_unref(m_playbin);
        m_playbin = nullptr;
    }
    releaseAudioSink();
    stopReadAhead();
}

void GStreamerPlaybackBackend::setProfile(const PlaybackProfile &profile)
{
    // Only rebuild the pipeline when an output-affecting field actually changes;
    // read-ahead (and any other non-output tweak) must not tear down a playing
    // pipeline.  This keeps a settings change seamless for the user.
    const bool rebuild = (m_playbin == nullptr) || outputConfigDiffers(m_profile, profile);
    m_profile = profile;
    if (rebuild) {
        m_softPaused = false;
        m_pendingSeekMs = -1;
        m_waitingForOutput = false;
        m_waitingForDirect = false;
        m_resumeOnOutput = false;
        finishTargetTransition();
        rebuildPipeline();
        // MainWindow owns transport restoration for output changes. It must
        // coordinate the rebuild with PipeWire ownership first; restoring here
        // as well races a second play/seek and can lose either position or
        // play/pause intent during shared ↔ bit-perfect transitions.
        return;
    }

    publishOutputStatus();

    // Apply the read-ahead change live against the current source.
    if (m_profile.readAheadMb <= 0) {
        stopReadAhead();
    } else if (m_readAheadFd < 0) {
        QString uri;
        {
            QMutexLocker locker(&m_mutex);
            uri = m_currentUri;
        }
        if (!uri.isEmpty()) {
            startReadAhead(QUrl::fromEncoded(uri.toUtf8()));
        }
    }
}

bool GStreamerPlaybackBackend::outputConfigDiffers(const PlaybackProfile &a,
                                                   const PlaybackProfile &b)
{
    return a.backend != b.backend
        || a.mode != b.mode
        || a.sink != b.sink
        || a.device != b.device
        || a.softwareVolume != b.softwareVolume
        || a.replayGain != b.replayGain
        || a.allowResample != b.allowResample
        || a.releaseSinkOnPause != b.releaseSinkOnPause
        || (a.mode == QStringLiteral("shared")
            && (a.sharedEndpoint != b.sharedEndpoint
                || a.sharedEndpointFallback != b.sharedEndpointFallback));
}

void GStreamerPlaybackBackend::play(const QUrl &url)
{
    if (url.isEmpty()) {
        return;
    }
    m_deferredDsdUri.clear();
    if (m_pendingOutputMode == OutputMode::NativeDsd) {
        playDsd(url, State::Playing);
        return;
    }
    // Coming back from native DSD: rebuild the playbin the normal path expects.
    // Same after a pipeline error — GStreamer only guarantees recovery through
    // NULL, and a fresh playbin is the reliable way to leave the failed state
    // behind (READY alone can keep dead elements inside uridecodebin).
    if (m_dsdActive || m_state == State::Error || m_playbin == nullptr) {
        rebuildPipeline();
    }
    if (m_playbin == nullptr) {
        return;
    }

    m_softPaused = false;
    m_pendingSeekMs = -1;
    m_lastRecoveryUri.clear();
    m_lastRecoveryPositionMs = -1;
    gst_element_set_state(m_playbin, GST_STATE_READY);
    resetTimeline();
    loadUri(uriForUrl(url), State::Playing);
    // State updates arrive over the bus, which only poll() drains — make sure
    // the timer is live before the first message lands.  Don't publish Playing
    // here: a source that fails to preroll must surface as Error, not flash
    // Playing first.  handleMessage forwards the real transition (m_targetState
    // filters the READY/PAUSED hops on the way there).
    m_pollTimer.start();
    startReadAhead(url);
}

void GStreamerPlaybackBackend::loadPaused(const QUrl &url)
{
    if (url.isEmpty()) {
        return;
    }
    m_deferredDsdUri.clear();
    if (m_pendingOutputMode == OutputMode::NativeDsd) {
        playDsd(url, State::Paused);
        return;
    }
    // As in play(): a fresh playbin after native DSD or a pipeline error.
    if (m_dsdActive || m_state == State::Error || m_playbin == nullptr) {
        rebuildPipeline();
    }
    if (m_playbin == nullptr) {
        return;
    }

    m_softPaused = false;
    m_pendingSeekMs = -1;
    m_lastRecoveryUri.clear();
    m_lastRecoveryPositionMs = -1;
    gst_element_set_state(m_playbin, GST_STATE_READY);
    resetTimeline();
    loadUri(uriForUrl(url), State::Paused);
    // As in play(): wait for the bus to confirm the preroll instead of
    // publishing Paused for a source that may turn out to be unplayable.
    startReadAhead(url);
}

void GStreamerPlaybackBackend::setOutputMode(OutputMode mode, const QString &dsdDevice)
{
    m_pendingOutputMode = mode;
    m_dsdDevice = dsdDevice;
}

PlaybackBackend::DsdSupport GStreamerPlaybackBackend::dsdSupport() const
{
    DsdSupport support;
    // The .dsf/.dsdiff demuxer comes from gst-libav; the DSD→PCM decoders too.
    // dsdconvert (gst-plugins-base) plus alsasink's audio/x-dsd support give the
    // native passthrough leg.
    const bool demux = factoryExists("avdemux_dsf");
    const bool decoder = factoryExists("avdec_dsd_lsbf") || factoryExists("avdec_dsd_msbf")
                         || factoryExists("avdec_dsd_lsbf_planar") || factoryExists("avdec_dsd_msbf_planar");
    support.pcmDecode = demux && decoder;
    support.nativePassthrough = demux && factoryExists("dsdconvert");
    return support;
}

void GStreamerPlaybackBackend::dsdPadAddedCallback(GstElement *demux, GstPad *pad, void *userData)
{
    Q_UNUSED(demux);
    auto *convert = static_cast<GstElement *>(userData);
    GstPad *sinkPad = gst_element_get_static_pad(convert, "sink");
    if (sinkPad == nullptr) {
        return;
    }
    if (!gst_pad_is_linked(sinkPad)) {
        gst_pad_link(pad, sinkPad);
    }
    gst_object_unref(sinkPad);
}

bool GStreamerPlaybackBackend::buildDsdPipeline(const QString &filePath, QString *error)
{
    GstElement *src = gst_element_factory_make("filesrc", nullptr);
    GstElement *demux = gst_element_factory_make("avdemux_dsf", nullptr);
    GstElement *convert = gst_element_factory_make("dsdconvert", nullptr);
    GstElement *sink = gst_element_factory_make("alsasink", nullptr);
    if (src == nullptr || demux == nullptr || convert == nullptr || sink == nullptr) {
        for (GstElement *e : {src, demux, convert, sink}) {
            if (e != nullptr) {
                gst_object_unref(e);
            }
        }
        if (error != nullptr) {
            *error = QStringLiteral("DSD playback needs the gst-plugins-bad and gst-libav plugins");
        }
        return false;
    }

    g_object_set(G_OBJECT(src), "location", filePath.toUtf8().constData(), nullptr);
    // Native DSD must reach the DAC's raw hw device; the shared PipeWire/Pulse
    // path cannot carry audio/x-dsd. The orchestration supplies the device.
    const QString device = !m_dsdDevice.isEmpty() ? m_dsdDevice : m_profile.device;
    if (!device.isEmpty()) {
        g_object_set(G_OBJECT(sink), "device", device.toUtf8().constData(), nullptr);
    }

    GstElement *pipeline = gst_pipeline_new("muzaiten-dsd");
    if (pipeline == nullptr) {
        for (GstElement *e : {src, demux, convert, sink}) {
            gst_object_unref(e);
        }
        if (error != nullptr) {
            *error = QStringLiteral("Failed to create the DSD pipeline");
        }
        return false;
    }

    // gst_bin_add_many takes ownership; unref(pipeline) below frees the children
    // if linking fails.
    gst_bin_add_many(GST_BIN(pipeline), src, demux, convert, sink, nullptr);
    if (!gst_element_link(src, demux) || !gst_element_link(convert, sink)) {
        gst_object_unref(pipeline);
        if (error != nullptr) {
            *error = QStringLiteral("Failed to link the DSD pipeline");
        }
        return false;
    }
    // avdemux_dsf exposes its DSD src pad only once it has parsed the header.
    g_signal_connect(demux, "pad-added", G_CALLBACK(dsdPadAddedCallback), convert);

    m_playbin = pipeline;
    m_dsdActive = true;
    return true;
}

void GStreamerPlaybackBackend::playDsd(const QUrl &url, State targetState)
{
    const QString filePath = url.toLocalFile();
    if (filePath.isEmpty()) {
        emit errorOccurred(QStringLiteral("Native DSD playback requires a local file"));
        return;
    }
    if (m_directCheck) {
        const DirectDevice device = checkDirectDevice(!m_dsdDevice.isEmpty() ? m_dsdDevice : m_profile.device);
        if (!device.available) {
            deferDsdStart(url, targetState, device.label);
            return;
        }
        m_dsdDevice = device.hwPath;
    }
    m_deferredDsdUri.clear();

    // Tear down whatever pipeline is live (playbin or an earlier DSD graph) and
    // build a fresh DSD passthrough graph for this file.
    m_softPaused = false;
    m_pendingSeekMs = -1;
    m_lastRecoveryUri.clear();
    m_lastRecoveryPositionMs = -1;
    finishTargetTransition();
    clearSeekInFlight();
    stopReadAhead();
    {
        QMutexLocker locker(&m_mutex);
        m_currentUri.clear();
        m_playingUri.clear();
        m_preparedUri.clear();
        invalidateGaplessAdvanceLocked();
    }
    removeAudioSinkProbe();
    if (m_playbin != nullptr) {
        gst_element_set_state(m_playbin, GST_STATE_NULL);
        gst_object_unref(m_playbin);
        m_playbin = nullptr;
    }
    releaseAudioSink();
    m_dsdActive = false;
    resetTimeline();

    QString error;
    if (!buildDsdPipeline(filePath, &error)) {
        m_targetState = State::Error;
        finishTargetTransition();
        updateState(State::Error);
        emit errorOccurred(error);
        return;
    }

    {
        QMutexLocker locker(&m_mutex);
        m_currentUri = uriForUrl(url);
        m_playingUri = m_currentUri;
        m_preparedUri.clear();
        invalidateGaplessAdvanceLocked();
    }
    publishOutputStatus();
    beginTargetTransition(targetState);
    gst_element_set_state(m_playbin, targetState == State::Playing ? GST_STATE_PLAYING : GST_STATE_PAUSED);
    // As with playbin, the real transition (and any preroll failure) arrives over
    // the bus; the poll timer must be running to drain it.
    m_pollTimer.start();
    startReadAhead(url);
}

void GStreamerPlaybackBackend::loadUri(const QString &uri, State targetState, qint64 positionMs)
{
    clearSeekInFlight();
    {
        QMutexLocker locker(&m_mutex);
        m_currentUri = uri;
        m_playingUri = uri;
        m_preparedUri.clear();
        invalidateGaplessAdvanceLocked();
        g_object_set(G_OBJECT(m_playbin), "uri", uri.toUtf8().constData(), nullptr);
    }

    const bool direct = m_profile.mode == QStringLiteral("bit-perfect");
    if (direct ? !prepareDirectOpen() : mustWaitForOutput()) {
        // Hold the source in READY, exactly like a soft pause, so the
        // output's return (or a later resume) opens it at this position.
        m_softPaused = true;
        m_resumePositionMs = std::max<qint64>(0, positionMs);
        m_pendingSeekMs = -1;
        m_waitingForOutput = true;
        m_waitingForDirect = direct;
        m_resumeOnOutput = targetState == State::Playing;
        finishTargetTransition();
        if (m_positionMs != m_resumePositionMs) {
            m_positionMs = m_resumePositionMs;
            emit positionChanged(m_positionMs);
        }
        updateState(m_resumeOnOutput ? State::Buffering : State::Paused);
        publishOutputStatus();
        return;
    }
    m_waitingForOutput = false;
    m_waitingForDirect = false;
    m_resumeOnOutput = false;
    applyStreamTarget();
    beginTargetTransition(targetState);
    if (targetState == State::Playing && positionMs > 0) {
        m_pendingSeekMs = positionMs;
        gst_element_set_state(m_playbin, GST_STATE_PAUSED);
    } else {
        gst_element_set_state(m_playbin, targetState == State::Playing ? GST_STATE_PLAYING : GST_STATE_PAUSED);
        if (targetState == State::Paused && positionMs > 0) {
            gst_element_seek_simple(m_playbin,
                                    GST_FORMAT_TIME,
                                    kSeekFlags,
                                    static_cast<gint64>(positionMs) * GST_MSECOND);
        }
    }
    m_pollTimer.start();
}

void GStreamerPlaybackBackend::prepareNext(const QUrl &url)
{
    QString uri;
    if (!url.isEmpty()) {
        uri = uriForUrl(url);
        // Pull the next track's head into the page cache now so the gapless
        // hand-off doesn't stall on a cold read; the persistent sliding window
        // is set up later in onGaplessTrackAdvanced().
        warmFileHead(url);
    }
    QMutexLocker locker(&m_mutex);
    m_preparedUri = uri;
}

void GStreamerPlaybackBackend::setGaplessStopPending(bool pending)
{
    QMutexLocker locker(&m_mutex);
    m_gaplessStopPending = pending;
    if (pending) {
        m_preparedUri.clear();
    }
}

void GStreamerPlaybackBackend::pause()
{
    if (m_waitingForOutput) {
        // Nothing is open; only the intent for the endpoint's return changes.
        m_resumeOnOutput = false;
        updateState(State::Paused);
        publishOutputStatus();
        return;
    }
    // Native DSD holds the ALSA device exclusively, so pausing must release it
    // (READY) just like bit-perfect — otherwise the card stays locked while idle.
    pauseInternal(m_profile.mode == QStringLiteral("bit-perfect")
                  || m_profile.releaseSinkOnPause
                  || m_dsdActive);
}

void GStreamerPlaybackBackend::pauseInternal(bool release)
{
    if (m_playbin == nullptr) {
        return;
    }

    // If the sink already accepted the successor, make it current before this
    // user action observes or mutates transport state. Otherwise a pause that
    // wins the Qt event race could reload the outgoing URI at the successor's
    // position.
    commitSinkStartedGaplessAdvanceIfNeeded();

    qint64 handoffPositionMs = -1;
    bool handoffPending = false;
    {
        QMutexLocker locker(&m_mutex);
        handoffPending = m_gaplessAdvancePending;
    }
    if (handoffPending && !m_dsdActive) {
        gint64 pos = GST_CLOCK_TIME_NONE;
        handoffPositionMs = gst_element_query_position(m_playbin, GST_FORMAT_TIME, &pos)
            ? clockTimeToMs(pos)
            : std::max<qint64>(0, m_positionMs);
        // about-to-finish has already handed playbin the successor URI. Restore
        // the still-audible source before pausing and keep that successor armed.
        reloadCurrentAtPosition(handoffPositionMs, State::Paused);
    }

    if (release) {
        // Capture position before tearing down the pipeline. The query can fail
        // transiently (e.g. mid-flush); the polled position is then the best
        // truth we have — don't let a failed query reset the resume point to 0.
        gint64 pos = GST_CLOCK_TIME_NONE;
        if (handoffPositionMs >= 0) {
            m_resumePositionMs = handoffPositionMs;
        } else if (gst_element_query_position(m_playbin, GST_FORMAT_TIME, &pos)) {
            m_resumePositionMs = clockTimeToMs(pos);
        } else {
            m_resumePositionMs = std::max<qint64>(0, m_positionMs);
        }
        m_softPaused = true;
        // READY releases the audio device but keeps the element graph intact
        // so re-preroll on resume is fast.  State messages from the
        // PLAYING→PAUSED→READY transitions are suppressed in handleMessage
        // while m_softPaused is true.
        gst_element_set_state(m_playbin, GST_STATE_READY);
        finishTargetTransition();
        updateState(m_waitingForOutput && m_resumeOnOutput ? State::Buffering : State::Paused);
    } else if (handoffPositionMs < 0) {
        beginTargetTransition(State::Paused);
        gst_element_set_state(m_playbin, GST_STATE_PAUSED);
    }
}

void GStreamerPlaybackBackend::resume()
{
    if (!m_deferredDsdUri.isEmpty()) {
        // playDsd() checks the device again and defers once more if absent.
        const QString uri = std::exchange(m_deferredDsdUri, QString());
        playDsd(QUrl::fromEncoded(uri.toUtf8()), State::Playing);
        return;
    }
    if (m_playbin == nullptr) {
        return;
    }

    if (m_softPaused) {
        const bool direct = m_profile.mode == QStringLiteral("bit-perfect") || m_dsdActive;
        bool present = true;
        if (m_dsdActive && m_directCheck) {
            const DirectDevice device = checkDirectDevice(m_dsdDevice);
            present = device.available;
            m_directLabel = device.label;
            if (present && device.hwPath != m_dsdDevice) {
                // The card is back under another number: rebuild the DSD
                // graph on it and resume at the held position.
                QString uri;
                {
                    QMutexLocker locker(&m_mutex);
                    uri = m_currentUri;
                }
                const qint64 resumeAt = m_resumePositionMs;
                playDsd(QUrl::fromEncoded(uri.toUtf8()), State::Paused);
                m_pendingSeekMs = resumeAt;
                beginTargetTransition(State::Playing);
                return;
            }
        } else if (direct) {
            present = prepareDirectOpen();
        } else {
            present = !mustWaitForOutput();
        }
        if (!present) {
            m_waitingForOutput = true;
            m_waitingForDirect = direct;
            m_resumeOnOutput = true;
            updateState(State::Buffering);
            publishOutputStatus();
            return;
        }
        m_waitingForOutput = false;
        m_waitingForDirect = false;
        m_resumeOnOutput = false;
        m_softPaused = false;
        beginTargetTransition(State::Playing);
        // Re-preroll asynchronously: READY→PAUSED.  We must NOT block the GUI
        // thread waiting for preroll — a slow/contended device could freeze the
        // UI for seconds.  Remember the resume position; handleMessage() applies
        // the seek and transitions to PLAYING once GST_MESSAGE_ASYNC_DONE shows
        // the pipeline has prerolled.
        m_pendingSeekMs = std::max<qint64>(0, m_resumePositionMs);
        applyStreamTarget();
        gst_element_set_state(m_playbin, GST_STATE_PAUSED);
        return;
    }
    beginTargetTransition(State::Playing);
    gst_element_set_state(m_playbin, GST_STATE_PLAYING);
    m_pollTimer.start();
}

void GStreamerPlaybackBackend::stop()
{
    m_deferredDsdUri.clear();
    m_softPaused = false;
    m_waitingForOutput = false;
    m_waitingForDirect = false;
    m_resumeOnOutput = false;
    m_pendingSeekMs = -1;
    m_lastRecoveryUri.clear();
    m_lastRecoveryPositionMs = -1;
    clearSeekInFlight();
    m_targetState = State::Stopped;
    finishTargetTransition();
    if (m_playbin != nullptr) {
        gst_element_set_state(m_playbin, GST_STATE_NULL);
    }
    stopReadAhead();
    // Clear all source state so hasSource() is honest after stop/end-of-queue.
    {
        QMutexLocker locker(&m_mutex);
        m_currentUri.clear();
        m_playingUri.clear();
        m_preparedUri.clear();
        invalidateGaplessAdvanceLocked();
    }
    const qint64 prevPos = m_positionMs;
    const qint64 prevDur = m_durationMs;
    m_positionMs = 0;
    m_durationMs = 0;
    if (prevPos != 0) {
        emit positionChanged(0);
    }
    if (prevDur != 0) {
        emit durationChanged(0);
    }
    updateState(State::Stopped);
    publishOutputStatus();
}

void GStreamerPlaybackBackend::seek(qint64 positionMs)
{
    if (m_playbin == nullptr) {
        return;
    }
    positionMs = std::max<qint64>(0, positionMs);
    commitSinkStartedGaplessAdvanceIfNeeded();

    // A soft-paused pipeline sits in READY, which cannot execute a seek (the
    // request would be silently dropped and the resume would snap back to the
    // old position). Retarget the resume point instead.
    if (m_softPaused) {
        m_resumePositionMs = positionMs;
        if (m_positionMs != positionMs) {
            m_positionMs = positionMs;
            emit positionChanged(m_positionMs);
        }
        return;
    }

    // A resume/load preroll is still in flight; its ASYNC_DONE handler will
    // apply m_pendingSeekMs, so just retarget it.
    if (m_pendingSeekMs >= 0) {
        m_pendingSeekMs = positionMs;
        return;
    }

    // Between about-to-finish and the next stream's start, playbin has already
    // consumed the queued uri; a flushing seek in that window is undefined and
    // can wedge the pipeline. Cancel the handoff and reload the audible track
    // at the target position (the queued next track is re-armed afterwards).
    bool handoffPending = false;
    {
        QMutexLocker locker(&m_mutex);
        handoffPending = m_gaplessAdvancePending;
    }
    if (handoffPending && !m_dsdActive) {
        reloadCurrentAtPosition(positionMs,
                                m_state == State::Paused ? State::Paused : State::Playing);
        return;
    }

    // Never stack flushing seeks (a slider scrub emits one per mouse move):
    // coalesce onto the in-flight one; ASYNC_DONE issues the latest target.
    if (m_seekInFlight) {
        m_queuedSeekMs = positionMs;
        return;
    }
    issueSeek(positionMs);
}

void GStreamerPlaybackBackend::issueSeek(qint64 positionMs)
{
    m_lastSeekMs = positionMs;
    const gboolean ok = gst_element_seek_simple(
        m_playbin,
        GST_FORMAT_TIME,
        kSeekFlags,
        static_cast<gint64>(positionMs) * GST_MSECOND);
    if (ok) {
        m_seekInFlight = true;
        m_seekWatchdog.start();
        m_pollTimer.start();
        // Rebase the published position to the target now: the UI tracks the
        // seek instantly instead of snapping back until the flush completes,
        // and a backward jump can never masquerade as the position reset the
        // gapless handoff commit watches for.
        if (m_positionMs != positionMs) {
            m_positionMs = positionMs;
            emit positionChanged(m_positionMs);
        }
    } else {
        clearSeekInFlight();
    }
}

void GStreamerPlaybackBackend::clearSeekInFlight()
{
    m_seekInFlight = false;
    m_queuedSeekMs = -1;
    m_seekWatchdog.stop();
}

bool GStreamerPlaybackBackend::tryRecoverFromStreamError()
{
    // The in-place reload path drives a playbin; the hand-built DSD graph and
    // a missing pipeline publish the error as before.
    if (m_playbin == nullptr || m_dsdActive) {
        return false;
    }
    QString uri;
    {
        QMutexLocker locker(&m_mutex);
        uri = m_playingUri.isEmpty() ? m_currentUri : m_playingUri;
    }
    if (uri.isEmpty()) {
        return false;
    }
    // An error mid-flush belongs to the seek target, not the last polled spot.
    const qint64 positionMs = m_seekInFlight && m_lastSeekMs >= 0
        ? m_lastSeekMs
        : std::max<qint64>(0, m_positionMs);
    if (uri == m_lastRecoveryUri && std::abs(positionMs - m_lastRecoveryPositionMs) < 3000) {
        return false;
    }
    m_lastRecoveryUri = uri;
    m_lastRecoveryPositionMs = positionMs;
    const State target = m_targetState == State::Paused || m_state == State::Paused
        ? State::Paused
        : State::Playing;
    reloadCurrentAtPosition(positionMs, target);
    return true;
}

void GStreamerPlaybackBackend::handleSeekWatchdogTimeout()
{
    if (!m_seekInFlight || m_playbin == nullptr) {
        return;
    }
    // ASYNC_DONE never arrived: the flushing seek wedged the pipeline (seen
    // around gapless source switches). The DSD graph never hits that race, so
    // just drop the bookkeeping there; for playbin, rebuild deterministically.
    if (m_dsdActive) {
        clearSeekInFlight();
        return;
    }
    reloadCurrentAtPosition(std::max<qint64>(0, m_lastSeekMs),
                            m_state == State::Paused ? State::Paused : State::Playing);
}

void GStreamerPlaybackBackend::reloadCurrentAtPosition(qint64 positionMs, State targetState,
                                                       bool preservePreparedNext)
{
    QString uri;
    QString nextUri;
    {
        QMutexLocker locker(&m_mutex);
        uri = m_playingUri.isEmpty() ? m_currentUri : m_playingUri;
        // Mid-handoff, the queued next track lives in m_currentUri (about-to-
        // finish already swapped it); otherwise the prepared uri still holds it.
        if (preservePreparedNext) {
            nextUri = m_gaplessAdvancePending ? m_currentUri : m_preparedUri;
        }
        invalidateGaplessAdvanceLocked();
    }
    if (uri.isEmpty()) {
        return;
    }
    clearSeekInFlight();
    // playbin only picks up a new uri from READY; this restarts the source
    // cleanly, which is exactly the point of the recovery.
    gst_element_set_state(m_playbin, GST_STATE_READY);
    loadUri(uri, targetState, positionMs);
    {
        QMutexLocker locker(&m_mutex);
        m_preparedUri = nextUri;
    }
}

void GStreamerPlaybackBackend::setVolume(double volume0To1)
{
    m_volume = std::clamp(volume0To1, 0.0, 1.0);
    // The native DSD pipeline is bit-perfect passthrough — it has no "volume"
    // element, and poking a non-existent property would warn. Volume on DSD is
    // the DAC's job.
    if (m_playbin != nullptr && !m_dsdActive && m_profile.softwareVolume) {
        g_object_set(G_OBJECT(m_playbin), "volume", m_volume, nullptr);
    }
}

PlaybackBackend::State GStreamerPlaybackBackend::state() const
{
    return m_state;
}

bool GStreamerPlaybackBackend::hasSource() const
{
    QMutexLocker locker(&m_mutex);
    return !m_currentUri.isEmpty();
}

qint64 GStreamerPlaybackBackend::position() const
{
    return m_positionMs;
}

qint64 GStreamerPlaybackBackend::duration() const
{
    return m_durationMs;
}

void GStreamerPlaybackBackend::stabilizeGaplessHandoff()
{
    // Once the sink has accepted the successor it is the audible truth, so
    // commit it before the owner mutates queue indices. An armed-only handoff
    // can still be canceled safely by reloading the outgoing source at its
    // current position; the owner will prepare the post-mutation successor.
    commitSinkStartedGaplessAdvanceIfNeeded();
    bool pending = false;
    {
        QMutexLocker locker(&m_mutex);
        pending = m_gaplessAdvancePending;
    }
    if (!pending || m_dsdActive || m_playbin == nullptr) {
        return;
    }
    gint64 position = GST_CLOCK_TIME_NONE;
    const qint64 positionMs = gst_element_query_position(m_playbin, GST_FORMAT_TIME, &position)
        ? clockTimeToMs(position)
        : std::max<qint64>(0, m_positionMs);
    reloadCurrentAtPosition(positionMs,
                            m_state == State::Paused ? State::Paused : State::Playing,
                            /*preservePreparedNext=*/false);
}

void GStreamerPlaybackBackend::aboutToFinishCallback(GstElement *playbin, void *userData)
{
    auto *self = static_cast<GStreamerPlaybackBackend *>(userData);
    QString preparedUri;
    {
        QMutexLocker locker(&self->m_mutex);
        if (!self->m_gaplessStopPending) {
            preparedUri = self->m_preparedUri;
        }
        self->m_preparedUri.clear();
        if (!preparedUri.isEmpty() && !self->m_gaplessAdvancePending) {
            ++self->m_gaplessGeneration;
            self->m_gaplessStartQueued = false;
            self->m_gaplessAdvancePending = true;
            self->m_currentUri = preparedUri;
            g_object_set(G_OBJECT(playbin), "uri", preparedUri.toUtf8().constData(), nullptr);
        } else if (!preparedUri.isEmpty()) {
            // A short or fully cached successor can trigger its own
            // about-to-finish before it reaches the sink. There is only one
            // prepared row at the PlayerCore boundary, so keep this URI for a
            // later explicit load instead of replacing the handoff that is
            // still waiting to become audible. An empty nested callback must
            // likewise leave that first handoff armed.
            self->m_preparedUri = preparedUri;
        }
    }

    QMetaObject::invokeMethod(self, [self]() {
        emit self->aboutToNeedNext();
    }, Qt::QueuedConnection);
}

void GStreamerPlaybackBackend::rebuildPipeline()
{
    clearSeekInFlight();
    stopReadAhead();
    {
        QMutexLocker locker(&m_mutex);
        m_currentUri.clear();
        m_playingUri.clear();
        m_preparedUri.clear();
        invalidateGaplessAdvanceLocked();
    }
    resetTimeline();
    removeAudioSinkProbe();
    if (m_playbin != nullptr) {
        gst_element_set_state(m_playbin, GST_STATE_NULL);
        gst_object_unref(m_playbin);
        m_playbin = nullptr;
    }
    releaseAudioSink();
    // Whatever we just tore down, we are about to build a playbin, so the active
    // pipeline is no longer the native-DSD graph.
    m_dsdActive = false;

    m_playbin = gst_element_factory_make("playbin3", "muzaiten-playbin");
    const QString playbinName = m_playbin != nullptr ? QStringLiteral("playbin3") : QStringLiteral("playbin");
    if (m_playbin == nullptr) {
        m_playbin = gst_element_factory_make("playbin", "muzaiten-playbin");
    }
    if (m_playbin == nullptr) {
        m_targetState = State::Error;
        finishTargetTransition();
        updateState(State::Error);
        emit errorOccurred(QStringLiteral("GStreamer playbin is not available"));
        return;
    }

    // We are an audio player: never let playbin render video or subtitles. Files
    // with embedded cover art (common in DSF, FLAC, M4A…) otherwise expose an
    // image/video stream that playbin auto-plugs into its own top-level output
    // window — a stray window that also throws "output window was closed" errors
    // if dismissed. Cover art is sourced separately (TagLib/ArtworkCache), so
    // clear the VIDEO and TEXT bits of GstPlayFlags. (The flag bits aren't in a
    // public header; mirror the playbin enum values.)
    {
        constexpr int kGstPlayFlagVideo = (1 << 0);
        constexpr int kGstPlayFlagText = (1 << 2);
        gint flags = 0;
        g_object_get(G_OBJECT(m_playbin), "flags", &flags, nullptr);
        flags &= ~(kGstPlayFlagVideo | kGstPlayFlagText);
        g_object_set(G_OBJECT(m_playbin), "flags", flags, nullptr);
    }

    if (!configureSink()) {
        // configureSink already reported the error and set State::Error; don't
        // advertise the pipeline as healthy or wire up gapless signalling.
        return;
    }
    setVolume(m_volume);
    g_signal_connect(m_playbin, "about-to-finish", G_CALLBACK(GStreamerPlaybackBackend::aboutToFinishCallback), this);
    updateEndpointMonitor();
    publishOutputStatus();
    emit technicalInfoChanged(QStringLiteral("GStreamer backend using %1 with %2")
                                  .arg(playbinName, transportLabel(m_sinkFactory)));
}

bool GStreamerPlaybackBackend::configureSink()
{
    QString factory;
    GstElement *sink = makeSink(m_profile, &factory);
    if (sink == nullptr) {
        // Drive into Error (matching the playbin-missing and bus-error paths)
        // rather than letting playbin silently auto-plug its default sink — a
        // bit-perfect/explicit-device profile must not fall through to shared
        // output without the user knowing.
        m_targetState = State::Error;
        finishTargetTransition();
        updateState(State::Error);
        emit errorOccurred(QStringLiteral("No usable GStreamer audio sink is available"));
        return false;
    }
    // STREAM_START is serialized with the audio and reaches this pad directly
    // before the first buffer of each source. It is therefore the authoritative
    // gapless boundary; position queries can fail or return no samples for an
    // arbitrary interval around a source switch.
    m_audioSink = GST_ELEMENT(gst_object_ref_sink(sink));
    m_sinkFactory = factory;
    // Requested by the first open, through applyStreamTarget().
    m_handoffTarget = sharedSinkTarget(m_profile);
    if (m_sinkFactory == QStringLiteral("pulsesink")) {
        // pulsesink announces every change to its stream (moves included) as
        // a current-device notification from the streaming thread.
        const auto notify = +[](GObject *, GParamSpec *, gpointer userData) {
            auto *self = static_cast<GStreamerPlaybackBackend *>(userData);
            QMetaObject::invokeMethod(self, [self]() { self->m_routeCheckTimer.start(); }, Qt::QueuedConnection);
        };
        m_currentDeviceHandler = g_signal_connect(m_audioSink, "notify::current-device", G_CALLBACK(notify), this);
    }
    m_audioSinkPad = gst_element_get_static_pad(sink, "sink");
    if (m_audioSinkPad != nullptr) {
        const auto probe = +[](GstPad *pad, GstPadProbeInfo *info, gpointer userData) -> GstPadProbeReturn {
            if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) == 0) {
                return GST_PAD_PROBE_OK;
            }
            GstEvent *event = gst_pad_probe_info_get_event(info);
            if (event == nullptr) {
                return GST_PAD_PROBE_OK;
            }
            auto *self = static_cast<GStreamerPlaybackBackend *>(userData);
            if (GST_EVENT_TYPE(event) == GST_EVENT_STREAM_START) {
                self->queueGaplessAdvanceFromSink();
            } else if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
                if (GstElement *element = gst_pad_get_parent_element(pad)) {
                    self->retargetBeforeFormatChange(element, event);
                    gst_object_unref(element);
                }
            }
            return GST_PAD_PROBE_OK;
        };
        m_audioSinkProbeId = gst_pad_add_probe(m_audioSinkPad,
                                               GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                                               probe,
                                               this,
                                               nullptr);
    }
    g_object_set(G_OBJECT(m_playbin), "audio-sink", sink, nullptr);
    return true;
}

void GStreamerPlaybackBackend::removeAudioSinkProbe()
{
    if (m_audioSinkPad == nullptr) {
        m_audioSinkProbeId = 0;
        return;
    }
    if (m_audioSinkProbeId != 0) {
        gst_pad_remove_probe(m_audioSinkPad, m_audioSinkProbeId);
    }
    gst_object_unref(m_audioSinkPad);
    m_audioSinkPad = nullptr;
    m_audioSinkProbeId = 0;
}

void GStreamerPlaybackBackend::releaseAudioSink()
{
    if (m_audioSink != nullptr) {
        if (m_currentDeviceHandler != 0) {
            g_signal_handler_disconnect(m_audioSink, m_currentDeviceHandler);
        }
        gst_object_unref(m_audioSink);
        m_audioSink = nullptr;
    }
    m_currentDeviceHandler = 0;
    m_sinkFactory.clear();
    m_handoffTarget.clear();
    m_currentEndpoint.clear();
    m_externalMoveCandidate.clear();
    m_routeCheckTimer.stop();
    m_externalMoveTimer.stop();
}

QString GStreamerPlaybackBackend::streamTarget() const
{
    if (m_profile.mode != QStringLiteral("shared")) {
        return {};
    }
    if (preferredEndpointRoutable()) {
        // A missing preferred endpoint is never requested: the stream either
        // waits for it or, by choice, takes the session manager's route.
        return endpointAvailable(m_profile.sharedEndpoint) ? m_profile.sharedEndpoint : QString();
    }
    return m_handoffTarget;
}

void GStreamerPlaybackBackend::applyStreamTarget()
{
    const QString target = streamTarget();
    m_handoffTarget.clear();
    {
        // Reopens inside a running stream follow the lasting choice; the
        // one-shot hand-off target ends with the stream it opens.
        QMutexLocker locker(&m_mutex);
        m_streamTargetUtf8 = streamTarget().toUtf8();
    }
    // pulsesink records the sink each stream ended up on, fallbacks included,
    // and would request it again on the next open. Overwrite that memory with
    // the intended target (none: the session manager decides) every time.
    setSinkTarget(target.toUtf8());
}

void GStreamerPlaybackBackend::setSinkTarget(const QByteArray &target)
{
    const char *property = streamTargetProperty(m_sinkFactory);
    if (m_audioSink == nullptr || property == nullptr) {
        return;
    }
    QMutexLocker locker(&m_sinkTargetMutex);
    g_object_set(G_OBJECT(m_audioSink), property, target.isEmpty() ? nullptr : target.constData(), nullptr);
}

void GStreamerPlaybackBackend::retargetBeforeFormatChange(GstElement *sink, GstEvent *capsEvent)
{
    GstElementFactory *factory = gst_element_get_factory(sink);
    if (factory == nullptr || !GST_IS_AUDIO_BASE_SINK(sink)
        || g_strcmp0(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)), "pulsesink") != 0) {
        return;
    }
    GstCaps *caps = nullptr;
    gst_event_parse_caps(capsEvent, &caps);
    GstAudioRingBuffer *ring = GST_AUDIO_BASE_SINK(sink)->ringbuffer;
    if (caps == nullptr || ring == nullptr) {
        return;
    }
    if (gst_audio_ring_buffer_is_acquired(ring)) {
        if (ring->spec.caps != nullptr && gst_caps_is_equal(ring->spec.caps, caps)) {
            return;  // same format: the stream stays open
        }
        // The same steps the sink's setcaps performs right after this probe,
        // taken early so no stream exists when the target is written below.
        gst_audio_ring_buffer_pause(ring);
        gst_audio_ring_buffer_activate(ring, FALSE);
        gst_audio_ring_buffer_release(ring);
    }
    QByteArray target;
    {
        QMutexLocker locker(&m_mutex);
        target = m_streamTargetUtf8;
    }
    QMutexLocker locker(&m_sinkTargetMutex);
    g_object_set(G_OBJECT(sink), "device", target.isEmpty() ? nullptr : target.constData(), nullptr);
}

QString GStreamerPlaybackBackend::sharedSinkFactory(const QString &sinkChoice)
{
    initializeGStreamer();
    const auto available = [](const char *name) {
        return factoryExists(name) ? QString::fromLatin1(name) : QString();
    };
    if (sinkChoice == QStringLiteral("alsa")) {
        return available("alsasink");
    }
    if (sinkChoice == QStringLiteral("pipewire")) {
        return available("pipewiresink");
    }
    if (sinkChoice == QStringLiteral("pulse")) {
        return available("pulsesink");
    }
    for (const char *name : {"pipewiresink", "pulsesink", "autoaudiosink"}) {
        if (factoryExists(name)) {
            return QString::fromLatin1(name);
        }
    }
    return {};
}

bool GStreamerPlaybackBackend::prepareDirectOpen()
{
    if (!m_directCheck) {
        return true;
    }
    const DirectDevice device = checkDirectDevice(m_profile.device);
    m_directLabel = device.label;
    if (!device.available) {
        return false;
    }
    if (m_audioSink != nullptr && m_playbin != nullptr && m_sinkFactory == QStringLiteral("alsasink")) {
        gchar *current = nullptr;
        g_object_get(G_OBJECT(m_audioSink), "device", &current, nullptr);
        const bool moved = QString::fromUtf8(current) != device.hwPath;
        g_free(current);
        if (moved) {
            gst_element_set_state(m_playbin, GST_STATE_NULL);
            g_object_set(G_OBJECT(m_audioSink), "device", device.hwPath.toUtf8().constData(), nullptr);
        }
    }
    m_profile.device = device.hwPath;
    return true;
}

PlaybackBackend::DirectDevice GStreamerPlaybackBackend::checkDirectDevice(const QString &savedDevice) const
{
    DirectDevice device = m_directCheck(m_profile.deviceId, savedDevice);
    if (device.label.isEmpty()) {
        device.label = !m_profile.deviceLabel.isEmpty() ? m_profile.deviceLabel
            : (!m_profile.deviceId.isEmpty() ? m_profile.deviceId : savedDevice);
    }
    return device;
}

void GStreamerPlaybackBackend::deferDsdStart(const QUrl &url, State targetState, const QString &label)
{
    // Leave nothing open on the old number, as playDsd() would.
    finishTargetTransition();
    clearSeekInFlight();
    stopReadAhead();
    removeAudioSinkProbe();
    if (m_playbin != nullptr) {
        gst_element_set_state(m_playbin, GST_STATE_NULL);
        gst_object_unref(m_playbin);
        m_playbin = nullptr;
    }
    releaseAudioSink();
    m_dsdActive = false;
    m_softPaused = false;
    m_pendingSeekMs = -1;
    resetTimeline();
    {
        QMutexLocker locker(&m_mutex);
        m_currentUri = uriForUrl(url);
        m_playingUri = m_currentUri;
        m_preparedUri.clear();
        invalidateGaplessAdvanceLocked();
    }
    m_deferredDsdUri = uriForUrl(url);
    m_directLabel = label;
    m_waitingForOutput = true;
    m_waitingForDirect = true;
    m_resumeOnOutput = targetState == State::Playing;
    updateState(m_resumeOnOutput ? State::Buffering : State::Paused);
    publishOutputStatus();
}

void GStreamerPlaybackBackend::retryDirectDevice()
{
    if (m_waitingForOutput && m_waitingForDirect && m_resumeOnOutput) {
        resume();
    }
}

QVector<OutputEndpoint> GStreamerPlaybackBackend::sharedEndpoints(const QString &sinkChoice) const
{
    const QString factory = sharedSinkFactory(sinkChoice);
    if (m_endpoints != nullptr && m_endpoints->running() && m_monitoredFactory == factory) {
        return m_endpoints->endpoints();
    }
    QVector<OutputEndpoint> endpoints = OutputEndpointMonitor::probe(factory);
    std::sort(endpoints.begin(), endpoints.end(), [](const OutputEndpoint &a, const OutputEndpoint &b) {
        return a.label.localeAwareCompare(b.label) < 0;
    });
    return endpoints;
}

bool GStreamerPlaybackBackend::preferredEndpointRoutable() const
{
    return m_profile.mode == QStringLiteral("shared") && !m_dsdActive
        && !m_profile.sharedEndpoint.isEmpty()
        && streamTargetProperty(m_sinkFactory) != nullptr;
}

bool GStreamerPlaybackBackend::endpointAvailable(const QString &name) const
{
    return m_endpoints == nullptr || !m_endpoints->running() || m_endpoints->contains(name);
}

bool GStreamerPlaybackBackend::mustWaitForOutput() const
{
    return preferredEndpointRoutable() && !m_profile.sharedEndpointFallback
        && !endpointAvailable(m_profile.sharedEndpoint);
}

void GStreamerPlaybackBackend::updateEndpointMonitor()
{
    // Only a preferred endpoint needs watching; the system route is the
    // session manager's to follow.
    const bool wanted = m_profile.mode == QStringLiteral("shared")
        && !m_profile.sharedEndpoint.isEmpty()
        && !OutputEndpointMonitor::providerFor(m_sinkFactory).isEmpty();
    if (!wanted) {
        if (m_endpoints != nullptr) {
            m_endpoints->stop();
        }
        m_monitoredFactory.clear();
        return;
    }
    if (m_endpoints == nullptr) {
        m_endpoints = new OutputEndpointMonitor(this);
        connect(m_endpoints, &OutputEndpointMonitor::endpointAdded,
                this, &GStreamerPlaybackBackend::handleEndpointAdded);
        connect(m_endpoints, &OutputEndpointMonitor::endpointRemoved,
                this, &GStreamerPlaybackBackend::handleEndpointRemoved);
    }
    if (!m_endpoints->running() || m_monitoredFactory != m_sinkFactory) {
        m_monitoredFactory = m_endpoints->start(m_sinkFactory) ? m_sinkFactory : QString();
    }
}

void GStreamerPlaybackBackend::handleEndpointAdded(const QString &name)
{
    if (name != m_profile.sharedEndpoint || !preferredEndpointRoutable()) {
        publishOutputStatus();
        return;
    }
    {
        QMutexLocker locker(&m_mutex);
        m_streamTargetUtf8 = name.toUtf8();
    }
    if (m_waitingForOutput) {
        m_waitingForOutput = false;
        m_waitingForDirect = false;
        const bool resumePlaying = std::exchange(m_resumeOnOutput, false);
        if (resumePlaying) {
            resume();
        } else {
            updateState(State::Paused);
        }
        publishOutputStatus();
        return;
    }
    // A fallback stream may be playing elsewhere: bring it back. pulsesink
    // moves its live stream when given a target (and only records it when
    // none is open); pipewiresink reads its target only when it connects.
    if (m_sinkFactory == QStringLiteral("pulsesink")) {
        setSinkTarget(name.toUtf8());
    } else if (m_playbin != nullptr && !m_softPaused
               && (m_state == State::Playing || m_state == State::Paused)) {
        reloadCurrentAtPosition(m_positionMs, m_state);
    }
    publishOutputStatus();
}

void GStreamerPlaybackBackend::handleEndpointRemoved(const QString &name)
{
    if (name == m_profile.sharedEndpoint) {
        ++m_preferredRemovals;
    }
    if (name != m_profile.sharedEndpoint || !preferredEndpointRoutable()) {
        publishOutputStatus();
        return;
    }
    {
        // Streams the sink reopens from now on take the system route.
        QMutexLocker locker(&m_mutex);
        m_streamTargetUtf8.clear();
    }
    if (!m_profile.sharedEndpointFallback) {
        waitForPreferredEndpoint();
    }
    publishOutputStatus();
}

void GStreamerPlaybackBackend::waitForPreferredEndpoint()
{
    if (m_waitingForOutput || m_playbin == nullptr || !hasSource()) {
        return;
    }
    if (m_softPaused) {
        // Paused and released already; a resume will wait for the endpoint.
        m_waitingForOutput = true;
        m_resumeOnOutput = false;
        return;
    }
    const bool wasPlaying = m_state == State::Playing
        || (m_waitingForTargetState && m_targetState == State::Playing);
    m_waitingForOutput = true;
    m_resumeOnOutput = wasPlaying;
    pauseInternal(/*release=*/true);
}

void GStreamerPlaybackBackend::checkCurrentRoute()
{
    QString current;
    if (m_audioSink != nullptr && m_sinkFactory == QStringLiteral("pulsesink") && !m_softPaused) {
        GstState state = GST_STATE_NULL;
        gst_element_get_state(m_audioSink, &state, nullptr, 0);
        if (state >= GST_STATE_PAUSED) {
            gchar *device = nullptr;
            g_object_get(G_OBJECT(m_audioSink), "current-device", &device, nullptr);
            current = QString::fromUtf8(device);
            g_free(device);
        }
    }
    m_currentEndpoint = current;

    QByteArray requested;
    {
        QMutexLocker locker(&m_mutex);
        requested = m_streamTargetUtf8;
    }
    const QString preferred = m_profile.sharedEndpoint;
    if (preferredEndpointRoutable() && !current.isEmpty() && current != preferred
        && requested == preferred.toUtf8() && endpointAvailable(preferred)) {
        if (m_externalMoveCandidate != current) {
            m_externalMoveCandidate = current;
            m_candidateRemovals = m_preferredRemovals;
            m_externalMoveTimer.start();
        }
    } else {
        m_externalMoveCandidate.clear();
        m_externalMoveTimer.stop();
    }
    publishOutputStatus();
}

void GStreamerPlaybackBackend::confirmExternalMove()
{
    const QString candidate = std::exchange(m_externalMoveCandidate, QString());
    if (candidate.isEmpty()) {
        return;
    }
    checkCurrentRoute();
    m_externalMoveCandidate.clear();
    m_externalMoveTimer.stop();
    const QString preferred = m_profile.sharedEndpoint;
    if (m_currentEndpoint != candidate || m_preferredRemovals != m_candidateRemovals
        || !preferredEndpointRoutable() || !endpointAvailable(preferred)
        || (m_endpoints != nullptr && !m_endpoints->contains(candidate))) {
        return;
    }
    // The preferred endpoint stayed present, so nothing forced this move:
    // someone chose the other output. Request it from now on.
    m_profile.sharedEndpoint = candidate;
    m_profile.sharedEndpointLabel = m_endpoints != nullptr ? m_endpoints->label(candidate) : QString();
    {
        QMutexLocker locker(&m_mutex);
        m_streamTargetUtf8 = candidate.toUtf8();
    }
    emit sharedEndpointChosenExternally(candidate, m_profile.sharedEndpointLabel);
    publishOutputStatus();
}

OutputStatus GStreamerPlaybackBackend::outputStatus() const
{
    OutputStatus status;
    if (m_dsdActive || m_waitingForDirect || m_profile.mode == QStringLiteral("bit-perfect")) {
        status.transport = QStringLiteral("ALSA");
        status.route = m_waitingForOutput && m_waitingForDirect ? OutputStatus::Route::Waiting
                                                               : OutputStatus::Route::Direct;
        if (status.route == OutputStatus::Route::Waiting) {
            status.preferredLabel = m_directLabel;
        }
        return status;
    }
    status.transport = transportLabel(m_sinkFactory);
    if (m_profile.sink == QStringLiteral("auto") && m_sinkFactory == QStringLiteral("pulsesink")) {
        status.transportNote = QStringLiteral("the GStreamer PipeWire plugin is not installed");
    } else if (m_profile.sink == QStringLiteral("auto") && m_sinkFactory == QStringLiteral("autoaudiosink")) {
        status.transportNote = QStringLiteral("no GStreamer PipeWire or PulseAudio plugin is installed");
    }
    status.currentEndpoint = m_currentEndpoint;
    if (m_endpoints != nullptr) {
        status.currentLabel = m_endpoints->label(m_currentEndpoint);
    }
    if (m_profile.sharedEndpoint.isEmpty()) {
        status.route = OutputStatus::Route::System;
        return status;
    }
    status.preferredEndpoint = m_profile.sharedEndpoint;
    status.preferredLabel = m_profile.sharedEndpointLabel;
    if (m_endpoints != nullptr && m_endpoints->contains(m_profile.sharedEndpoint)) {
        status.preferredLabel = m_endpoints->label(m_profile.sharedEndpoint);
    }
    if (!preferredEndpointRoutable()) {
        status.route = OutputStatus::Route::Unsupported;
        return status;
    }
    status.preferredAvailable = endpointAvailable(m_profile.sharedEndpoint);
    if (m_waitingForOutput || (!status.preferredAvailable && !m_profile.sharedEndpointFallback)) {
        status.route = OutputStatus::Route::Waiting;
    } else if (!status.preferredAvailable) {
        status.route = OutputStatus::Route::Fallback;
    } else {
        status.route = OutputStatus::Route::Preferred;
    }
    return status;
}

void GStreamerPlaybackBackend::publishOutputStatus()
{
    const OutputStatus status = outputStatus();
    if (status == m_publishedStatus) {
        return;
    }
    m_publishedStatus = status;
    emit outputStatusChanged();
}

void GStreamerPlaybackBackend::queueGaplessAdvanceFromSink()
{
    quint64 generation = 0;
    {
        QMutexLocker locker(&m_mutex);
        if (!m_gaplessAdvancePending || m_gaplessStartQueued) {
            return;
        }
        m_gaplessStartQueued = true;
        generation = m_gaplessGeneration;
    }
    // The pad probe runs on GStreamer's streaming thread. Commit on the Qt
    // owner thread, where PlayerCore and every observer expect transport events.
    QMetaObject::invokeMethod(this, [this, generation]() {
        commitGaplessAdvance(generation);
    }, Qt::QueuedConnection);
}

void GStreamerPlaybackBackend::commitGaplessAdvance(quint64 generation)
{
    {
        QMutexLocker locker(&m_mutex);
        if (!m_gaplessAdvancePending || generation != m_gaplessGeneration) {
            return;
        }
        m_gaplessAdvancePending = false;
        m_gaplessStartQueued = false;
        m_playingUri = m_currentUri;
    }

    // Publish a clean per-track timeline before PlayerCore presents the new row.
    // The next poll refines the position from the sink clock, normally within
    // 100 ms, without ever inheriting the outgoing track's tail.
    resetTimeline(0, m_durationMs);
    emit preparedTrackStarted();

    // Duration was held while the handoff was pending. Publish the successor's
    // length in the same event-loop turn as the queue advance.
    gint64 duration = GST_CLOCK_TIME_NONE;
    if (m_playbin != nullptr
        && gst_element_query_duration(m_playbin, GST_FORMAT_TIME, &duration)) {
        const qint64 durationMs = clockTimeToMs(duration);
        if (durationMs != m_durationMs) {
            m_durationMs = durationMs;
            emit durationChanged(m_durationMs);
        }
    }
}

void GStreamerPlaybackBackend::commitSinkStartedGaplessAdvanceIfNeeded()
{
    quint64 generation = 0;
    {
        QMutexLocker locker(&m_mutex);
        if (m_gaplessAdvancePending && m_gaplessStartQueued) {
            generation = m_gaplessGeneration;
        }
    }
    if (generation != 0) {
        commitGaplessAdvance(generation);
    }
}

void GStreamerPlaybackBackend::invalidateGaplessAdvanceLocked()
{
    ++m_gaplessGeneration;
    m_gaplessAdvancePending = false;
    m_gaplessStartQueued = false;
}

void GStreamerPlaybackBackend::poll()
{
    pollBus();
    pollPosition();
}

void GStreamerPlaybackBackend::pollBus()
{
    if (m_playbin == nullptr) {
        return;
    }

    GstBus *bus = gst_element_get_bus(m_playbin);
    while (GstMessage *message = gst_bus_pop(bus)) {
        handleMessage(message);
        gst_message_unref(message);
    }
    gst_object_unref(bus);
}

void GStreamerPlaybackBackend::pollPosition()
{
    if (m_playbin == nullptr || m_softPaused) {
        return;
    }

    // Between about-to-finish and the audible switch the pipeline can already
    // answer duration queries with the queued next track's length while
    // position still reads the outgoing track's tail — publishing that mix
    // makes the progress bar jump through bogus ratios (100% → ~20% → 0%).
    // Hold the outgoing duration until the handoff commits below.
    //
    // The sink-pad STREAM_START probe normally commits the handoff. Keep the
    // position reset below as a fallback for a third-party sink without a
    // readable static pad; app-issued seeks cannot fake it because seek()
    // reloads during a pending handoff and issueSeek() rebases m_positionMs.
    bool handoffPending = false;
    {
        QMutexLocker locker(&m_mutex);
        handoffPending = m_gaplessAdvancePending;
    }

    gint64 position = GST_CLOCK_TIME_NONE;
    gint64 duration = GST_CLOCK_TIME_NONE;
    const qint64 previousPositionMs = m_positionMs;
    if (gst_element_query_position(m_playbin, GST_FORMAT_TIME, &position)) {
        const qint64 positionMs = clockTimeToMs(position);
        if (positionMs != m_positionMs) {
            m_positionMs = positionMs;
            emit positionChanged(m_positionMs);
        }
    }
    if (!handoffPending && gst_element_query_duration(m_playbin, GST_FORMAT_TIME, &duration)) {
        const qint64 durationMs = clockTimeToMs(duration);
        if (durationMs != m_durationMs) {
            m_durationMs = durationMs;
            emit durationChanged(m_durationMs);
        }
    }

    quint64 fallbackGeneration = 0;
    if (m_audioSinkProbeId == 0 && handoffPending && m_state == State::Playing) {
        const bool positionReset = m_positionMs + 1000 < previousPositionMs;
        if (m_positionMs <= 2000 || positionReset) {
            QMutexLocker locker(&m_mutex);
            if (m_gaplessAdvancePending) {
                fallbackGeneration = m_gaplessGeneration;
            }
        }
    }
    if (fallbackGeneration != 0) {
        commitGaplessAdvance(fallbackGeneration);
    }

    // Slide the read-ahead window forward to track the playhead.
    pumpReadAhead(m_positionMs);
}

void GStreamerPlaybackBackend::beginTargetTransition(State targetState)
{
    m_targetState = targetState;
    m_waitingForTargetState = targetState == State::Playing || targetState == State::Paused;
    if (m_waitingForTargetState) {
        m_transitionTimer.start();
        m_pollTimer.start();
    } else {
        m_transitionTimer.stop();
    }
}

void GStreamerPlaybackBackend::finishTargetTransition()
{
    m_waitingForTargetState = false;
    m_transitionTimer.stop();
}

void GStreamerPlaybackBackend::handleTargetTransitionTimeout()
{
    if (!m_waitingForTargetState || m_playbin == nullptr) {
        return;
    }

    GstState current = GST_STATE_NULL;
    GstState pending = GST_STATE_VOID_PENDING;
    gst_element_get_state(m_playbin, &current, &pending, 0);
    Q_UNUSED(pending)
    m_pendingSeekMs = -1;
    finishTargetTransition();
    publishObservedState(stateFromGst(current));
}

void GStreamerPlaybackBackend::publishObservedState(State observedState)
{
    if (m_waitingForTargetState) {
        if (observedState != m_targetState) {
            return;
        }
        finishTargetTransition();
    }
    updateState(observedState);
}

void GStreamerPlaybackBackend::handleMessage(GstMessage *message)
{
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
        // The sink may have seen the successor start while its queued Qt
        // callback waits behind this bus poll. Commit first so recovery reloads
        // the stream that actually failed, not the outgoing URI.
        commitSinkStartedGaplessAdvanceIfNeeded();
        GError *error = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        const QString text = error != nullptr ? QString::fromUtf8(error->message) : QStringLiteral("Unknown GStreamer error");
        if (error != nullptr) {
            g_error_free(error);
        }
        if (debug != nullptr) {
            g_free(debug);
        }
        // Decode/parse errors (a bad frame after a seek, a transient stall) are
        // often survivable: reload the source in place before surfacing the
        // error and freezing the transport on the user.
        if (tryRecoverFromStreamError()) {
            emit technicalInfoChanged(QStringLiteral("Recovered from pipeline error: %1").arg(text));
            break;
        }
        m_targetState = State::Error;
        finishTargetTransition();
        updateState(State::Error);
        emit errorOccurred(text);
        break;
    }
    case GST_MESSAGE_EOS: {
        // A short successor may reach EOS while its sink-start callback waits
        // on a busy UI thread. Advance before publishing finished(), so
        // PlayerCore finishes the row that was actually audible.
        commitSinkStartedGaplessAdvanceIfNeeded();
        m_targetState = State::Stopped;
        finishTargetTransition();
        clearSeekInFlight();
        {
            // A handoff that never produced a stream (e.g. the queued uri failed)
            // must not leave the advance flag armed for the next load.
            QMutexLocker locker(&m_mutex);
            invalidateGaplessAdvanceLocked();
        }
        updateState(State::Stopped);
        emit finished();
        break;
    }
    case GST_MESSAGE_ASYNC_DONE:
        // Preroll (or a flushing seek) has completed.  If a soft-pause resume is
        // pending, this is the safe point to seek to the saved position and
        // start playing — without ever having blocked the GUI thread.
        if (m_pendingSeekMs >= 0 && GST_MESSAGE_SRC(message) == GST_OBJECT(m_playbin)) {
            const qint64 seekTo = m_pendingSeekMs;
            m_pendingSeekMs = -1; // clear first so the post-seek ASYNC_DONE is a no-op
            if (seekTo > 0) {
                gst_element_seek_simple(m_playbin,
                                        GST_FORMAT_TIME,
                                        kSeekFlags,
                                        static_cast<gint64>(seekTo) * GST_MSECOND);
            }
            gst_element_set_state(m_playbin, GST_STATE_PLAYING);
        } else if (GST_MESSAGE_SRC(message) == GST_OBJECT(m_playbin)) {
            // The in-flight flushing seek completed; release the coalescing
            // latch and fire the newest queued target, if any arrived while
            // the flush was running (slider scrubs produce these bursts).
            if (m_seekInFlight) {
                m_seekWatchdog.stop();
                m_seekInFlight = false;
                if (m_queuedSeekMs >= 0) {
                    const qint64 next = m_queuedSeekMs;
                    m_queuedSeekMs = -1;
                    issueSeek(next);
                }
            }
            GstState current = GST_STATE_NULL;
            GstState pending = GST_STATE_VOID_PENDING;
            gst_element_get_state(m_playbin, &current, &pending, 0);
            Q_UNUSED(pending)
            publishObservedState(stateFromGst(current));
        }
        break;
    case GST_MESSAGE_STATE_CHANGED:
        if (GST_MESSAGE_SRC(message) == GST_OBJECT(m_playbin)) {
            GstState oldState = GST_STATE_NULL;
            GstState newState = GST_STATE_NULL;
            GstState pending = GST_STATE_NULL;
            gst_message_parse_state_changed(message, &oldState, &newState, &pending);
            Q_UNUSED(oldState)
            Q_UNUSED(pending)
            // Publish transport intent, not GStreamer's transient hops. Source
            // swaps and resume preroll move through READY/PAUSED on the way to
            // PLAYING; forwarding those as user-visible Stopped/Paused drifts
            // PlayerBar, IPC/MPRIS and scrobbling away from audible playback.
            if (!m_softPaused && m_pendingSeekMs < 0) {
                publishObservedState(stateFromGst(newState));
            }
        }
        break;
    default:
        break;
    }
}

void GStreamerPlaybackBackend::updateState(State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    if (m_state == State::Stopped || m_state == State::Error) {
        m_pollTimer.stop();
    } else {
        m_pollTimer.start();
    }
    emit stateChanged(m_state);
}

void GStreamerPlaybackBackend::resetTimeline(qint64 positionMs, qint64 durationMs)
{
    const qint64 safePosition = std::max<qint64>(0, positionMs);
    const qint64 safeDuration = std::max<qint64>(0, durationMs);
    const bool positionUpdated = m_positionMs != safePosition;
    const bool durationUpdated = m_durationMs != safeDuration;
    m_positionMs = safePosition;
    m_durationMs = safeDuration;
    if (positionUpdated) {
        emit positionChanged(m_positionMs);
    }
    if (durationUpdated) {
        emit durationChanged(m_durationMs);
    }
}

QString GStreamerPlaybackBackend::uriForUrl(const QUrl &url) const
{
    return QString::fromUtf8(url.toEncoded());
}

void GStreamerPlaybackBackend::onGaplessTrackAdvanced()
{
    // The prepared track has become the current one (no play() call fires for a
    // gapless advance), so re-point the read-ahead window at the new file.
    QString uri;
    {
        QMutexLocker locker(&m_mutex);
        uri = m_currentUri;
    }
    if (!uri.isEmpty()) {
        startReadAhead(QUrl::fromEncoded(uri.toUtf8()));
    } else {
        stopReadAhead();
    }
}

namespace {
constexpr qint64 kMiB = 1024 * 1024;
} // namespace

void GStreamerPlaybackBackend::startReadAhead(const QUrl &url)
{
    stopReadAhead();

    if (m_profile.readAheadMb <= 0 || !url.isLocalFile()) {
        return;
    }

    const std::string path = url.toLocalFile().toStdString();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return; // best-effort: leave read-ahead off
    }

    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
        ::close(fd);
        return;
    }

    m_readAheadFd = fd;
    m_readAheadSize = static_cast<qint64>(st.st_size);
    m_readAheadAdvised = 0;
    // Hint sequential access so the kernel widens its own read-ahead window;
    // then prime the head of the file immediately.
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    pumpReadAhead(0);
}

void GStreamerPlaybackBackend::pumpReadAhead(qint64 positionMs)
{
    if (m_readAheadFd < 0 || m_profile.readAheadMb <= 0 || m_readAheadSize <= 0) {
        return;
    }

    // Estimate the current byte offset from the playback ratio (approximate for
    // VBR, which is fine — this is only a cache hint).
    qint64 offset = 0;
    if (positionMs > 0 && m_durationMs > 0) {
        offset = m_readAheadSize * std::min<qint64>(positionMs, m_durationMs) / m_durationMs;
    }

    const qint64 window = static_cast<qint64>(m_profile.readAheadMb) * kMiB;
    // A large backward seek lands before the warmed region: rewind the watermark
    // so the new neighbourhood gets pulled in again.
    if (offset + window < m_readAheadAdvised) {
        m_readAheadAdvised = offset;
    }

    const qint64 desiredEnd = std::min(m_readAheadSize, offset + window);
    if (desiredEnd > m_readAheadAdvised) {
        ::posix_fadvise(m_readAheadFd, m_readAheadAdvised,
                        desiredEnd - m_readAheadAdvised, POSIX_FADV_WILLNEED);
        m_readAheadAdvised = desiredEnd;
    }
}

void GStreamerPlaybackBackend::stopReadAhead()
{
    if (m_readAheadFd >= 0) {
        ::close(m_readAheadFd);
        m_readAheadFd = -1;
    }
    m_readAheadSize = 0;
    m_readAheadAdvised = 0;
}

void GStreamerPlaybackBackend::warmFileHead(const QUrl &url) const
{
    if (m_profile.readAheadMb <= 0 || !url.isLocalFile()) {
        return;
    }
    const std::string path = url.toLocalFile().toStdString();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    // Page cache is per-inode, so the warmed pages persist after we drop the fd.
    ::posix_fadvise(fd, 0, static_cast<qint64>(m_profile.readAheadMb) * kMiB,
                    POSIX_FADV_WILLNEED);
    ::close(fd);
}
