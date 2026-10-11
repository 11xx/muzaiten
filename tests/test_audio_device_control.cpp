#include "AudioDeviceFixtures.h"

#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QFileSystemWatcher>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QtTest>

#define private public
#include "playback/DirectDeviceResolver.h"
#undef private

using Status = AudioDeviceControl::DirectTarget::Status;

class AudioDeviceControlTest final : public QObject {
    Q_OBJECT

private slots:
    // Card A is gone and card B now has A's old number: A's identity must
    // resolve to nothing, never to B.
    void absentIdentityNeverResolvesToTheReusedNumber()
    {
        FakeAudioGraph graph;
        graph.cards = {{41, 2, QStringLiteral("alsa_card.b"), QStringLiteral("Card B")}};
        const auto target = AudioDeviceControl::resolveDirect(QStringLiteral("alsa_card.a"), QStringLiteral("hw:2"));
        QCOMPARE(target.status, Status::Unavailable);
        QVERIFY(!AudioDeviceControl::findSelected(QStringLiteral("alsa_card.a"), QStringLiteral("hw:2")));
        QVERIFY(AudioDeviceControl::findByHwPath(QStringLiteral("hw:2")).has_value());
    }

    void identityFollowsTheCardToItsNewNumber()
    {
        FakeAudioGraph graph;
        graph.cards = {{41, 2, QStringLiteral("alsa_card.b"), QStringLiteral("Card B")},
                       {42, 3, QStringLiteral("alsa_card.a"), QStringLiteral("Card A")}};
        const auto target = AudioDeviceControl::resolveDirect(QStringLiteral("alsa_card.a"), QStringLiteral("hw:2"));
        QCOMPARE(target.status, Status::Resolved);
        QCOMPARE(target.hwPath, QStringLiteral("hw:3"));
        QCOMPARE(target.description, QStringLiteral("Card A"));
        QCOMPARE(AudioDeviceControl::findSelected(QStringLiteral("alsa_card.a"), QStringLiteral("hw:2"))->pwId, 42);
    }

    // An entry without a stable id (typed by hand, or saved before ids were
    // recorded) opens its device string as given.
    void manualEntryKeepsItsDeviceString()
    {
        FakeAudioGraph graph;
        const auto target = AudioDeviceControl::resolveDirect(QString(), QStringLiteral("hw:CARD=Example,DEV=0"));
        QCOMPARE(target.status, Status::Manual);
        QCOMPARE(target.hwPath, QStringLiteral("hw:CARD=Example,DEV=0"));
        QCOMPARE(AudioDeviceControl::resolveDirect(QString(), QString()).status, Status::Unavailable);
        QCOMPARE(graph.dumps, 0);
    }

    // One snapshot answers several questions, and a read that has not
    // answered never holds up its caller.
    void snapshotReadsAnswerLater()
    {
        FakeAudioGraph graph;
        graph.cards = {{42, 3, QStringLiteral("alsa_card.a"), QStringLiteral("Card A")}};
        graph.readDelayMs = 100;
        QObject context;
        std::optional<AudioDeviceControl::Snapshot> answer;
        AudioDeviceControl::readSnapshot(&context, [&answer](const AudioDeviceControl::Snapshot &graph) {
            answer = graph;
        });
        QVERIFY(!answer);
        QTRY_VERIFY(answer.has_value());
        QCOMPARE(graph.dumps, 1);
        const auto card = AudioDeviceControl::findSelected(*answer, QStringLiteral("alsa_card.a"), QString());
        QVERIFY(card.has_value());
        QCOMPARE(card->hwPath, QStringLiteral("hw:3"));
        QCOMPARE(AudioDeviceControl::sinkNodeId(*answer, QStringLiteral("hw:3")), 1042);
        QVERIFY(AudioDeviceControl::sinkNodeReady(*answer, QStringLiteral("hw:3")));
        QCOMPARE(graph.dumps, 1);

        // No answer reaches a caller that is gone.
        auto *gone = new QObject;
        bool answered = false;
        AudioDeviceControl::readSnapshot(gone, [&answered](const AudioDeviceControl::Snapshot &) { answered = true; });
        delete gone;
        QTest::qWait(250);
        QVERIFY(!answered);
    }

    // The installed reader runs pw-dump without waiting for it, and a pw-dump
    // that hangs answers with an empty graph once its time is up.
    void systemReadDoesNotWaitForPwDump()
    {
        QTest::failOnWarning(QRegularExpression(QStringLiteral("Destroyed while process")));
        QTemporaryDir bin;
        QVERIFY(bin.isValid());
        const auto writeTool = [&bin](const QByteArray &script) {
            QFile tool(bin.filePath(QStringLiteral("pw-dump")));
            QVERIFY(tool.open(QIODevice::WriteOnly | QIODevice::Truncate));
            tool.write(script);
            tool.close();
            tool.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        };
        const QByteArray path = qgetenv("PATH");
        qputenv("PATH", QFile::encodeName(bin.path()) + ':' + path);
        const auto restorePath = qScopeGuard([&path]() { qputenv("PATH", path); });

        FakeAudioGraph fixture;
        fixture.cards = {{42, 3, QStringLiteral("alsa_card.a"), QStringLiteral("Card A")}};
        const QByteArray graphJson = QJsonDocument(fixture.snapshot()).toJson(QJsonDocument::Compact);
        QVERIFY(fixture.replaced().dumpAsync);

        writeTool("#!/bin/sh\nsleep 0.3\ncat <<'JSON'\n" + graphJson + "\nJSON\n");
        QObject context;
        std::optional<AudioDeviceControl::Snapshot> answer;
        QElapsedTimer clock;
        clock.start();
        fixture.replaced().dumpAsync(&context, [&answer](std::optional<QByteArray> out) {
            answer = AudioDeviceControl::Snapshot{out ? QJsonDocument::fromJson(*out).array() : QJsonArray()};
        });
        QVERIFY(clock.elapsed() < 200);
        QVERIFY(!answer);
        QTRY_VERIFY_WITH_TIMEOUT(answer.has_value(), 5000);
        QVERIFY(AudioDeviceControl::findSelected(*answer, QStringLiteral("alsa_card.a"), QString()).has_value());

        writeTool("#!/bin/sh\nexec sleep 30\n");
        answer.reset();
        clock.restart();
        fixture.replaced().dumpAsync(&context, [&answer](std::optional<QByteArray> out) {
            answer = AudioDeviceControl::Snapshot{out ? QJsonDocument::fromJson(*out).array() : QJsonArray()};
        });
        QVERIFY(clock.elapsed() < 200);
        QTRY_VERIFY_WITH_TIMEOUT(answer.has_value(), 6000);
        QVERIFY(answer->objects.isEmpty());

        // A caller that goes away mid-read is not held up by the process and
        // gets no answer; the process is killed and released after it exits.
        writeTool("#!/bin/sh\nsleep 1\necho '[]'\n");
        bool answered = false;
        auto *leaving = new QObject;
        fixture.replaced().dumpAsync(leaving, [&answered](std::optional<QByteArray>) { answered = true; });
        clock.restart();
        delete leaving;
        QVERIFY(clock.elapsed() < 200);
        QTest::qWait(1500);
        QVERIFY(!answered);

        // A timed-out read whose answer ends its caller: the caller's
        // destruction does not wait for the killed process either.
        writeTool("#!/bin/sh\nexec sleep 30\n");
        auto *ending = new QObject;
        qint64 deleteMs = -1;
        fixture.replaced().dumpAsync(ending, [&ending, &deleteMs](std::optional<QByteArray>) {
            QElapsedTimer deleting;
            deleting.start();
            delete ending;
            ending = nullptr;
            deleteMs = deleting.elapsed();
        });
        QTRY_VERIFY_WITH_TIMEOUT(ending == nullptr, 6000);
        QVERIFY(deleteMs >= 0 && deleteMs < 200);
        QTest::qWait(500);
    }

    void takeoverAndReleaseSwitchOnlyTheGivenCard()
    {
        FakeAudioGraph graph;
        graph.cards = {{41, 2, QStringLiteral("alsa_card.b"), QStringLiteral("Card B")}};
        const auto card = AudioDeviceControl::findSelected(QStringLiteral("alsa_card.b"), QString());
        QVERIFY(card.has_value());
        QVERIFY(AudioDeviceControl::takeOver(*card));
        QVERIFY(AudioDeviceControl::release(*card));
        const QVector<QPair<int, int>> expected{{41, 0}, {41, 1}};
        QCOMPARE(graph.profileSwitches, expected);
    }

    // A found card is reused until a sound card appears or goes away; an
    // absent one is looked for again at every use.
    void resolverReusesOnlyFoundCards()
    {
        FakeAudioGraph graph;
        graph.cards = {{42, 3, QStringLiteral("alsa_card.a"), QStringLiteral("Card A")}};
        DirectDeviceResolver resolver;
        QCOMPARE(resolver.resolve(QStringLiteral("alsa_card.a"), QString()).hwPath, QStringLiteral("hw:3"));
        const int afterFirst = graph.dumps;
        resolver.resolve(QStringLiteral("alsa_card.a"), QString());
        QCOMPARE(graph.dumps, resolver.m_watching ? afterFirst : afterFirst + 1);

        graph.cards.clear();
        resolver.invalidate();
        QCOMPARE(resolver.resolve(QStringLiteral("alsa_card.a"), QStringLiteral("hw:3")).status, Status::Unavailable);
        const int afterAbsent = graph.dumps;
        resolver.resolve(QStringLiteral("alsa_card.a"), QStringLiteral("hw:3"));
        QCOMPARE(graph.dumps, afterAbsent + 1);

        // A device-node change forgets the reused answer at once and
        // announces the change once PipeWire had time to catch up.
        graph.cards = {{42, 5, QStringLiteral("alsa_card.a"), QStringLiteral("Card A")}};
        QSignalSpy changed(&resolver, &DirectDeviceResolver::cardsChanged);
        resolver.handleDeviceNodesChanged();
        QCOMPARE(resolver.resolve(QStringLiteral("alsa_card.a"), QString()).hwPath, QStringLiteral("hw:5"));
        QTRY_COMPARE_WITH_TIMEOUT(changed.count(), 1, 3000);
    }
};

QTEST_GUILESS_MAIN(AudioDeviceControlTest)

#include "test_audio_device_control.moc"
