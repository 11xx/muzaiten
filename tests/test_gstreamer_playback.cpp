#include "playback/GStreamerPlaybackBackend.h"

#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest/QtTest>

#include <cmath>

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
};

QTEST_GUILESS_MAIN(GStreamerPlaybackTest)
#include "test_gstreamer_playback.moc"
