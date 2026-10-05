#include "PrivateAudioGraph.h"

#include <QDataStream>
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
