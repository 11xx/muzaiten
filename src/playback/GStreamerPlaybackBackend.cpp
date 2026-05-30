#include "playback/GStreamerPlaybackBackend.h"

#include <QMutexLocker>

#include <gst/gst.h>

#include <algorithm>
#include <mutex>

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

    m_pollTimer.setInterval(100);
    connect(&m_pollTimer, &QTimer::timeout, this, &GStreamerPlaybackBackend::poll);
    m_pollTimer.start();
}

GStreamerPlaybackBackend::~GStreamerPlaybackBackend()
{
    if (m_playbin != nullptr) {
        gst_element_set_state(m_playbin, GST_STATE_NULL);
        gst_object_unref(m_playbin);
        m_playbin = nullptr;
    }
}

void GStreamerPlaybackBackend::setProfile(const PlaybackProfile &profile)
{
    m_profile = profile;
    rebuildPipeline();
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
}

void GStreamerPlaybackBackend::prepareNext(const QUrl &url)
{
    const QString uri = uriForUrl(url);
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
        const qint64 seekTo = m_resumePositionMs;
        m_softPaused = false;
        // Re-preroll: READY→PAUSED.  Block until the pipeline has prerolled
        // (typically <50 ms for local files) so we can seek reliably before
        // letting it play.
        gst_element_set_state(m_playbin, GST_STATE_PAUSED);
        gst_element_get_state(m_playbin, nullptr, nullptr, 5 * GST_SECOND);
        if (seekTo > 0) {
            gst_element_seek_simple(m_playbin,
                                    GST_FORMAT_TIME,
                                    static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH
                                                              | GST_SEEK_FLAG_KEY_UNIT),
                                    static_cast<gint64>(seekTo) * GST_MSECOND);
        }
    }
    gst_element_set_state(m_playbin, GST_STATE_PLAYING);
}

void GStreamerPlaybackBackend::stop()
{
    m_softPaused = false;
    m_pendingSeekMs = -1;
    if (m_playbin != nullptr) {
        gst_element_set_state(m_playbin, GST_STATE_NULL);
    }
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

    configureSink();
    setVolume(m_volume);
    g_signal_connect(m_playbin, "about-to-finish", G_CALLBACK(GStreamerPlaybackBackend::aboutToFinishCallback), this);
    emit technicalInfoChanged(QStringLiteral("GStreamer backend using %1").arg(playbinName));
}

void GStreamerPlaybackBackend::configureSink()
{
    GstElement *sink = makeSink(m_profile);
    if (sink == nullptr) {
        emit errorOccurred(QStringLiteral("No usable GStreamer audio sink is available"));
        return;
    }
    g_object_set(G_OBJECT(m_playbin), "audio-sink", sink, nullptr);
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
    case GST_MESSAGE_STATE_CHANGED:
        if (GST_MESSAGE_SRC(message) == GST_OBJECT(m_playbin)) {
            GstState oldState = GST_STATE_NULL;
            GstState newState = GST_STATE_NULL;
            GstState pending = GST_STATE_NULL;
            gst_message_parse_state_changed(message, &oldState, &newState, &pending);
            Q_UNUSED(oldState)
            Q_UNUSED(pending)
            // While soft-paused the pipeline is transitioning to READY to release
            // the audio device; suppress those intermediate state messages so the
            // UI stays in Paused and we don't notify scrobblers/MPRIS of a stop.
            if (!m_softPaused) {
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
    emit stateChanged(m_state);
}

QString GStreamerPlaybackBackend::uriForUrl(const QUrl &url) const
{
    return QString::fromUtf8(url.toEncoded());
}
