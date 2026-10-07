#include "reco/RadioProfile.h"
#include "reco/RadioProfileExchange.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

#include <limits>

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
    void exchangeRoundTrip();
    void exchangeRejects_data();
    void exchangeRejects();
    void learnedConsentAndOriginSurviveEdits();
    void legacyOriginIsConservative();
    void importPreviewAndReplace();
    void failedImportKeepsMemoryAndStorage();
    void atomicExportAndConsent();

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
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Ignoring malformed radio profiles: .*")));
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

void RadioProfileTest::exchangeRoundTrip()
{
    RadioProfile profile = RadioProfileStore::defaultProfile();
    profile.name = QString::fromUtf8("Evening 🌙");
    profile.weights.audioWeight = 4.25;
    profile.sessionDecay = {17, 1.0, 0.35, 0.6};
    QByteArray first;
    QString error;
    QVERIFY2(RadioProfileExchange::encode(profile, false, &first, &error), qPrintable(error));
    RadioProfile parsed;
    QVERIFY2(RadioProfileExchange::parse(first, &parsed, &error), qPrintable(error));
    QCOMPARE(parsed.origin, RadioProfile::Origin::Manual);
    QCOMPARE(parsed.sessionDecay.decayStartTrack, 17);
    QCOMPARE(parsed.sessionDecay.decayCurve, 1.0);
    QCOMPARE(parsed.sessionDecay.noveltyDecayFloor, 0.35);
    QCOMPARE(parsed.sessionDecay.ratingDecayFloor, 0.6);
    QVERIFY(parsed.createdAtUtc.isEmpty());
    QByteArray second;
    QVERIFY(RadioProfileExchange::encode(parsed, false, &second, &error));
    QCOMPARE(first, second);
    QCOMPARE(QJsonDocument::fromJson(first).object().size(), 6);
}

void RadioProfileTest::exchangeRejects_data()
{
    QTest::addColumn<QByteArray>("bytes");
    QByteArray valid;
    QString error;
    QVERIFY(RadioProfileExchange::encode(RadioProfileStore::defaultProfile(), false, &valid, &error));
    const QJsonObject original = QJsonDocument::fromJson(valid).object();
    const auto root = [&original](const QString &key, const QJsonValue &value) {
        QJsonObject changed = original;
        changed.insert(key, value);
        return QJsonDocument(changed).toJson();
    };
    QTest::newRow("format") << root(QStringLiteral("format"), QStringLiteral("other"));
    QTest::newRow("version") << root(QStringLiteral("version"), 2);
    QTest::newRow("fractional-version") << root(QStringLiteral("version"), 1.5);
    QTest::newRow("string-version") << root(QStringLiteral("version"), QStringLiteral("1"));
    QTest::newRow("unknown-root-key") << root(QStringLiteral("history"), QJsonArray{});
    QTest::newRow("empty-name") << root(QStringLiteral("name"), QStringLiteral(""));
    QTest::newRow("trimmed-name") << root(QStringLiteral("name"), QStringLiteral(" name "));
    QTest::newRow("long-name") << root(QStringLiteral("name"), QString(201, QLatin1Char('a')));
    QTest::newRow("control-name") << root(QStringLiteral("name"), QStringLiteral("name\nline"));
    QTest::newRow("invisible-name")
        << root(QStringLiteral("name"), QStringLiteral("Default") + QChar(0x200B));
    QTest::newRow("bidi-name")
        << root(QStringLiteral("name"), QStringLiteral("Default") + QChar(0x202E));
    QTest::newRow("unknown-origin") << root(QStringLiteral("origin"), QStringLiteral("unknown"));
    QTest::newRow("null-weights") << root(QStringLiteral("weights"), QJsonValue(QJsonValue::Null));
    for (const QString &section : {QStringLiteral("weights"), QStringLiteral("sessionDecay")}) {
        QJsonObject nested = original.value(section).toObject();
        nested.insert(QStringLiteral("unknown"), 1);
        QTest::newRow(qPrintable(section + QStringLiteral("-unknown"))) << root(section, nested);
        nested = original.value(section).toObject();
        nested.remove(nested.keys().first());
        QTest::newRow(qPrintable(section + QStringLiteral("-missing"))) << root(section, nested);
    }
    QJsonObject weights = original.value(QStringLiteral("weights")).toObject();
    weights.insert(QStringLiteral("genreWeight"), -1.0);
    QTest::newRow("negative-weight") << root(QStringLiteral("weights"), weights);
    weights.insert(QStringLiteral("genreWeight"), QStringLiteral("3"));
    QTest::newRow("string-weight") << root(QStringLiteral("weights"), weights);
    const auto decay = [&original, &root](const QString &key, const QJsonValue &value) {
        QJsonObject nested = original.value(QStringLiteral("sessionDecay")).toObject();
        nested.insert(key, value);
        return root(QStringLiteral("sessionDecay"), nested);
    };
    QTest::newRow("start-zero") << decay(QStringLiteral("decayStartTrack"), 0);
    QTest::newRow("start-large") << decay(QStringLiteral("decayStartTrack"), 1001);
    QTest::newRow("start-fraction") << decay(QStringLiteral("decayStartTrack"), 1.5);
    QTest::newRow("curve-range") << decay(QStringLiteral("decayCurve"), 2.0);
    QTest::newRow("floor-range") << decay(QStringLiteral("noveltyDecayFloor"), -0.1);
    QTest::newRow("floor-null") << decay(QStringLiteral("ratingDecayFloor"), QJsonValue(QJsonValue::Null));
    QTest::newRow("nan") << QByteArray(valid).replace("\"decayCurve\": 0", "\"decayCurve\": NaN");
    QTest::newRow("infinity") << QByteArray(valid).replace("\"decayCurve\": 0", "\"decayCurve\": 1e999");
    QTest::newRow("truncated") << valid.left(valid.size() / 2);
    QTest::newRow("oversize") << (valid + QByteArray(RadioProfileExchange::MaximumBytes, ' '));
    QTest::newRow("duplicate") << QByteArray(valid).replace("\"version\": 1", "\"version\": 2, \"version\": 1");
    QTest::newRow("escaped-duplicate") << QByteArray(valid).replace("\"version\": 1", "\"version\": 2, \"vers\\u0069on\": 1");
    QTest::newRow("invalid-utf8") << QByteArray(valid).replace("Default", QByteArray(1, '\xff'));
}

void RadioProfileTest::exchangeRejects()
{
    QFETCH(QByteArray, bytes);
    RadioProfileStore store;
    QVERIFY(store.save());
    QFile storage(RadioProfileStore::storagePath());
    QVERIFY(storage.open(QIODevice::ReadOnly));
    const QByteArray before = storage.readAll();
    storage.close();
    RadioProfile parsed = store.activeProfile();
    const RadioProfile original = parsed;
    QString error;
    QVERIFY(!RadioProfileExchange::parse(bytes, &parsed, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(parsed, original);
    QVERIFY(storage.open(QIODevice::ReadOnly));
    QCOMPARE(storage.readAll(), before);
}

void RadioProfileTest::learnedConsentAndOriginSurviveEdits()
{
    RadioProfileStore store;
    RadioProfile profile = store.activeProfile();
    profile.origin = RadioProfile::Origin::Learned;
    QVERIFY(store.saveProfile(QStringLiteral("Learned"), profile));
    QVERIFY(store.setActiveProfileName(QStringLiteral("Learned")));
    QVERIFY(store.renameActiveProfile(QStringLiteral("Renamed")));
    QVERIFY(store.createProfile(QStringLiteral("Copy"), true));
    profile = store.activeProfile();
    profile.weights.energyWeight = 4.0;
    QVERIFY(store.previewActiveProfile(profile));
    QVERIFY(store.commitActivePreview());
    QVERIFY(store.save());
    RadioProfileStore restored;
    QVERIFY(restored.load());
    QCOMPARE(restored.activeProfile().origin, RadioProfile::Origin::Learned);
    QByteArray bytes;
    QString error;
    QVERIFY(!RadioProfileExchange::encode(restored.activeProfile(), false, &bytes, &error));
    QVERIFY(RadioProfileExchange::encode(restored.activeProfile(), true, &bytes, &error));
    QVERIFY(restored.resetActiveProfile());
    QCOMPARE(restored.activeProfile().origin, RadioProfile::Origin::Manual);
    QVERIFY(restored.undo());
    QCOMPARE(restored.activeProfile().origin, RadioProfile::Origin::Learned);
    profile.origin = RadioProfile::Origin::Manual;
    profile.weights.genreWeight = std::numeric_limits<double>::quiet_NaN();
    QVERIFY(!RadioProfileExchange::encode(profile, false, &bytes, &error));
}

void RadioProfileTest::legacyOriginIsConservative()
{
    RadioProfileStore store;
    QVERIFY(store.createProfile(QStringLiteral("learned-20260101"), false));
    QVERIFY(store.save());
    QFile file(RadioProfileStore::storagePath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QJsonArray profiles;
    for (const QJsonValue &value : root.value(QStringLiteral("profiles")).toArray()) {
        QJsonObject entry = value.toObject();
        entry.remove(QStringLiteral("origin"));
        profiles.append(entry);
    }
    root.insert(QStringLiteral("profiles"), profiles);
    root.remove(QStringLiteral("histories"));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(root).toJson());
    file.close();
    QVERIFY(store.load());
    QCOMPARE(store.activeProfile().origin, RadioProfile::Origin::Learned);
    QVERIFY(store.setActiveProfileName(QStringLiteral("Default")));
    QCOMPARE(store.activeProfile().origin, RadioProfile::Origin::Unknown);
    QByteArray bytes;
    QString error;
    QVERIFY(!RadioProfileExchange::encode(store.activeProfile(), false, &bytes, &error));
    QVERIFY(RadioProfileExchange::encode(store.activeProfile(), true, &bytes, &error));
    QCOMPARE(QJsonDocument::fromJson(bytes).object().value(QStringLiteral("origin")).toString(), QStringLiteral("learned"));
    QVERIFY(store.save());
    RadioProfileStore restored;
    QVERIFY(restored.load());
    QCOMPARE(restored.activeProfile().origin, RadioProfile::Origin::Unknown);
}

void RadioProfileTest::importPreviewAndReplace()
{
    RadioProfileStore store;
    QVERIFY(store.save());
    RadioProfile imported = store.activeProfile();
    imported.weights.audioWeight = 8.0;
    imported.sessionDecay = {12, 1.0, 0.4, 0.7};
    QString error;
    QFile file(RadioProfileStore::storagePath());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray before = file.readAll();
    file.close();
    const RadioProfile original = store.activeProfile();
    QVERIFY(store.importProfile(imported, false, true, &error));
    QCOMPARE(store.activeProfile(), original);
    QVERIFY(!store.importProfile(imported, false, false, &error));
    QCOMPARE(store.activeProfile(), original);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), before);
    file.close();
    QVERIFY(store.importProfile(imported, true, false, &error));
    QCOMPARE(store.activeProfileName(), original.name);
    QCOMPARE(store.activeProfile().weights.audioWeight, 8.0);
    RadioProfileStore restored;
    QVERIFY(restored.load());
    QByteArray first;
    QByteArray second;
    QVERIFY(RadioProfileExchange::encode(imported, false, &first, &error));
    QVERIFY(RadioProfileExchange::encode(restored.activeProfile(), false, &second, &error));
    QCOMPARE(first, second);
    imported.name = QStringLiteral("Other");
    QVERIFY(store.importProfile(imported, false, false, &error));
    QCOMPARE(store.activeProfileName(), original.name);
}

void RadioProfileTest::failedImportKeepsMemoryAndStorage()
{
    RadioProfileStore store;
    const RadioProfile original = store.activeProfile();
    QVERIFY(QDir().mkdir(RadioProfileStore::storagePath()));
    RadioProfile imported = original;
    imported.name = QStringLiteral("Other");
    QString error;
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("Could not save radio profiles: .*")));
    QVERIFY(!store.importProfile(imported, false, false, &error));
    QCOMPARE(store.activeProfile(), original);
    QCOMPARE(store.profiles().size(), 1);
    QVERIFY(QFileInfo(RadioProfileStore::storagePath()).isDir());
    QVERIFY(QDir().rmdir(RadioProfileStore::storagePath()));
}

void RadioProfileTest::atomicExportAndConsent()
{
    const QString path = m_temp.filePath(QStringLiteral("exchange.json"));
    QFile::remove(path);
    RadioProfile profile = RadioProfileStore::defaultProfile();
    QString error;
    QVERIFY(RadioProfileExchange::write(path, profile, false, false, &error));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray before = file.readAll();
    file.close();
    profile.origin = RadioProfile::Origin::Learned;
    QVERIFY(!RadioProfileExchange::write(path, profile, false, true, &error));
    QVERIFY(!RadioProfileExchange::write(path, profile, true, false, &error));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), before);
    file.close();
    QVERIFY(RadioProfileExchange::write(path, profile, true, true, &error));
    RadioProfile read;
    QVERIFY(RadioProfileExchange::read(path, &read, &error));
    QCOMPARE(read.origin, RadioProfile::Origin::Learned);
}

QTEST_MAIN(RadioProfileTest)
#include "test_radio_profile.moc"
