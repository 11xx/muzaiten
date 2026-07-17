#include "reco/RadioProfile.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class RadioProfileTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void jsonRoundTrip();
    void missingStorageFallsBack();
    void corruptStorageFallsBack();
    void writeReadRoundTrip();

private:
    QTemporaryDir m_temp;
};

void RadioProfileTest::init()
{
    QVERIFY(m_temp.isValid());
    qputenv("MUZAITEN_CONFIG_DIR", m_temp.path().toUtf8());
}

void RadioProfileTest::jsonRoundTrip()
{
    RadioProfileStore store;
    RadioProfile profile = store.activeProfile();
    profile.weights.audioWeight = 4.25;
    profile.weights.skipPenalty = -8.0;
    profile.sessionDecay.decayStartTrack = 9;
    profile.sessionDecay.decayCurve = 1.0;
    QVERIFY(store.setProfiles({profile}, profile.name));
    QVERIFY(store.save());

    RadioProfileStore restored;
    QVERIFY(restored.load());
    QCOMPARE(restored.activeProfile(), profile);
}

void RadioProfileTest::missingStorageFallsBack()
{
    RadioProfileStore store;
    QVERIFY(store.load());
    QCOMPARE(store.profiles().size(), 1);
    QCOMPARE(store.activeProfileName(), QStringLiteral("Default"));
}

void RadioProfileTest::corruptStorageFallsBack()
{
    QFile file(RadioProfileStore::storagePath());
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("not json");
    file.close();

    RadioProfileStore store;
    QVERIFY(!store.load());
    QCOMPARE(store.profiles().size(), 1);
    QCOMPARE(store.activeProfileName(), QStringLiteral("Default"));
}

void RadioProfileTest::writeReadRoundTrip()
{
    RadioProfileStore store;
    RadioProfile first = store.activeProfile();
    first.name = QStringLiteral("Warm");
    RadioProfile second = first;
    second.name = QStringLiteral("Late night");
    second.weights.energyWeight = 2.4;
    QVERIFY(store.setProfiles({first, second}, second.name));
    QVERIFY(store.save());

    RadioProfileStore restored;
    QVERIFY(restored.load());
    QCOMPARE(restored.profiles().size(), 2);
    QCOMPARE(restored.activeProfileName(), second.name);
    QCOMPARE(restored.activeProfile(), second);
}

QTEST_MAIN(RadioProfileTest)
#include "test_radio_profile.moc"
