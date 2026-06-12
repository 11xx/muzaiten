#include "playback/GStreamerPlaybackBackend.h"

#include <QMutexLocker>

#include <gst/gst.h>

#include <algorithm>
#include <mutex>

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

GstElement *makeSink(const PlaybackProfile &profile)
{
    const auto make = [](const char *name) -> GstElement * {
        return factoryExists(name) ? gst_element_factory_make(name, nullptr) : nullptr;
    };

    // Bit-perfect always goes direct to ALSA hw: regardless of the sink field.
    if (profile.mode == QStringLiteral("bit-perfect")) {
        GstElement *sink = make("alsasink");
        if (sink != nullptr && !profile.device.isEmpty()) {
            g_object_set(G_OBJECT(sink), "device", profile.device.toUtf8().constData(), nullptr);
        }
        return sink;
    }

    GstElement *sink = nullptr;
    if (profile.sink == QStringLiteral("alsa")) {
        sink = make("alsasink");
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

    sink = make("pipewiresink");
    if (sink != nullptr) {
        return sink;
    }
    sink = make("pulsesink");
    if (sink != nullptr) {
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
}

GStreamerPlaybackBackend::~GStreamerPlaybackBackend()
{
    if (m_playbin != nullptr) {
        gst_element_set_state(m_playbin, GST_STATE_NULL);
        gst_object_unref(m_playbin);
        m_playbin = nullptr;
    }
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
        rebuildPipeline();
        return;
    }

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
        || a.releaseSinkOnPause != b.releaseSinkOnPause;
}

void GStreamerPlaybackBackend::play(const QUrl &url)
{
    if (m_playbin == nullptr || url.isEmpty()) {
        return;
    }

    m_softPaused = false;
    m_pendingSeekMs = -1;
    gst_element_set_state(m_playbin, GST_STATE_READY);

    const QString uri = uriForUrl(url);
    {
        QMutexLocker locker(&m_mutex);
        m_currentUri = uri;
        m_preparedUri.clear();
        m_gaplessAdvancePending = false;
    }
    g_object_set(G_OBJECT(m_playbin), "uri", uri.toUtf8().constData(), nullptr);
    gst_element_set_state(m_playbin, GST_STATE_PLAYING);
    // State updates arrive over the bus, which only poll() drains — make sure
    // the timer is live before the first message lands.
    m_pollTimer.start();
    startReadAhead(url);
}

void GStreamerPlaybackBackend::loadPaused(const QUrl &url)
{
    if (m_playbin == nullptr || url.isEmpty()) {
        return;
    }

    m_softPaused = false;
    m_pendingSeekMs = -1;
    gst_element_set_state(m_playbin, GST_STATE_READY);

    const QString uri = uriForUrl(url);
    {
        QMutexLocker locker(&m_mutex);
        m_currentUri = uri;
        m_preparedUri.clear();
        m_gaplessAdvancePending = false;
    }
    g_object_set(G_OBJECT(m_playbin), "uri", uri.toUtf8().constData(), nullptr);
    // Transition only to PAUSED: pipeline prerolls (buffering the first frame)
    // but the audio sink never produces output. No blip.
    gst_element_set_state(m_playbin, GST_STATE_PAUSED);
    m_pollTimer.start();
    startReadAhead(url);
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

void GStreamerPlaybackBackend::pause()
{
    if (m_playbin == nullptr) {
        return;
    }

    const bool release = (m_profile.mode == QStringLiteral("bit-perfect"))
                         || m_profile.releaseSinkOnPause;

    if (release) {
        // Capture position before tearing down the pipeline.
        gint64 pos = GST_CLOCK_TIME_NONE;
        gst_element_query_position(m_playbin, GST_FORMAT_TIME, &pos);
        m_resumePositionMs = clockTimeToMs(pos);
        m_softPaused = true;
        // READY releases the audio device but keeps the element graph intact
        // so re-preroll on resume is fast.  State messages from the
        // PLAYING→PAUSED→READY transitions are suppressed in handleMessage
        // while m_softPaused is true.
        gst_element_set_state(m_playbin, GST_STATE_READY);
        updateState(State::Paused);
    } else {
        gst_element_set_state(m_playbin, GST_STATE_PAUSED);
    }
}

void GStreamerPlaybackBackend::resume()
{
    if (m_playbin == nullptr) {
        return;
    }

    if (m_softPaused) {
        m_softPaused = false;
        // Re-preroll asynchronously: READY→PAUSED.  We must NOT block the GUI
        // thread waiting for preroll — a slow/contended device could freeze the
        // UI for seconds.  Remember the resume position; handleMessage() applies
        // the seek and transitions to PLAYING once GST_MESSAGE_ASYNC_DONE shows
        // the pipeline has prerolled.
        m_pendingSeekMs = std::max<qint64>(0, m_resumePositionMs);
        gst_element_set_state(m_playbin, GST_STATE_PAUSED);
        // Reflect the user's intent immediately; STATE_CHANGED messages are
        // suppressed while a resume seek is pending so the UI doesn't flicker
        // through Paused.
        updateState(State::Playing);
        return;
    }
    gst_element_set_state(m_playbin, GST_STATE_PLAYING);
    m_pollTimer.start();
}

void GStreamerPlaybackBackend::stop()
{
    m_softPaused = false;
    m_pendingSeekMs = -1;
    if (m_playbin != nullptr) {
        gst_element_set_state(m_playbin, GST_STATE_NULL);
    }
    stopReadAhead();
    // Clear all source state so hasSource() is honest after stop/end-of-queue.
    {
        QMutexLocker locker(&m_mutex);
        m_currentUri.clear();
        m_preparedUri.clear();
        m_gaplessAdvancePending = false;
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
}

void GStreamerPlaybackBackend::seek(qint64 positionMs)
{
    if (m_playbin == nullptr) {
        return;
    }
    gst_element_seek_simple(m_playbin,
                            GST_FORMAT_TIME,
                            static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                            static_cast<gint64>(std::max<qint64>(0, positionMs)) * GST_MSECOND);
}

void GStreamerPlaybackBackend::setVolume(double volume0To1)
{
    m_volume = std::clamp(volume0To1, 0.0, 1.0);
    if (m_playbin != nullptr && m_profile.softwareVolume) {
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

void GStreamerPlaybackBackend::aboutToFinishCallback(GstElement *playbin, void *userData)
{
    auto *self = static_cast<GStreamerPlaybackBackend *>(userData);
    QString preparedUri;
    {
        QMutexLocker locker(&self->m_mutex);
        preparedUri = self->m_preparedUri;
        self->m_preparedUri.clear();
        self->m_gaplessAdvancePending = !preparedUri.isEmpty();
        if (!preparedUri.isEmpty()) {
            self->m_currentUri = preparedUri;
        }
    }

    if (!preparedUri.isEmpty()) {
        g_object_set(G_OBJECT(playbin), "uri", preparedUri.toUtf8().constData(), nullptr);
    }

    QMetaObject::invokeMethod(self, [self]() {
        emit self->aboutToNeedNext();
    }, Qt::QueuedConnection);
}

void GStreamerPlaybackBackend::rebuildPipeline()
{
    if (m_playbin != nullptr) {
        gst_element_set_state(m_playbin, GST_STATE_NULL);
        gst_object_unref(m_playbin);
        m_playbin = nullptr;
    }

    m_playbin = gst_element_factory_make("playbin3", "muzaiten-playbin");
    const QString playbinName = m_playbin != nullptr ? QStringLiteral("playbin3") : QStringLiteral("playbin");
    if (m_playbin == nullptr) {
        m_playbin = gst_element_factory_make("playbin", "muzaiten-playbin");
    }
    if (m_playbin == nullptr) {
        updateState(State::Error);
        emit errorOccurred(QStringLiteral("GStreamer playbin is not available"));
        return;
    }

    if (!configureSink()) {
        // configureSink already reported the error and set State::Error; don't
        // advertise the pipeline as healthy or wire up gapless signalling.
        return;
    }
    setVolume(m_volume);
    g_signal_connect(m_playbin, "about-to-finish", G_CALLBACK(GStreamerPlaybackBackend::aboutToFinishCallback), this);
    emit technicalInfoChanged(QStringLiteral("GStreamer backend using %1").arg(playbinName));
}

bool GStreamerPlaybackBackend::configureSink()
{
    GstElement *sink = makeSink(m_profile);
    if (sink == nullptr) {
        // Drive into Error (matching the playbin-missing and bus-error paths)
        // rather than letting playbin silently auto-plug its default sink — a
        // bit-perfect/explicit-device profile must not fall through to shared
        // output without the user knowing.
        updateState(State::Error);
        emit errorOccurred(QStringLiteral("No usable GStreamer audio sink is available"));
        return false;
    }
    g_object_set(G_OBJECT(m_playbin), "audio-sink", sink, nullptr);
    return true;
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
    if (gst_element_query_duration(m_playbin, GST_FORMAT_TIME, &duration)) {
        const qint64 durationMs = clockTimeToMs(duration);
        if (durationMs != m_durationMs) {
            m_durationMs = durationMs;
            emit durationChanged(m_durationMs);
        }
    }

    bool emitPreparedStarted = false;
    {
        QMutexLocker locker(&m_mutex);
        const bool positionReset = m_positionMs + 1000 < previousPositionMs;
        if (m_gaplessAdvancePending && m_state == State::Playing && (m_positionMs <= 2000 || positionReset)) {
            m_gaplessAdvancePending = false;
            emitPreparedStarted = true;
        }
    }
    if (emitPreparedStarted) {
        emit preparedTrackStarted();
    }

    // Slide the read-ahead window forward to track the playhead.
    pumpReadAhead(m_positionMs);
}

void GStreamerPlaybackBackend::handleMessage(GstMessage *message)
{
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_ERROR: {
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
        updateState(State::Error);
        emit errorOccurred(text);
        break;
    }
    case GST_MESSAGE_EOS:
        updateState(State::Stopped);
        emit finished();
        break;
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
                                        static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH
                                                                  | GST_SEEK_FLAG_KEY_UNIT),
                                        static_cast<gint64>(seekTo) * GST_MSECOND);
            }
            gst_element_set_state(m_playbin, GST_STATE_PLAYING);
            updateState(State::Playing);
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
            // Suppress intermediate state messages when:
            //  - soft-paused: pipeline is dropping to READY to release the device
            //    (keep the UI in Paused, don't tell scrobblers/MPRIS we stopped);
            //  - a resume seek is pending: pipeline briefly sits in PAUSED while
            //    re-prerolling, but the user already asked to play.
            if (!m_softPaused && m_pendingSeekMs < 0) {
                updateState(stateFromGst(newState));
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
