#include "fs/MediaProbe.h"
#include "indexer/DecodeGate.h"

#include <QtTest>

class TestDecodeGate : public QObject
{
    Q_OBJECT

private slots:
    void clampsInitialTarget();
    void shrinksOnSlowWindow();
    void growsOnFastWindow();
    void holdsInsideDeadBand();
    void ignoresNonDecodes();
    void mediaProbeNames();
};

namespace {

void feedWindow(DecodeGate &gate, double ms, bool &changed)
{
    changed = false;
    for (int i = 0; i < 8; ++i) {
        changed = gate.recordDecodeMs(ms) || changed;
    }
}

} // namespace

void TestDecodeGate::clampsInitialTarget()
{
    QCOMPARE(DecodeGate(2, 1, 16).target(), 2);
    QCOMPARE(DecodeGate(99, 1, 16).target(), 16);
    QCOMPARE(DecodeGate(0, 1, 16).target(), 1);
    QCOMPARE(DecodeGate(4, 1, 16).initialTarget(), 4);
}

void TestDecodeGate::shrinksOnSlowWindow()
{
    DecodeGate gate(4, 1, 16);
    bool changed = false;
    feedWindow(gate, 4000.0, changed);
    QVERIFY(changed);
    QCOMPARE(gate.target(), 3);
    feedWindow(gate, 4000.0, changed);
    feedWindow(gate, 4000.0, changed);
    feedWindow(gate, 4000.0, changed);
    QCOMPARE(gate.target(), 1);
    feedWindow(gate, 4000.0, changed);
    QVERIFY(!changed);
    QCOMPARE(gate.target(), 1); // floor holds
}

void TestDecodeGate::growsOnFastWindow()
{
    DecodeGate gate(2, 1, 4);
    bool changed = false;
    feedWindow(gate, 200.0, changed);
    QVERIFY(changed);
    QCOMPARE(gate.target(), 3);
    feedWindow(gate, 200.0, changed);
    QCOMPARE(gate.target(), 4);
    feedWindow(gate, 200.0, changed);
    QVERIFY(!changed);
    QCOMPARE(gate.target(), 4); // cap holds
}

void TestDecodeGate::holdsInsideDeadBand()
{
    DecodeGate gate(3, 1, 16);
    bool changed = false;
    feedWindow(gate, 1200.0, changed);
    QVERIFY(!changed);
    QCOMPARE(gate.target(), 3);
}

void TestDecodeGate::ignoresNonDecodes()
{
    DecodeGate gate(3, 1, 16);
    for (int i = 0; i < 64; ++i) {
        QVERIFY(!gate.recordDecodeMs(0.0));
    }
    QCOMPARE(gate.target(), 3);
}

void TestDecodeGate::mediaProbeNames()
{
    QCOMPARE(MediaProbe::name(MediaProbe::Class::Fast), QStringLiteral("fast"));
    QCOMPARE(MediaProbe::name(MediaProbe::Class::Rotational), QStringLiteral("rotational"));
    QCOMPARE(MediaProbe::name(MediaProbe::Class::Network), QStringLiteral("network"));
    QVERIFY(!MediaProbe::seekSensitive(MediaProbe::Class::Fast));
    QVERIFY(MediaProbe::seekSensitive(MediaProbe::Class::Rotational));
    QVERIFY(MediaProbe::seekSensitive(MediaProbe::Class::Network));
    // classify() must never crash on ordinary paths; the concrete class
    // depends on the host running the tests.
    const MediaProbe::Class root = MediaProbe::classify(QStringLiteral("/"));
    QVERIFY(root == MediaProbe::Class::Fast || root == MediaProbe::Class::Rotational
            || root == MediaProbe::Class::Network);
}

QTEST_MAIN(TestDecodeGate)
#include "test_decode_gate.moc"
