#include <QMutex>
#include <QTimer>

#define private public
#include "playback/GStreamerPlaybackBackend.h"
#undef private
#include "playback/OutputEndpointMonitor.h"
#include "playback/QtPlaybackBackend.h"
#include "player/PlayerCore.h"

#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest/QtTest>

#include <cmath>

#include <gst/gst.h>

namespace {

bool writeToneWav(const QString &path, double frequencyHz, int durationMs)
{
    constexpr quint32 sampleRate = 8000;
    constexpr quint16 channels = 1;
    constexpr quint16 bitsPerSample = 16;
    const quint32 sampleCount = sampleRate * static_cast<quint32>(durationMs) / 1000;
    const quint32 dataBytes = sampleCount * channels * (bitsPerSample / 8);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("RIFF", 4);
    out << quint32{36 + dataBytes};
    out.writeRawData("WAVEfmt ", 8);
    out << quint32{16} << quint16{1} << channels << sampleRate;
    out << quint32{sampleRate * channels * (bitsPerSample / 8)};
    out << quint16{channels * (bitsPerSample / 8)} << bitsPerSample;
    out.writeRawData("data", 4);
    out << dataBytes;

    constexpr double tau = 6.2831853071795864769;
    for (quint32 sample = 0; sample < sampleCount; ++sample) {
        const double phase = tau * frequencyHz * static_cast<double>(sample)
            / static_cast<double>(sampleRate);
        out << static_cast<qint16>(std::sin(phase) * 12000.0);
    }
    return out.status() == QDataStream::Ok;
}

} // namespace

class GStreamerPlaybackTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qputenv("MUZAITEN_DEMO_SILENT_AUDIO", "1");
    }

    void cleanupTestCase()
    {
        qunsetenv("MUZAITEN_DEMO_SILENT_AUDIO");
    }

    void releasingPauseRestoresPositionOnTheCorrectSinkState_data()
    {
        QTest::addColumn<bool>("native");
        QTest::newRow("native") << true;
        QTest::newRow("other-sink") << false;
    }

    void releasingPauseRestoresPositionOnTheCorrectSinkState()
    {
        QFETCH(bool, native);
        QTemporaryDir dir;
        const QString tone = dir.filePath(QStringLiteral("resume.wav"));
        QVERIFY(writeToneWav(tone, 440.0, 30'000));
        struct Observation {
            GstElement *pipeline = nullptr;
            std::atomic<int> flushState{GST_STATE_NULL};
            std::atomic<bool> muted{false};
        } observation;
        GStreamerPlaybackBackend backend;
        PlaybackProfile profile;
        profile.releaseSinkOnPause = true;
        backend.setProfile(profile);
        backend.play(QUrl::fromLocalFile(tone));
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(backend.position() >= 1500, 3000);
        backend.pause();
        const qint64 held = backend.m_resumePositionMs;
        QVERIFY(held >= 1500);
        GstState state = GST_STATE_NULL;
        gst_element_get_state(backend.m_playbin, &state, nullptr, 0);
        QCOMPARE(state, GST_STATE_READY);
        // The silent sink exercises transport ordering without a sound server.
        if (native) {
            backend.m_sinkFactory = QStringLiteral("pipewiresink");
        }
        observation.pipeline = backend.m_playbin;
        GstPad *pad = gst_element_get_static_pad(backend.m_audioSink, "sink");
        gst_pad_add_probe(pad,
                          static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH),
                          [](GstPad *, GstPadProbeInfo *info, gpointer data) -> GstPadProbeReturn {
                              auto *observed = static_cast<Observation *>(data);
                              if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) == GST_EVENT_FLUSH_START) {
                                  GstState current = GST_STATE_NULL;
                                  gst_element_get_state(observed->pipeline, &current, nullptr, 0);
                                  observed->flushState.store(current);
                                  gboolean muted = FALSE;
                                  g_object_get(G_OBJECT(observed->pipeline), "mute", &muted, nullptr);
                                  observed->muted.store(muted);
                              }
                              return GST_PAD_PROBE_OK;
                          }, &observation, nullptr);
        gst_object_unref(pad);
        QSignalSpy positions(&backend, &PlaybackBackend::positionChanged);
        backend.resume();
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(backend.position() >= held + 300, 3000);
        QCOMPARE(observation.flushState.load(), native ? GST_STATE_PLAYING : GST_STATE_PAUSED);
        QCOMPARE(observation.muted.load(), native);
        for (const auto &position : positions) {
            QVERIFY(position.first().toLongLong() >= held - 250);
        }
        gboolean muted = TRUE;
        g_object_get(G_OBJECT(backend.m_playbin), "mute", &muted, nullptr);
        QVERIFY(!muted);
        QVERIFY(backend.m_profile.releaseSinkOnPause);
        backend.stop();
    }

    void nativeRestoreKeepsTheLatestPauseAndSeekIntent_data()
    {
        QTest::addColumn<bool>("release");
        QTest::newRow("release") << true;
        QTest::newRow("keep-open") << false;
    }

    void nativeRestoreKeepsTheLatestPauseAndSeekIntent()
    {
        QFETCH(bool, release);
        QTemporaryDir dir;
        const QString tone = dir.filePath(QStringLiteral("intent.wav"));
        QVERIFY(writeToneWav(tone, 440.0, 30'000));
        GStreamerPlaybackBackend backend;
        PlaybackProfile profile;
        profile.releaseSinkOnPause = release;
        backend.setProfile(profile);
        backend.play(QUrl::fromLocalFile(tone));
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 3000);
        backend.m_sinkFactory = QStringLiteral("pipewiresink");
        QSignalSpy positions(&backend, &PlaybackBackend::positionChanged);
        backend.reloadCurrentAtPosition(5000, PlaybackBackend::State::Playing);
        backend.seek(6000);
        QCOMPARE(backend.position(), qint64{6000});
        QVERIFY(!positions.isEmpty());
        QCOMPARE(positions.last().at(0).toLongLong(), qint64{6000});
        backend.pause();
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Paused, 3000);
        QVERIFY(std::abs(backend.position() - 6000) < 250);
        GstState state = GST_STATE_NULL;
        gst_element_get_state(backend.m_playbin, &state, nullptr, 0);
        QCOMPARE(state, release ? GST_STATE_READY : GST_STATE_PAUSED);
        QTest::qWait(200);
        QCOMPARE(backend.state(), PlaybackBackend::State::Paused);
        QVERIFY(std::abs(backend.position() - 6000) < 250);
        backend.resume();
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(backend.position() > 6300, 3000);
        backend.stop();
        gboolean muted = TRUE;
        g_object_get(G_OBJECT(backend.m_playbin), "mute", &muted, nullptr);
        QVERIFY(!muted);
    }

    void nativeRestoreCanReloadPausedAndCancelForAnotherTrack()
    {
        QTemporaryDir dir;
        const QString tone = dir.filePath(QStringLiteral("reload.wav"));
        const QString next = dir.filePath(QStringLiteral("next.wav"));
        QVERIFY(writeToneWav(tone, 440.0, 30'000));
        QVERIFY(writeToneWav(next, 660.0, 30'000));
        GStreamerPlaybackBackend backend;
        backend.play(QUrl::fromLocalFile(tone));
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 3000);
        backend.prepareNext(QUrl::fromLocalFile(next));
        backend.m_sinkFactory = QStringLiteral("pipewiresink");
        backend.reloadCurrentAtPosition(5000, PlaybackBackend::State::Paused);
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Paused, 3000);
        QVERIFY(std::abs(backend.position() - 5000) < 250);
        QVERIFY(backend.m_preparedUri.endsWith(QStringLiteral("next.wav")));
        backend.resume();
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 3000);
        backend.pause();
        backend.resume();
        backend.stop();
        backend.play(QUrl::fromLocalFile(next));
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(backend.position() > 300, 3000);
        QVERIFY(backend.position() < 3000);
        gboolean muted = TRUE;
        g_object_get(G_OBJECT(backend.m_playbin), "mute", &muted, nullptr);
        QVERIFY(!muted);
        backend.stop();
    }

    // The transition deadline passes while the restore still waits for its
    // first seek. The handler is invoked directly instead of waiting out the
    // 15 s deadline, then the queued preroll is released through the bus.
    void nativeRestoreTimeoutFailsClosedBeforeItsSeek()
    {
        QTemporaryDir dir;
        const QString tone = dir.filePath(QStringLiteral("timeout.wav"));
        QVERIFY(writeToneWav(tone, 440.0, 30'000));
        GStreamerPlaybackBackend backend;
        PlaybackProfile profile;
        profile.releaseSinkOnPause = true;
        backend.setProfile(profile);
        backend.play(QUrl::fromLocalFile(tone));
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(backend.position() >= 1500, 3000);
        backend.pause();
        backend.m_sinkFactory = QStringLiteral("pipewiresink");
        QSignalSpy errors(&backend, &PlaybackBackend::errorOccurred);
        backend.seek(5000);
        backend.resume();
        QVERIFY(backend.m_nativePositionRestore);
        backend.seek(6000);
        backend.handleTargetTransitionTimeout();

        QCOMPARE(errors.count(), 1);
        QCOMPARE(backend.state(), PlaybackBackend::State::Error);
        QVERIFY(!backend.m_nativePositionRestore);
        QVERIFY(backend.m_pendingSeekMs < 0);
        gboolean muted = TRUE;
        g_object_get(G_OBJECT(backend.m_playbin), "mute", &muted, nullptr);
        QVERIFY(!muted);

        // The stale preroll completes after the deadline. It must neither
        // restore the held position nor publish a transport state.
        GstState settled = GST_STATE_NULL;
        gst_element_get_state(backend.m_playbin, &settled, nullptr, 2 * GST_SECOND);
        QVERIFY(settled == GST_STATE_READY || settled == GST_STATE_NULL);
        backend.pollBus();
        QCOMPARE(backend.state(), PlaybackBackend::State::Error);
        QCOMPARE(errors.count(), 1);

        // The released stream keeps the latest requested position, so resume
        // restores 6000 ms rather than starting from zero.
        backend.resume();
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(backend.position() > 6300, 3000);
        g_object_get(G_OBJECT(backend.m_playbin), "mute", &muted, nullptr);
        QVERIFY(!muted);
        backend.stop();
    }

    // The restore seek is refused by a pipeline that cannot seek. The pipeline
    // is taken to NULL before the preroll completes, so gst_element_seek_simple
    // reports failure; the PAUSED-to-PLAYING completion that would issue the
    // seek is then delivered to the handler directly, since NULL cancels the
    // real preroll before it can post it.
    void nativeRestoreRefusedSeekPublishesOneError_data()
    {
        QTest::addColumn<bool>("stopInCallback");
        QTest::newRow("publish") << false;
        QTest::newRow("stop-in-callback") << true;
    }

    void nativeRestoreRefusedSeekPublishesOneError()
    {
        QFETCH(bool, stopInCallback);
        QTemporaryDir dir;
        const QString tone = dir.filePath(QStringLiteral("refused.wav"));
        QVERIFY(writeToneWav(tone, 440.0, 30'000));
        const QString playing = QStringLiteral("state:%1")
                                    .arg(static_cast<int>(PlaybackBackend::State::Playing));
        const QString error = QStringLiteral("error");
        QStringList log;
        GStreamerPlaybackBackend backend;
        PlaybackProfile profile;
        profile.releaseSinkOnPause = true;
        backend.setProfile(profile);
        backend.play(QUrl::fromLocalFile(tone));
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(backend.position() >= 1500, 3000);
        backend.pause();
        backend.m_sinkFactory = QStringLiteral("pipewiresink");
        connect(&backend, &PlaybackBackend::stateChanged, this, [&log](PlaybackBackend::State state) {
            log << QStringLiteral("state:%1").arg(static_cast<int>(state));
        });
        connect(&backend, &PlaybackBackend::errorOccurred, this, [&backend, &log, stopInCallback](const QString &) {
            log << QStringLiteral("error");
            if (stopInCallback) {
                backend.stop();
            }
        });
        backend.resume();
        QVERIFY(backend.m_nativePositionRestore);
        gst_element_set_state(backend.m_playbin, GST_STATE_NULL);
        GstMessage *preroll = gst_message_new_state_changed(GST_OBJECT(backend.m_playbin),
                                                            GST_STATE_PAUSED,
                                                            GST_STATE_PLAYING,
                                                            GST_STATE_VOID_PENDING);
        backend.handleMessage(preroll);
        gst_message_unref(preroll);

        QCOMPARE(log.count(error), 1);
        QVERIFY(!log.mid(log.indexOf(error) + 1).contains(playing));
        QCOMPARE(backend.state(),
                 stopInCallback ? PlaybackBackend::State::Stopped : PlaybackBackend::State::Error);
        QVERIFY(!backend.m_seekInFlight);
        QVERIFY(!backend.m_nativePositionRestore);
        GstState settled = GST_STATE_NULL;
        gst_element_get_state(backend.m_playbin, &settled, nullptr, 2 * GST_SECOND);
        QVERIFY(settled == GST_STATE_READY || settled == GST_STATE_NULL);
        backend.pollBus();
        QCOMPARE(log.count(error), 1);
        QCOMPARE(backend.state(),
                 stopInCallback ? PlaybackBackend::State::Stopped : PlaybackBackend::State::Error);
        gboolean muted = TRUE;
        g_object_get(G_OBJECT(backend.m_playbin), "mute", &muted, nullptr);
        QVERIFY(!muted);
    }

    // The first restore seek is accepted and a newer retarget queues behind it.
    // When the source refuses the retarget, the restore still owes the newest
    // position, and the failure keeps that position as the resume point.
    void nativeRestoreRefusedQueuedSeekPublishesOneError()
    {
        QTemporaryDir dir;
        const QString tone = dir.filePath(QStringLiteral("queued.wav"));
        QVERIFY(writeToneWav(tone, 440.0, 30'000));
        const QString playing = QStringLiteral("state:%1")
                                    .arg(static_cast<int>(PlaybackBackend::State::Playing));
        const QString error = QStringLiteral("error");
        QStringList log;
        GStreamerPlaybackBackend backend;
        PlaybackProfile profile;
        profile.releaseSinkOnPause = true;
        backend.setProfile(profile);
        backend.play(QUrl::fromLocalFile(tone));
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(backend.position() >= 1500, 3000);
        backend.pause();
        backend.m_sinkFactory = QStringLiteral("pipewiresink");
        connect(&backend, &PlaybackBackend::stateChanged, this, [&log](PlaybackBackend::State state) {
            log << QStringLiteral("state:%1").arg(static_cast<int>(state));
        });
        connect(&backend, &PlaybackBackend::errorOccurred, this, [&log](const QString &) {
            log << QStringLiteral("error");
        });
        QSignalSpy errors(&backend, &PlaybackBackend::errorOccurred);
        backend.resume();
        QVERIFY(backend.m_nativePositionRestore);

        // The preroll completes, PLAYING starts the restore, and the seek it
        // issues is accepted by the playing pipeline.
        GstState current = GST_STATE_NULL;
        gst_element_get_state(backend.m_playbin, &current, nullptr, 3 * GST_SECOND);
        QCOMPARE(current, GST_STATE_PAUSED);
        GstMessage *prerolled = gst_message_new_async_done(GST_OBJECT(backend.m_playbin),
                                                           GST_CLOCK_TIME_NONE);
        backend.handleMessage(prerolled);
        gst_message_unref(prerolled);
        gst_element_get_state(backend.m_playbin, &current, nullptr, 3 * GST_SECOND);
        QCOMPARE(current, GST_STATE_PLAYING);
        GstMessage *started = gst_message_new_state_changed(GST_OBJECT(backend.m_playbin),
                                                            GST_STATE_PAUSED,
                                                            GST_STATE_PLAYING,
                                                            GST_STATE_VOID_PENDING);
        backend.handleMessage(started);
        gst_message_unref(started);
        QVERIFY(backend.m_seekInFlight);
        QVERIFY(backend.m_nativePositionRestore);

        const qint64 latest = 6000;
        backend.seek(latest);
        QCOMPARE(backend.m_queuedSeekMs, latest);
        // NULL refuses the queued seek; its completion is delivered directly.
        gst_element_set_state(backend.m_playbin, GST_STATE_NULL);
        GstMessage *completed = gst_message_new_async_done(GST_OBJECT(backend.m_playbin),
                                                           GST_CLOCK_TIME_NONE);
        backend.handleMessage(completed);
        gst_message_unref(completed);

        QCOMPARE(errors.count(), 1);
        QCOMPARE(backend.state(), PlaybackBackend::State::Error);
        QVERIFY(!log.mid(log.indexOf(error) + 1).contains(playing));
        QCOMPARE(backend.m_resumePositionMs, latest);
        QVERIFY(backend.m_softPaused);
        QVERIFY(!backend.m_seekInFlight);
        QVERIFY(!backend.m_nativePositionRestore);
        GstState settled = GST_STATE_NULL;
        gst_element_get_state(backend.m_playbin, &settled, nullptr, 2 * GST_SECOND);
        QVERIFY(settled == GST_STATE_READY || settled == GST_STATE_NULL);
        backend.pollBus();
        QCOMPARE(errors.count(), 1);
        QCOMPARE(backend.state(), PlaybackBackend::State::Error);
        gboolean muted = TRUE;
        g_object_get(G_OBJECT(backend.m_playbin), "mute", &muted, nullptr);
        QVERIFY(!muted);

        // The released stream keeps the newest position, so resume restores it.
        backend.resume();
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(backend.position() > latest + 300, 3000);
        backend.stop();
    }

    // A restore on a sink without a native seek path owes its position until the
    // PAUSED preroll accepts the positive seek. A refused seek fails the restore,
    // and PLAYING is never requested without the held position.
    void nonNativeRestoreRefusedSeekPublishesOneError()
    {
        QTemporaryDir dir;
        const QString tone = dir.filePath(QStringLiteral("refused-other.wav"));
        QVERIFY(writeToneWav(tone, 440.0, 30'000));
        const QString playing = QStringLiteral("state:%1")
                                    .arg(static_cast<int>(PlaybackBackend::State::Playing));
        const QString error = QStringLiteral("error");
        QStringList log;
        GStreamerPlaybackBackend backend;
        PlaybackProfile profile;
        profile.releaseSinkOnPause = true;
        backend.setProfile(profile);
        backend.play(QUrl::fromLocalFile(tone));
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(backend.position() >= 1500, 3000);
        backend.pause();
        const qint64 held = backend.m_resumePositionMs;
        QVERIFY(held >= 1500);
        connect(&backend, &PlaybackBackend::stateChanged, this, [&log](PlaybackBackend::State state) {
            log << QStringLiteral("state:%1").arg(static_cast<int>(state));
        });
        connect(&backend, &PlaybackBackend::errorOccurred, this, [&log](const QString &) {
            log << QStringLiteral("error");
        });
        QSignalSpy errors(&backend, &PlaybackBackend::errorOccurred);
        backend.resume();
        QVERIFY(!backend.m_nativePositionRestore);
        QCOMPARE(backend.m_pendingSeekMs, held);

        // NULL refuses the preroll's seek; its ASYNC_DONE is delivered directly.
        gst_element_set_state(backend.m_playbin, GST_STATE_NULL);
        GstMessage *prerolled = gst_message_new_async_done(GST_OBJECT(backend.m_playbin),
                                                           GST_CLOCK_TIME_NONE);
        backend.handleMessage(prerolled);
        gst_message_unref(prerolled);

        QCOMPARE(errors.count(), 1);
        QCOMPARE(backend.state(), PlaybackBackend::State::Error);
        QVERIFY(!log.mid(log.indexOf(error) + 1).contains(playing));
        QCOMPARE(backend.m_resumePositionMs, held);
        QCOMPARE(backend.m_pendingSeekMs, qint64{-1});
        QVERIFY(backend.m_softPaused);
        GstState settled = GST_STATE_NULL;
        gst_element_get_state(backend.m_playbin, &settled, nullptr, 2 * GST_SECOND);
        QVERIFY(settled == GST_STATE_READY || settled == GST_STATE_NULL);
        backend.pollBus();
        QCOMPARE(errors.count(), 1);
        QCOMPARE(backend.state(), PlaybackBackend::State::Error);

        backend.resume();
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(backend.position() > held + 300, 3000);
        backend.stop();
    }

    // A pipeline error during the restore whose in-place recovery is declined
    // (a repeat error at the same spot) must release the restore it interrupts.
    void nativeRestoreDecodeErrorFailsClosed()
    {
        QTemporaryDir dir;
        const QString tone = dir.filePath(QStringLiteral("decode.wav"));
        QVERIFY(writeToneWav(tone, 440.0, 30'000));
        GStreamerPlaybackBackend backend;
        PlaybackProfile profile;
        profile.releaseSinkOnPause = true;
        backend.setProfile(profile);
        backend.play(QUrl::fromLocalFile(tone));
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(backend.position() >= 1500, 3000);
        backend.pause();
        backend.m_sinkFactory = QStringLiteral("pipewiresink");
        QSignalSpy errors(&backend, &PlaybackBackend::errorOccurred);
        backend.seek(5000);
        backend.resume();
        QVERIFY(backend.m_nativePositionRestore);
        backend.m_lastRecoveryUri = backend.m_playingUri;
        backend.m_lastRecoveryPositionMs = backend.m_positionMs;
        GError *error = g_error_new_literal(GST_CORE_ERROR, GST_CORE_ERROR_FAILED, "decode failure");
        GstMessage *message = gst_message_new_error(GST_OBJECT(backend.m_playbin), error, "probe");
        g_error_free(error);
        backend.handleMessage(message);
        gst_message_unref(message);

        QCOMPARE(errors.count(), 1);
        QCOMPARE(errors.first().first().toString(), QStringLiteral("decode failure"));
        QCOMPARE(backend.state(), PlaybackBackend::State::Error);
        QVERIFY(!backend.m_nativePositionRestore);
        gboolean muted = TRUE;
        g_object_get(G_OBJECT(backend.m_playbin), "mute", &muted, nullptr);
        QVERIFY(!muted);

        backend.resume();
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(backend.position() > 5300, 3000);
        g_object_get(G_OBJECT(backend.m_playbin), "mute", &muted, nullptr);
        QVERIFY(!muted);
        backend.stop();
    }

    // What the native PipeWire transport is given, checked without needing
    // its plugin: the endpoint goes in target-object and endpoints come from
    // PipeWire's own device provider. Auto never claims a missing plugin.
    void transportsNameEndpointsTheirOwnWay()
    {
        QCOMPARE(QByteArray(GStreamerPlaybackBackend::streamTargetProperty(QStringLiteral("pipewiresink"))),
                 QByteArray("target-object"));
        QCOMPARE(QByteArray(GStreamerPlaybackBackend::streamTargetProperty(QStringLiteral("pulsesink"))),
                 QByteArray("device"));
        QVERIFY(GStreamerPlaybackBackend::streamTargetProperty(QStringLiteral("alsasink")) == nullptr);
        QCOMPARE(OutputEndpointMonitor::providerFor(QStringLiteral("pipewiresink")),
                 QStringLiteral("pipewiredeviceprovider"));
        QCOMPARE(OutputEndpointMonitor::providerFor(QStringLiteral("pulsesink")),
                 QStringLiteral("pulsedeviceprovider"));
        QVERIFY(OutputEndpointMonitor::providerFor(QStringLiteral("alsasink")).isEmpty());

        gst_init(nullptr, nullptr);
        GstElementFactory *native = gst_element_factory_find("pipewiresink");
        const QString autoFactory = GStreamerPlaybackBackend::sharedSinkFactory(QStringLiteral("auto"));
        if (native != nullptr) {
            gst_object_unref(native);
            QCOMPARE(autoFactory, QStringLiteral("pipewiresink"));
        } else {
            QVERIFY(GStreamerPlaybackBackend::sharedSinkFactory(QStringLiteral("pipewire")).isEmpty());
            QVERIFY(autoFactory != QStringLiteral("pipewiresink"));
        }
    }

    void outputStatusIsHonestAboutDestinations()
    {
        // The silent test sink cannot route to an endpoint: a configured one
        // is reported as unsupported, never as honored.
        GStreamerPlaybackBackend backend;
        PlaybackProfile profile;
        backend.setProfile(profile);
        QCOMPARE(backend.outputStatus().route, OutputStatus::Route::System);
        QCOMPARE(backend.outputStatus().transport, QStringLiteral("silent"));
        profile.sharedEndpoint = QStringLiteral("some_sink");
        profile.sharedEndpointLabel = QStringLiteral("Some sink");
        backend.setProfile(profile);
        QCOMPARE(backend.outputStatus().route, OutputStatus::Route::Unsupported);
        QCOMPARE(backend.outputStatus().preferredLabel, QStringLiteral("Some sink"));

        QtPlaybackBackend qtBackend;
        QVERIFY(!qtBackend.supportsSharedEndpoint());
        QSignalSpy info(&qtBackend, &PlaybackBackend::technicalInfoChanged);
        qtBackend.setProfile(profile);
        QCOMPARE(qtBackend.outputStatus().route, OutputStatus::Route::Unsupported);
        QVERIFY(info.last().first().toString().contains(QStringLiteral("cannot choose an output")));
        profile.sharedEndpoint.clear();
        qtBackend.setProfile(profile);
        QCOMPARE(qtBackend.outputStatus().route, OutputStatus::Route::System);
    }

    // A direct open asks which card is selected right now and waits while
    // it is absent, resuming only the intent the user left it with.
    void directOpenWaitsForItsCard()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("direct.wav"));
        QVERIFY(writeToneWav(path, 440.0, 5000));

        GStreamerPlaybackBackend backend;
        PlaybackProfile profile;
        profile.mode = QStringLiteral("bit-perfect");
        profile.sink = QStringLiteral("alsa");
        profile.device = QStringLiteral("hw:2");
        profile.deviceId = QStringLiteral("alsa_card.a");
        backend.setProfile(profile);
        bool present = false;
        int checks = 0;
        backend.setDirectDeviceCheck([&](const QString &stableId, const QString &) {
            if (stableId != QStringLiteral("alsa_card.a")) {
                return PlaybackBackend::DirectDevice{};
            }
            ++checks;
            PlaybackBackend::DirectDevice device;
            device.available = present;
            device.hwPath = present ? QStringLiteral("hw:3") : QString();
            device.label = QStringLiteral("Card A");
            return device;
        });

        backend.play(QUrl::fromLocalFile(path));
        QCOMPARE(backend.state(), PlaybackBackend::State::Buffering);
        QCOMPARE(backend.outputStatus().route, OutputStatus::Route::Waiting);
        QCOMPARE(backend.outputStatus().preferredLabel, QStringLiteral("Card A"));
        backend.retryDirectDevice();
        QCOMPARE(backend.state(), PlaybackBackend::State::Buffering);

        present = true;
        backend.retryDirectDevice();
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 5000);
        QCOMPARE(backend.outputStatus().route, OutputStatus::Route::Direct);

        // Bit-perfect pauses release the card, so a resume checks it again.
        backend.pause();
        present = false;
        const int before = checks;
        backend.resume();
        QVERIFY(checks > before);
        QCOMPARE(backend.state(), PlaybackBackend::State::Buffering);
        backend.pause();
        QCOMPARE(backend.state(), PlaybackBackend::State::Paused);
        present = true;
        backend.retryDirectDevice();
        QCOMPARE(backend.state(), PlaybackBackend::State::Paused);
        backend.resume();
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 5000);
        backend.stop();
    }

    void nativeDsdStartWaitsForItsCard()
    {
        GStreamerPlaybackBackend backend;
        PlaybackProfile profile;
        profile.deviceId = QStringLiteral("alsa_card.a");
        backend.setProfile(profile);
        backend.setDirectDeviceCheck([](const QString &, const QString &) {
            return PlaybackBackend::DirectDevice{};
        });
        backend.setOutputMode(PlaybackBackend::OutputMode::NativeDsd, QStringLiteral("hw:2"));
        backend.play(QUrl::fromLocalFile(QStringLiteral("/nonexistent/track.dsf")));
        QCOMPARE(backend.state(), PlaybackBackend::State::Buffering);
        QVERIFY(backend.hasSource());
        QCOMPARE(backend.outputStatus().route, OutputStatus::Route::Waiting);
        backend.resume();
        QCOMPARE(backend.state(), PlaybackBackend::State::Buffering);
        backend.stop();
        QCOMPARE(backend.state(), PlaybackBackend::State::Stopped);
        QVERIFY(!backend.hasSource());
    }

    void shortSuccessorCannotCancelPendingHandoff()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString firstPath = dir.filePath(QStringLiteral("short-one.wav"));
        const QString secondPath = dir.filePath(QStringLiteral("short-two.wav"));
        QVERIFY(writeToneWav(firstPath, 440.0, 1200));
        QVERIFY(writeToneWav(secondPath, 660.0, 1200));

        GStreamerPlaybackBackend backend;
        QSignalSpy advanced(&backend, &PlaybackBackend::preparedTrackStarted);
        QSignalSpy finished(&backend, &PlaybackBackend::finished);
        QSignalSpy errors(&backend, &PlaybackBackend::errorOccurred);
        QElapsedTimer elapsed;
        qint64 advanceAtMs = -1;
        qint64 positionAtAdvance = -1;
        connect(&backend, &PlaybackBackend::preparedTrackStarted, this, [&]() {
            advanceAtMs = elapsed.elapsed();
            positionAtAdvance = backend.position();
        });
        elapsed.start();
        backend.play(QUrl::fromLocalFile(firstPath));
        backend.prepareNext(QUrl::fromLocalFile(secondPath));

        QTRY_COMPARE_WITH_TIMEOUT(advanced.count(), 1, 5000);
        QVERIFY2(advanceAtMs >= 800,
                 qPrintable(QStringLiteral("handoff committed %1 ms after play, before the successor was audible")
                                .arg(advanceAtMs)));
        QCOMPARE(positionAtAdvance, 0);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 5000);
        QCOMPARE(errors.count(), 0);
    }

    void lateStopSuppressionCancelsConsumedSuccessor()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString firstPath = dir.filePath(QStringLiteral("late-one.wav"));
        const QString secondPath = dir.filePath(QStringLiteral("late-two.wav"));
        QVERIFY(writeToneWav(firstPath, 440.0, 1800));
        QVERIFY(writeToneWav(secondPath, 660.0, 5000));

        auto *backend = new GStreamerPlaybackBackend;
        PlayerCore player(backend);
        Track first;
        first.path = firstPath;
        player.resetQueue({first});
        QSignalSpy errors(backend, &PlaybackBackend::errorOccurred);
        bool suppressionArmed = false;
        connect(backend, &PlaybackBackend::aboutToNeedNext, this, [&]() {
            if (!suppressionArmed) {
                suppressionArmed = true;
                backend->stabilizeGaplessHandoff();
                backend->setGaplessStopPending(true);
            }
        });
        QStringList finishedTracks;
        connect(&player, &PlayerCore::trackFinished, this,
                [&](const Track &track) { finishedTracks.push_back(track.path); });

        player.playAt(0);
        backend->prepareNext(QUrl::fromLocalFile(secondPath));

        QTRY_VERIFY_WITH_TIMEOUT(suppressionArmed, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!finishedTracks.isEmpty(), 8000);
        QCOMPARE(finishedTracks, QStringList({firstPath}));
        QCOMPARE(player.currentTrack().path, firstPath);
        QCOMPARE(backend->state(), PlaybackBackend::State::Stopped);
        QCOMPARE(errors.count(), 0);
    }

    void gaplessHandoffsCommitEverySuccessorAtZero()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QVector<QUrl> urls;
        for (const auto [name, frequency] : {
                 std::pair{QStringLiteral("one.wav"), 440.0},
                 std::pair{QStringLiteral("two.wav"), 660.0},
                 std::pair{QStringLiteral("three.wav"), 880.0},
            }) {
            const QString path = dir.filePath(name);
            QVERIFY(writeToneWav(path, frequency, 2500));
            urls.push_back(QUrl::fromLocalFile(path));
        }

        GStreamerPlaybackBackend backend;
        QSignalSpy errors(&backend, &PlaybackBackend::errorOccurred);
        QSignalSpy finished(&backend, &PlaybackBackend::finished);
        QVector<qint64> positionsAtAdvance;
        int current = 0;
        connect(&backend, &PlaybackBackend::preparedTrackStarted, this, [&]() {
            ++current;
            positionsAtAdvance.push_back(backend.position());
            if (current + 1 < urls.size()) {
                backend.prepareNext(urls.at(current + 1));
            } else {
                backend.prepareNext({});
            }
        });

        backend.play(urls.at(0));
        backend.prepareNext(urls.at(1));

        QTRY_COMPARE_WITH_TIMEOUT(current, 2, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 8000);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(positionsAtAdvance, QVector<qint64>({0, 0}));
        QCOMPARE(backend.state(), PlaybackBackend::State::Stopped);
    }

    void pipelineRebuildInvalidatesArmedHandoff()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString firstPath = dir.filePath(QStringLiteral("rebuild-one.wav"));
        const QString secondPath = dir.filePath(QStringLiteral("rebuild-two.wav"));
        QVERIFY(writeToneWav(firstPath, 440.0, 2500));
        QVERIFY(writeToneWav(secondPath, 660.0, 2500));

        GStreamerPlaybackBackend backend;
        QSignalSpy needNext(&backend, &PlaybackBackend::aboutToNeedNext);
        QSignalSpy advanced(&backend, &PlaybackBackend::preparedTrackStarted);
        backend.play(QUrl::fromLocalFile(firstPath));
        backend.prepareNext(QUrl::fromLocalFile(secondPath));
        QTRY_VERIFY_WITH_TIMEOUT(needNext.count() >= 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(backend.position() > 0, 1000);

        PlaybackProfile changed;
        changed.softwareVolume = false;
        backend.setProfile(changed);
        QVERIFY(!backend.hasSource());
        QCOMPARE(backend.position(), 0);
        QTest::qWait(2800);
        QCOMPARE(advanced.count(), 0);
    }

    void pauseDuringArmedHandoffResumesTheAudibleTrack()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString firstPath = dir.filePath(QStringLiteral("pause-one.wav"));
        const QString secondPath = dir.filePath(QStringLiteral("pause-two.wav"));
        QVERIFY(writeToneWav(firstPath, 440.0, 2500));
        QVERIFY(writeToneWav(secondPath, 660.0, 2500));

        GStreamerPlaybackBackend backend;
        QSignalSpy needNext(&backend, &PlaybackBackend::aboutToNeedNext);
        QSignalSpy advanced(&backend, &PlaybackBackend::preparedTrackStarted);
        qint64 positionAtAdvance = -1;
        connect(&backend, &PlaybackBackend::preparedTrackStarted, this, [&]() {
            positionAtAdvance = backend.position();
        });
        backend.play(QUrl::fromLocalFile(firstPath));
        backend.prepareNext(QUrl::fromLocalFile(secondPath));
        QTRY_VERIFY_WITH_TIMEOUT(needNext.count() >= 1, 3000);

        backend.pause();
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Paused, 3000);
        QTest::qWait(2800);
        QCOMPARE(advanced.count(), 0);

        backend.resume();
        QTRY_COMPARE_WITH_TIMEOUT(advanced.count(), 1, 5000);
        QCOMPARE(positionAtAdvance, 0);
    }

    void playerCorePresentsEveryTrackOnceInAudibleOrder()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        QVector<Track> tracks;
        for (const auto [name, frequency] : {
                 std::pair{QStringLiteral("queue-one.wav"), 440.0},
                 std::pair{QStringLiteral("queue-two.wav"), 660.0},
                 std::pair{QStringLiteral("queue-three.wav"), 880.0},
             }) {
            Track track;
            track.path = dir.filePath(name);
            track.title = name;
            QVERIFY(writeToneWav(track.path, frequency, 2500));
            tracks.push_back(track);
        }

        auto *backend = new GStreamerPlaybackBackend;
        PlayerCore player(backend);
        player.resetQueue(tracks);
        QStringList presented;
        QVector<qint64> positionsAtPresentation;
        QStringList finished;
        connect(&player, &PlayerCore::currentTrackChanged, this,
                [&](const Track &track, bool) {
                    presented.push_back(track.path);
                    positionsAtPresentation.push_back(backend->position());
                });
        connect(&player, &PlayerCore::trackFinished, this,
                [&](const Track &track) { finished.push_back(track.path); });

        player.playAt(0);

        QTRY_COMPARE_WITH_TIMEOUT(player.queueIndex(), 2, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(backend->state(), PlaybackBackend::State::Stopped, 5000);
        QCOMPARE(presented, QStringList({tracks.at(0).path, tracks.at(1).path, tracks.at(2).path}));
        QCOMPARE(positionsAtPresentation, QVector<qint64>({0, 0, 0}));
        QCOMPARE(finished, QStringList({tracks.at(0).path, tracks.at(1).path, tracks.at(2).path}));
    }

    void playNextDuringArmedHandoffChangesTheAudibleSuccessor()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        const auto makeTrack = [&](const QString &name, double frequency) {
            Track track;
            track.path = dir.filePath(name);
            track.title = name;
            if (!writeToneWav(track.path, frequency, 2500)) {
                track.path.clear();
            }
            return track;
        };
        const Track first = makeTrack(QStringLiteral("mutate-one.wav"), 440.0);
        const Track originalNext = makeTrack(QStringLiteral("mutate-two.wav"), 660.0);
        const Track last = makeTrack(QStringLiteral("mutate-three.wav"), 880.0);
        const Track playNext = makeTrack(QStringLiteral("mutate-inserted.wav"), 550.0);
        QVERIFY(!first.path.isEmpty());
        QVERIFY(!originalNext.path.isEmpty());
        QVERIFY(!last.path.isEmpty());
        QVERIFY(!playNext.path.isEmpty());

        auto *backend = new GStreamerPlaybackBackend;
        PlayerCore player(backend);
        player.resetQueue({first, originalNext, last});
        QSignalSpy needNext(backend, &PlaybackBackend::aboutToNeedNext);
        QStringList presented;
        connect(&player, &PlayerCore::currentTrackChanged, this,
                [&](const Track &track, bool) { presented.push_back(track.path); });
        player.playAt(0);
        QTRY_VERIFY_WITH_TIMEOUT(needNext.count() >= 1, 3000);

        player.playTracksNext({playNext});

        QTRY_COMPARE_WITH_TIMEOUT(presented.size(), 4, 14000);
        QTRY_COMPARE_WITH_TIMEOUT(backend->state(), PlaybackBackend::State::Stopped, 5000);
        QCOMPARE(presented,
                 QStringList({first.path, playNext.path, originalNext.path, last.path}));
    }
};

QTEST_GUILESS_MAIN(GStreamerPlaybackTest)
#include "test_gstreamer_playback.moc"
