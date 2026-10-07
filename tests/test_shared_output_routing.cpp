#include "PrivateAudioGraph.h"

#include <QDataStream>
#include <QProcess>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QMutex>
#include <QSignalSpy>
#include <QTimer>
#include <QUrl>
#include <QtTest/QtTest>

#include <gst/gst.h>

#include <cmath>
#include <memory>

#define private public
#include "playback/GStreamerPlaybackBackend.h"
#undef private
#include "playback/OutputEndpointMonitor.h"
#include "playback/PipeWireStreamWatch.h"

namespace {

bool writeToneWav(const QString &path, quint32 sampleRate, int durationMs)
{
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
        const double phase = tau * 440.0 * static_cast<double>(sample) / static_cast<double>(sampleRate);
        out << static_cast<qint16>(std::sin(phase) * 12000.0);
    }
    return out.status() == QDataStream::Ok;
}

PlaybackProfile pulseProfile()
{
    PlaybackProfile profile;
    profile.sink = QStringLiteral("pulse");
    profile.sharedSink = profile.sink;
    return profile;
}

// A profile on the named transport ("pulse" or "pipewire").
PlaybackProfile transportProfile(const QString &transport)
{
    PlaybackProfile profile;
    profile.sink = transport;
    profile.sharedSink = transport;
    return profile;
}

bool sinkInstalled(const char *factoryName)
{
    GstElementFactory *factory = gst_element_factory_find(factoryName);
    if (factory == nullptr) {
        return false;
    }
    gst_object_unref(factory);
    return true;
}

// The native transport needs GStreamer's PipeWire plugin, which may be absent.
#define SKIP_UNLESS_TRANSPORT(transport)                                                        \
    if ((transport) == QStringLiteral("pipewire") && !sinkInstalled("pipewiresink")) {          \
        QSKIP("GStreamer's PipeWire plugin (pipewiresink) is not installed");                    \
    }

} // namespace

// Exercises the real GStreamer backend and pulsesink against a private
// PipeWire graph, asserting where streams are actually linked.
class SharedOutputRoutingTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qunsetenv("MUZAITEN_DEMO_SILENT_AUDIO");
        m_graph = std::make_unique<PrivateAudioGraph>();
        QString reason;
        if (!m_graph->start(&reason)) {
            m_graph.reset();
            QSKIP(qPrintable(reason));
        }
        gst_init(nullptr, nullptr);
        GstElementFactory *pulse = gst_element_factory_find("pulsesink");
        if (pulse == nullptr) {
            m_graph.reset();
            QSKIP("GStreamer pulsesink is not installed");
        }
        gst_object_unref(pulse);
        QVERIFY(m_dir.isValid());
        QVERIFY(writeToneWav(path("long-a"), 8000, 30'000));
        QVERIFY(writeToneWav(path("long-b"), 8000, 30'000));
        QVERIFY(writeToneWav(path("short-8k"), 8000, 3'000));
        QVERIFY(writeToneWav(path("long-16k"), 16000, 30'000));
    }

    void cleanupTestCase()
    {
        m_graph.reset();
    }

    void init()
    {
        // WirePlumber remembers this application's last chosen sink by name;
        // fresh names keep one test's mixer move out of the next test.
        ++m_round;
        m_main = QStringLiteral("muz_main_%1").arg(m_round);
        m_other = QStringLiteral("muz_other_%1").arg(m_round);
        m_modules.clear();
        m_modules.insert(m_main, m_graph->addSink(m_main));
        m_modules.insert(m_other, m_graph->addSink(m_other));
        m_graph->setDefaultSink(m_main);
    }

    void cleanup()
    {
        for (const QString &module : std::as_const(m_modules)) {
            m_graph->removeModule(module);
        }
        QVERIFY(PrivateAudioGraph::waitFor([&] { return m_graph->streams().isEmpty(); }));
    }

    // A stream the session manager moved to a fallback while its sink was
    // missing must not carry that fallback into the next stream the app opens:
    // once the sink returns, playback goes back to it.
    void appOpenedStreamDoesNotInheritFallback()
    {
        GStreamerPlaybackBackend backend;
        backend.setProfile(pulseProfile());
        QSignalSpy errors(&backend, &PlaybackBackend::errorOccurred);
        backend.play(url("long-a"));
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), m_main, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 8000);

        m_graph->removeModule(m_modules.take(m_main));
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), m_other, 8000);

        const int fallbackStream = m_graph->streamIndex();
        backend.play(url("long-b"));
        QTRY_VERIFY_WITH_TIMEOUT(m_graph->streamIndex() >= 0 && m_graph->streamIndex() != fallbackStream, 8000);
        QCOMPARE(m_graph->requestedTarget(), QString());

        m_modules.insert(m_main, m_graph->addSink(m_main));
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), m_main, 8000);
        QVERIFY(m_graph->carriesAudio(m_main));
        QCOMPARE(errors.count(), 0);
        backend.stop();
    }

    // The same for a stream the sink reopens by itself when the next gapless
    // track has a different sample rate.
    void formatReopenDoesNotInheritFallback()
    {
        GStreamerPlaybackBackend backend;
        backend.setProfile(pulseProfile());
        QSignalSpy errors(&backend, &PlaybackBackend::errorOccurred);
        QSignalSpy advanced(&backend, &PlaybackBackend::preparedTrackStarted);
        backend.play(url("short-8k"));
        backend.prepareNext(url("long-16k"));
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), m_main, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 8000);

        m_graph->removeModule(m_modules.take(m_main));
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), m_other, 8000);

        const int fallbackStream = m_graph->streamIndex();
        QTRY_COMPARE_WITH_TIMEOUT(advanced.count(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(m_graph->streamIndex() >= 0 && m_graph->streamIndex() != fallbackStream, 8000);
        QCOMPARE(m_graph->requestedTarget(), QString());

        m_modules.insert(m_main, m_graph->addSink(m_main));
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), m_main, 8000);
        QCOMPARE(errors.count(), 0);
        backend.stop();
    }

    void mixerMoveSurvivesReopen_data()
    {
        QTest::addColumn<bool>("wiremix");
        QTest::newRow("wiremix metadata move") << true;
        QTest::newRow("pulse protocol move") << false;
    }

    // A deliberate mixer move is the session manager's remembered choice for
    // this application. The next stream the app opens must land there, even
    // when the sink element still remembers the earlier sink: the element's
    // memory can miss a move, and it must not decide the route.
    void mixerMoveSurvivesReopen()
    {
        QFETCH(bool, wiremix);
        PlaybackProfile profile = pulseProfile();
        profile.releaseSinkOnPause = true;
        profile.sharedReleaseSinkOnPause = true;
        GStreamerPlaybackBackend backend;
        backend.setProfile(profile);
        QSignalSpy errors(&backend, &PlaybackBackend::errorOccurred);
        backend.play(url("long-a"));
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), m_main, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 8000);

        if (wiremix) {
            m_graph->mixerMove(m_other);
        } else {
            m_graph->pulseMove(m_other);
        }
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), m_other, 8000);

        backend.pause();
        QTRY_VERIFY_WITH_TIMEOUT(m_graph->streams().isEmpty(), 8000);
        // No stream exists while soft-paused, so this only rewrites what the
        // element remembers; it moves nothing.
        g_object_set(G_OBJECT(backend.m_audioSink), "device", m_main.toUtf8().constData(), nullptr);

        backend.resume();
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), m_other, 8000);
        QCOMPARE(m_graph->requestedTarget(), QString());
        QVERIFY(m_graph->carriesAudio(m_other));
        QCOMPARE(errors.count(), 0);
        backend.stop();
    }

    // A preferred endpoint, here a virtual sink, is requested by every
    // stream, whatever happens to the default output and to other devices.
    void preferredEndpointHoldsThroughDeviceChurn()
    {
        const QString virtualSink = QStringLiteral("muz_virtual_%1").arg(m_round);
        m_modules.insert(virtualSink, m_graph->addSink(virtualSink, QStringLiteral("Virtual mix")));
        PlaybackProfile profile = pulseProfile();
        profile.sharedEndpoint = virtualSink;
        GStreamerPlaybackBackend backend;
        backend.setProfile(profile);
        QSignalSpy errors(&backend, &PlaybackBackend::errorOccurred);
        QSignalSpy adopted(&backend, &PlaybackBackend::sharedEndpointChosenExternally);
        QSignalSpy advanced(&backend, &PlaybackBackend::preparedTrackStarted);
        backend.play(url("short-8k"));
        backend.prepareNext(url("long-16k"));
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), virtualSink, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 8000);
        QCOMPARE(m_graph->requestedTarget(), virtualSink);
        QCOMPARE(backend.outputStatus().route, OutputStatus::Route::Preferred);
        QCOMPARE(backend.outputStatus().preferredLabel, QStringLiteral("Virtual mix"));
        QVERIFY(m_graph->carriesAudio(virtualSink));

        // A new device becomes the default, and a capture-only one appears.
        const QString newcomer = QStringLiteral("muz_new_%1").arg(m_round);
        m_modules.insert(newcomer, m_graph->addSink(newcomer));
        m_graph->setDefaultSink(newcomer);
        m_modules.insert(QStringLiteral("source"),
                         m_graph->run({QStringLiteral("pactl"), QStringLiteral("load-module"),
                                       QStringLiteral("module-null-source"),
                                       QStringLiteral("source_name=muz_capture_%1").arg(m_round)}));

        // The next track has another sample rate, so the sink reopens.
        const int firstStream = m_graph->streamIndex();
        QTRY_COMPARE_WITH_TIMEOUT(advanced.count(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(m_graph->streamIndex() >= 0 && m_graph->streamIndex() != firstStream, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), virtualSink, 8000);
        QCOMPARE(m_graph->requestedTarget(), virtualSink);
        QCOMPARE(adopted.count(), 0);
        QCOMPARE(errors.count(), 0);
        backend.stop();
    }

    // Playback waits for a missing preferred endpoint: it never opens
    // elsewhere, keeps its position, and resumes only if it was meant to play.
    void missingPreferredEndpointWaits_data()
    {
        QTest::addColumn<QString>("transport");
        QTest::newRow("pulse") << QStringLiteral("pulse");
        QTest::newRow("native") << QStringLiteral("pipewire");
    }

    void missingPreferredEndpointWaits()
    {
        QFETCH(QString, transport);
        SKIP_UNLESS_TRANSPORT(transport);
        const QString late = QStringLiteral("muz_late_%1").arg(m_round);
        PlaybackProfile profile = transportProfile(transport);
        profile.sharedEndpoint = late;
        profile.sharedEndpointLabel = QStringLiteral("Late device");
        GStreamerPlaybackBackend backend;
        backend.setProfile(profile);
        QSignalSpy errors(&backend, &PlaybackBackend::errorOccurred);

        backend.play(url("long-a"));
        QCOMPARE(backend.state(), PlaybackBackend::State::Buffering);
        QCOMPARE(backend.outputStatus().route, OutputStatus::Route::Waiting);
        QCOMPARE(backend.outputStatus().preferredLabel, QStringLiteral("Late device"));
        QTest::qWait(500);
        QVERIFY(m_graph->streams().isEmpty());
        // Another track picked while waiting replaces the held one.
        backend.play(url("long-b"));
        QCOMPARE(backend.state(), PlaybackBackend::State::Buffering);
        QVERIFY(m_graph->streams().isEmpty());

        m_modules.insert(late, m_graph->addSink(late));
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 8000);
        QVERIFY(backend.m_playingUri.endsWith(QStringLiteral("long-b.wav")));
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), late, 8000);
        QTRY_VERIFY_WITH_TIMEOUT(backend.position() >= 1500, 8000);

        m_graph->removeModule(m_modules.take(late));
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Buffering, 8000);
        QTRY_VERIFY_WITH_TIMEOUT(m_graph->streams().isEmpty(), 8000);
        QVERIFY(backend.outputStatus().currentEndpoint.isEmpty());
        const qint64 held = backend.position();
        QVERIFY2(held >= 1500, qPrintable(QString::number(held)));

        // Paused while waiting: the endpoint's return must not start playback.
        backend.pause();
        QCOMPARE(backend.state(), PlaybackBackend::State::Paused);
        m_modules.insert(late, m_graph->addSink(late));
        QTest::qWait(1000);
        QCOMPARE(backend.state(), PlaybackBackend::State::Paused);
        QVERIFY(m_graph->streams().isEmpty());

        backend.resume();
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), late, 8000);
        QVERIFY2(backend.position() >= held - 250, qPrintable(QString::number(backend.position())));
        QCOMPARE(backend.outputStatus().preferredEndpoint, late);
        QCOMPARE(errors.count(), 0);
        backend.stop();
    }

    // With fallback chosen, the system route plays while the preferred
    // endpoint is missing, and the stream returns when it comes back. The
    // fallback is never recorded as the preference.
    void fallbackPlaysUntilPreferredReturns_data()
    {
        QTest::addColumn<QString>("transport");
        QTest::newRow("pulse") << QStringLiteral("pulse");
        QTest::newRow("native") << QStringLiteral("pipewire");
    }

    void fallbackPlaysUntilPreferredReturns()
    {
        QFETCH(QString, transport);
        SKIP_UNLESS_TRANSPORT(transport);
        const QString preferred = QStringLiteral("muz_pref_%1").arg(m_round);
        m_modules.insert(preferred, m_graph->addSink(preferred));
        PlaybackProfile profile = transportProfile(transport);
        profile.sharedEndpoint = preferred;
        profile.sharedEndpointFallback = true;
        GStreamerPlaybackBackend backend;
        backend.setProfile(profile);
        QSignalSpy adopted(&backend, &PlaybackBackend::sharedEndpointChosenExternally);
        QSignalSpy errors(&backend, &PlaybackBackend::errorOccurred);
        backend.play(url("long-a"));
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), preferred, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(backend.outputStatus().currentEndpoint, preferred, 8000);

        m_graph->removeModule(m_modules.take(preferred));
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), m_main, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(backend.outputStatus().currentEndpoint, m_main, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(backend.outputStatus().route, OutputStatus::Route::Fallback, 8000);
        QCOMPARE(backend.state(), PlaybackBackend::State::Playing);
        QTest::qWait(1500);

        m_modules.insert(preferred, m_graph->addSink(preferred));
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), preferred, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(backend.outputStatus().route, OutputStatus::Route::Preferred, 8000);
        QTest::qWait(1500);
        QCOMPARE(adopted.count(), 0);
        QCOMPARE(backend.outputStatus().preferredEndpoint, preferred);
        QCOMPARE(errors.count(), 0);
        backend.stop();
    }

    void endpointLostDuringStartup_data()
    {
        QTest::addColumn<bool>("fallback");
        QTest::addColumn<QString>("transport");
        QTest::newRow("fallback, pulse") << true << QStringLiteral("pulse");
        QTest::newRow("wait, pulse") << false << QStringLiteral("pulse");
        QTest::newRow("fallback, native") << true << QStringLiteral("pipewire");
        QTest::newRow("wait, native") << false << QStringLiteral("pipewire");
    }

    // The preferred endpoint vanishing while the stream is still starting
    // must neither wedge the transport nor lose the intent to play.
    void endpointLostDuringStartup()
    {
        QFETCH(bool, fallback);
        QFETCH(QString, transport);
        SKIP_UNLESS_TRANSPORT(transport);
        const QString preferred = QStringLiteral("muz_pref_%1").arg(m_round);
        m_modules.insert(preferred, m_graph->addSink(preferred));
        PlaybackProfile profile = transportProfile(transport);
        profile.sharedEndpoint = preferred;
        profile.sharedEndpointFallback = fallback;
        GStreamerPlaybackBackend backend;
        backend.setProfile(profile);
        backend.play(url("long-a"));
        m_graph->removeModule(m_modules.take(preferred));
        if (fallback) {
            QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 10000);
            QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), m_main, 8000);
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Buffering, 10000);
            QTRY_VERIFY_WITH_TIMEOUT(m_graph->streams().isEmpty(), 8000);
        }
        m_modules.insert(preferred, m_graph->addSink(preferred));
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), preferred, 8000);
        backend.stop();
    }

    void mixerMoveOffPreferredIsAdopted_data()
    {
        QTest::addColumn<bool>("wiremix");
        QTest::newRow("wiremix metadata move") << true;
        QTest::newRow("pulse protocol move") << false;
    }

    // Moving the stream off an available preferred endpoint in a mixer is a
    // choice made outside the app; it becomes the preference.
    void mixerMoveOffPreferredIsAdopted()
    {
        QFETCH(bool, wiremix);
        const QString preferred = QStringLiteral("muz_pref_%1").arg(m_round);
        m_modules.insert(preferred, m_graph->addSink(preferred));
        PlaybackProfile profile = pulseProfile();
        profile.sharedEndpoint = preferred;
        profile.releaseSinkOnPause = true;
        GStreamerPlaybackBackend backend;
        backend.setProfile(profile);
        QSignalSpy adopted(&backend, &PlaybackBackend::sharedEndpointChosenExternally);
        backend.play(url("long-a"));
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), preferred, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 8000);

        if (wiremix) {
            m_graph->mixerMove(m_other);
        } else {
            m_graph->pulseMove(m_other);
        }
        QTRY_COMPARE_WITH_TIMEOUT(adopted.count(), 1, 8000);
        QCOMPARE(adopted.first().at(0).toString(), m_other);
        QCOMPARE(backend.outputStatus().preferredEndpoint, m_other);

        backend.pause();
        QTRY_VERIFY_WITH_TIMEOUT(m_graph->streams().isEmpty(), 8000);
        backend.resume();
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), m_other, 8000);
        QCOMPARE(m_graph->requestedTarget(), m_other);
        backend.stop();
    }

    void mixerMoveHoldsAtImmediateReopen_data()
    {
        QTest::addColumn<bool>("wiremix");
        QTest::addColumn<QString>("transport");
        QTest::newRow("wiremix move, pulse") << true << QStringLiteral("pulse");
        QTest::newRow("pulse protocol move, pulse") << false << QStringLiteral("pulse");
        QTest::newRow("wiremix move, native") << true << QStringLiteral("pipewire");
        QTest::newRow("pulse protocol move, native") << false << QStringLiteral("pipewire");
    }

    // A mixer move off the preferred endpoint holds for a stream opened the
    // very next moment, before any confirmation could run, and then becomes
    // the preference.
    void mixerMoveHoldsAtImmediateReopen()
    {
        QFETCH(bool, wiremix);
        QFETCH(QString, transport);
        SKIP_UNLESS_TRANSPORT(transport);
        const QString preferred = QStringLiteral("muz_pref_%1").arg(m_round);
        m_modules.insert(preferred, m_graph->addSink(preferred));
        PlaybackProfile profile = transportProfile(transport);
        profile.sharedEndpoint = preferred;
        GStreamerPlaybackBackend backend;
        backend.setProfile(profile);
        QSignalSpy adopted(&backend, &PlaybackBackend::sharedEndpointChosenExternally);
        backend.play(url("long-a"));
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), preferred, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 8000);

        if (wiremix) {
            m_graph->mixerMove(m_other);
        } else {
            m_graph->pulseMove(m_other);
        }
        // No event processing from here until the next stream is open.
        QVERIFY(PrivateAudioGraph::waitFor([&] { return m_graph->route() == m_other; }));
        const int movedStream = m_graph->streamIndex();
        backend.play(url("long-b"));
        QVERIFY(PrivateAudioGraph::waitFor([&] {
            return m_graph->streamIndex() >= 0 && m_graph->streamIndex() != movedStream;
        }));
        QCOMPARE(m_graph->requestedTarget(), m_other);
        QVERIFY(PrivateAudioGraph::waitFor([&] { return m_graph->route() == m_other; }));

        QTRY_COMPARE_WITH_TIMEOUT(adopted.count(), 1, 5000);
        QCOMPARE(adopted.first().at(0).toString(), m_other);
        QCOMPARE(m_graph->route(), m_other);
        backend.stop();
    }

    // The same when the move lands just before the next gapless track changes
    // the sample rate, so the sink reopens its stream by itself while the
    // owner thread is busy.
    void mixerMoveHoldsAtImmediateFormatChange()
    {
        const QString preferred = QStringLiteral("muz_pref_%1").arg(m_round);
        m_modules.insert(preferred, m_graph->addSink(preferred));
        PlaybackProfile profile = pulseProfile();
        profile.sharedEndpoint = preferred;
        GStreamerPlaybackBackend backend;
        backend.setProfile(profile);
        QSignalSpy adopted(&backend, &PlaybackBackend::sharedEndpointChosenExternally);
        QElapsedTimer started;
        started.start();
        backend.play(url("short-8k"));
        backend.prepareNext(url("long-16k"));
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), preferred, 8000);
        QTRY_VERIFY_WITH_TIMEOUT(started.elapsed() >= 2300, 8000);

        // From the move until the reopened stream is checked, the owner
        // thread runs nothing: only the sink-side reopen can keep the move.
        m_graph->mixerMove(m_other);
        QVERIFY(PrivateAudioGraph::waitFor([&] { return m_graph->route() == m_other; }));
        const int movedStream = m_graph->streamIndex();
        QVERIFY(PrivateAudioGraph::waitFor([&] {
            return m_graph->streamIndex() >= 0 && m_graph->streamIndex() != movedStream;
        }, 6000));
        QCOMPARE(m_graph->requestedTarget(), m_other);
        QVERIFY(PrivateAudioGraph::waitFor([&] { return m_graph->route() == m_other; }));

        QTRY_COMPARE_WITH_TIMEOUT(adopted.count(), 1, 5000);
        QCOMPARE(backend.outputStatus().preferredEndpoint, m_other);
        backend.stop();
    }

    void forcedFallbackAtImmediateReopenIsNotAdopted_data()
    {
        QTest::addColumn<bool>("fallback");
        QTest::addColumn<QString>("transport");
        QTest::newRow("fallback, pulse") << true << QStringLiteral("pulse");
        QTest::newRow("wait, pulse") << false << QStringLiteral("pulse");
        QTest::newRow("fallback, native") << true << QStringLiteral("pipewire");
        QTest::newRow("wait, native") << false << QStringLiteral("pipewire");
    }

    // The preferred endpoint vanishes and a stream opens before the endpoint
    // monitor has said so: the stream may briefly follow the fallback, but
    // the fallback never becomes the preference.
    void forcedFallbackAtImmediateReopenIsNotAdopted()
    {
        QFETCH(bool, fallback);
        QFETCH(QString, transport);
        SKIP_UNLESS_TRANSPORT(transport);
        const QString preferred = QStringLiteral("muz_pref_%1").arg(m_round);
        m_modules.insert(preferred, m_graph->addSink(preferred));
        PlaybackProfile profile = transportProfile(transport);
        profile.sharedEndpoint = preferred;
        profile.sharedEndpointFallback = fallback;
        GStreamerPlaybackBackend backend;
        backend.setProfile(profile);
        QSignalSpy adopted(&backend, &PlaybackBackend::sharedEndpointChosenExternally);
        backend.play(url("long-a"));
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), preferred, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 8000);

        m_graph->removeModule(m_modules.take(preferred));
        QVERIFY(PrivateAudioGraph::waitFor([&] { return m_graph->route() == m_main; }));
        backend.play(url("long-b"));

        if (fallback) {
            QTRY_COMPARE_WITH_TIMEOUT(backend.outputStatus().route, OutputStatus::Route::Fallback, 8000);
            QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), m_main, 8000);
        } else {
            QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Buffering, 8000);
            QTRY_VERIFY_WITH_TIMEOUT(m_graph->streams().isEmpty(), 8000);
        }
        QTest::qWait(2000);
        QCOMPARE(adopted.count(), 0);
        QCOMPARE(backend.outputStatus().preferredEndpoint, preferred);

        m_modules.insert(preferred, m_graph->addSink(preferred));
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), preferred, 12000);
        QCOMPARE(adopted.count(), 0);
        backend.stop();
    }

    // The stream watch follows a native PipeWire stream, found by its node
    // name, through a mixer move and its end.
    void nativeStreamWatchFollowsLinks()
    {
        if (!PipeWireStreamWatch::supported()) {
            QSKIP("built without PipeWire support");
        }
        if (QStandardPaths::findExecutable(QStringLiteral("pw-cat")).isEmpty()) {
            QSKIP("pw-cat is not installed");
        }
        const QString name = QStringLiteral("muz-watch-%1").arg(m_round);
        PipeWireStreamWatch watch;
        QSignalSpy changed(&watch, &PipeWireStreamWatch::linkedSinkChanged);
        QVERIFY(watch.start(name));
        QProcess player;
        player.start(QStringLiteral("pw-cat"),
                     {QStringLiteral("--playback"), QStringLiteral("--target"), m_main,
                      QStringLiteral("-P"), QStringLiteral("{ node.name = \"%1\" }").arg(name), path("long-a")});
        QVERIFY(player.waitForStarted(3000));
        QTRY_COMPARE_WITH_TIMEOUT(watch.linkedSink(), m_main, 8000);
        m_graph->mixerMove(m_other);
        QTRY_COMPARE_WITH_TIMEOUT(watch.linkedSink(), m_other, 8000);
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty() && changed.last().first().toString() == m_other, 3000);
        player.kill();
        player.waitForFinished(3000);
        QTRY_COMPARE_WITH_TIMEOUT(watch.linkedSink(), QString(), 8000);
        watch.stop();
    }

    void nativeStreamWatchDropsEndedRuns_data()
    {
        QTest::addColumn<bool>("replaced");
        QTest::newRow("stopped") << false;
        QTest::newRow("replaced by another run") << true;
    }

    // A report the watch queued before it was stopped, or before a new run
    // replaced it, is never delivered.
    void nativeStreamWatchDropsEndedRuns()
    {
        QFETCH(bool, replaced);
        if (!PipeWireStreamWatch::supported()) {
            QSKIP("built without PipeWire support");
        }
        if (QStandardPaths::findExecutable(QStringLiteral("pw-cat")).isEmpty()) {
            QSKIP("pw-cat is not installed");
        }
        const QString name = QStringLiteral("muz-watch-%1").arg(m_round);
        PipeWireStreamWatch watch;
        QSignalSpy changed(&watch, &PipeWireStreamWatch::linkedSinkChanged);
        QVERIFY(watch.start(name));
        QProcess player;
        player.start(QStringLiteral("pw-cat"),
                     {QStringLiteral("--playback"), QStringLiteral("--target"), m_main,
                      QStringLiteral("-P"), QStringLiteral("{ node.name = \"%1\" }").arg(name), path("long-a")});
        QVERIFY(player.waitForStarted(3000));
        // The route is known to the watch, and its report is still queued.
        QVERIFY(PrivateAudioGraph::waitFor([&] { return watch.linkedSink() == m_main; }));
        if (replaced) {
            QVERIFY(watch.start(name + QStringLiteral("-other")));
        } else {
            watch.stop();
        }
        QTest::qWait(500);
        for (const QList<QVariant> &report : std::as_const(changed)) {
            QVERIFY2(report.first().toString() != m_main, "a report from an ended run was delivered");
        }
        player.kill();
        player.waitForFinished(3000);
        watch.stop();
    }

    // A device removal the provider reported just before the monitor was
    // restarted belongs to the old run: it must not remove the endpoint from
    // the new run's list, which has it.
    void endpointMonitorDropsEventsOfEndedRuns()
    {
        OutputEndpointMonitor monitor;
        QVERIFY(monitor.start(QStringLiteral("pulsesink")));
        QVERIFY(monitor.contains(m_other));
        QSignalSpy removed(&monitor, &OutputEndpointMonitor::endpointRemoved);

        m_graph->removeModule(m_modules.take(m_other));
        // The provider reports the removal on its own thread; it is queued here.
        QThread::msleep(800);
        monitor.stop();
        m_modules.insert(m_other, m_graph->addSink(m_other));
        QVERIFY(monitor.start(QStringLiteral("pulsesink")));
        QVERIFY(monitor.contains(m_other));

        QTest::qWait(500);
        QVERIFY(monitor.contains(m_other));
        QCOMPARE(removed.count(), 0);
        monitor.stop();
    }

    void outputChangeDropsQueuedObservations_data()
    {
        QTest::addColumn<QString>("transport");
        QTest::newRow("pulse") << QStringLiteral("pulse");
        QTest::newRow("native") << QStringLiteral("pipewire");
    }

    // A mixer move is seen but not yet delivered when the user picks another
    // output. The old observation must not come back as a move to adopt over
    // the newer choice.
    void outputChangeDropsQueuedObservations()
    {
        QFETCH(QString, transport);
        SKIP_UNLESS_TRANSPORT(transport);
        const QString preferred = QStringLiteral("muz_pref_%1").arg(m_round);
        const QString chosen = QStringLiteral("muz_chosen_%1").arg(m_round);
        m_modules.insert(preferred, m_graph->addSink(preferred));
        m_modules.insert(chosen, m_graph->addSink(chosen));
        PlaybackProfile profile = transportProfile(transport);
        profile.sharedEndpoint = preferred;
        GStreamerPlaybackBackend backend;
        backend.setProfile(profile);
        QSignalSpy adopted(&backend, &PlaybackBackend::sharedEndpointChosenExternally);
        backend.play(url("long-a"));
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), preferred, 8000);
        QTRY_COMPARE_WITH_TIMEOUT(backend.state(), PlaybackBackend::State::Playing, 8000);

        // No event processing from the move until the new output is chosen.
        m_graph->mixerMove(m_other);
        QVERIFY(PrivateAudioGraph::waitFor([&] { return m_graph->route() == m_other; }));
        QThread::msleep(300);
        profile.sharedEndpoint = chosen;
        backend.setProfile(profile);

        // As after an output change in the app, events run before the track
        // is reopened on the new output.
        QTest::qWait(1500);
        QCOMPARE(adopted.count(), 0);
        QCOMPARE(backend.outputStatus().preferredEndpoint, chosen);
        backend.play(url("long-b"));
        QTRY_COMPARE_WITH_TIMEOUT(m_graph->route(), chosen, 8000);
        QCOMPARE(m_graph->requestedTarget(), chosen);
        QTest::qWait(1500);
        QCOMPARE(adopted.count(), 0);
        QCOMPARE(m_graph->route(), chosen);
        backend.stop();
    }

private:
    QString path(const char *name) const
    {
        return m_dir.filePath(QString::fromLatin1(name) + QStringLiteral(".wav"));
    }

    QUrl url(const char *name) const
    {
        return QUrl::fromLocalFile(path(name));
    }

    std::unique_ptr<PrivateAudioGraph> m_graph;
    QTemporaryDir m_dir;
    QHash<QString, QString> m_modules;
    QString m_main;
    QString m_other;
    int m_round = 0;
};

QTEST_GUILESS_MAIN(SharedOutputRoutingTest)

#include "test_shared_output_routing.moc"
