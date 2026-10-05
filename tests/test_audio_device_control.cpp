#include "AudioDeviceFixtures.h"

#include <QFileSystemWatcher>
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
