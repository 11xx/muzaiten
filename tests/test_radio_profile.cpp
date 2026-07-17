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
    void legacyWeightsSeedDefaultOnce();
    void corruptStorageFallsBack();
    void writeReadRoundTrip();
    void historyCapsAtFiftySnapshots();
    void undoRedoDiscardsRedoTail();
    void profileManagementKeepsIndependentHistory();

private:
    QTemporaryDir m_temp;
};

void RadioProfileTest::init()
{
    QVERIFY(m_temp.isValid());
    qputenv("MUZAITEN_CONFIG_DIR", m_temp.path().toUtf8());
    QFile::remove(RadioProfileStore::storagePath());
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

void RadioProfileTest::legacyWeightsSeedDefaultOnce()
{
    TrackScorer::Weights legacyWeights = TrackScorer::defaultWeights();
    legacyWeights.audioWeight = 4.25;

    RadioProfileStore store;
    QVERIFY(store.load(TrackScorer::weightsToJson(legacyWeights)));
    QCOMPARE(store.activeProfileName(), QStringLiteral("Default"));
    QCOMPARE(store.activeProfile().weights.audioWeight, legacyWeights.audioWeight);
    QVERIFY(QFile::exists(RadioProfileStore::storagePath()));

    TrackScorer::Weights laterLegacyWeights = TrackScorer::defaultWeights();
    laterLegacyWeights.audioWeight = 9.75;
    RadioProfileStore restored;
    QVERIFY(restored.load(TrackScorer::weightsToJson(laterLegacyWeights)));
    QCOMPARE(restored.activeProfile().weights.audioWeight, legacyWeights.audioWeight);
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

void RadioProfileTest::historyCapsAtFiftySnapshots()
{
    RadioProfileStore store;
    for (int value = 0; value < 60; ++value) {
        RadioProfile profile = store.activeProfile();
        profile.weights.audioWeight = static_cast<double>(value);
        QVERIFY(store.previewActiveProfile(profile));
        QVERIFY(store.commitActivePreview());
    }
    QCOMPARE(store.activeProfile().weights.audioWeight, 59.0);
    int undoCount = 0;
    while (store.undo()) {
        ++undoCount;
    }
    QCOMPARE(undoCount, RadioProfileStore::HistoryLimit - 1);
    QCOMPARE(store.activeProfile().weights.audioWeight, 10.0);
}

void RadioProfileTest::undoRedoDiscardsRedoTail()
{
    RadioProfileStore store;
    for (double value : {1.0, 2.0}) {
        RadioProfile profile = store.activeProfile();
        profile.weights.energyWeight = value;
        QVERIFY(store.previewActiveProfile(profile));
        QVERIFY(store.commitActivePreview());
    }
    QVERIFY(store.undo());
    QCOMPARE(store.activeProfile().weights.energyWeight, 1.0);
    RadioProfile profile = store.activeProfile();
    profile.weights.energyWeight = 3.0;
    QVERIFY(store.previewActiveProfile(profile));
    QVERIFY(store.commitActivePreview());
    QVERIFY(!store.canRedo());
    QCOMPARE(store.activeProfile().weights.energyWeight, 3.0);
}

void RadioProfileTest::profileManagementKeepsIndependentHistory()
{
    RadioProfileStore store;
    RadioProfile defaultProfile = store.activeProfile();
    defaultProfile.weights.tempoWeight = 2.0;
    QVERIFY(store.previewActiveProfile(defaultProfile));
    QVERIFY(store.commitActivePreview());
    QVERIFY(store.createProfile(QStringLiteral("Driving"), true));
    QCOMPARE(store.activeProfileName(), QStringLiteral("Driving"));
    RadioProfile driving = store.activeProfile();
    driving.weights.energyWeight = 4.0;
    QVERIFY(store.previewActiveProfile(driving));
    QVERIFY(store.commitActivePreview());
    QVERIFY(store.setActiveProfileName(QStringLiteral("Default")));
    QCOMPARE(store.activeProfile().weights.tempoWeight, 2.0);
    QVERIFY(store.undo());
    QCOMPARE(store.activeProfile().weights.tempoWeight, TrackScorer::defaultWeights().tempoWeight);
    QVERIFY(store.setActiveProfileName(QStringLiteral("Driving")));
    QCOMPARE(store.activeProfile().weights.energyWeight, 4.0);
    QVERIFY(store.undo());
    QCOMPARE(store.activeProfile().weights.energyWeight, TrackScorer::defaultWeights().energyWeight);
}

QTEST_MAIN(RadioProfileTest)
#include "test_radio_profile.moc"
