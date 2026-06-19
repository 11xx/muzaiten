#include <QtTest>

#include "core/HumanQuantity.h"
#include "core/Track.h"
#include "ui/trackinfo/TrackInfoSettings.h"

#include <QJsonArray>
#include <QJsonObject>

using namespace trackinfo;

namespace {

QJsonObject metaItem(const QString &key, const QString &mode = QStringLiteral("always"), int condValue = 0,
                     bool visible = true)
{
    QJsonObject item;
    item.insert(QStringLiteral("key"), key);
    item.insert(QStringLiteral("visible"), visible);
    item.insert(QStringLiteral("mode"), mode);
    item.insert(QStringLiteral("condValue"), condValue);
    return item;
}

} // namespace

class TestTrackInfoSettings : public QObject {
    Q_OBJECT

private slots:
    void durationFormatParseRoundTrip()
    {
        QCOMPARE(humanquantity::formatDuration(225000), QStringLiteral("3:45"));
        QCOMPARE(humanquantity::formatDuration(0), QString());
        // Minutes are intentionally not capped at 59 (long-standing display).
        QCOMPARE(humanquantity::formatDuration(3750000), QStringLiteral("62:30"));

        // formatClock rolls into hours instead of leaving minutes uncapped.
        QCOMPARE(humanquantity::formatClock(225000), QStringLiteral("3:45"));
        QCOMPARE(humanquantity::formatClock(0), QString());
        QCOMPARE(humanquantity::formatClock(-5), QString());
        QCOMPARE(humanquantity::formatClock(3750000), QStringLiteral("1:02:30"));
        QCOMPARE(humanquantity::formatClock(3600000), QStringLiteral("1:00:00"));

        QCOMPARE(humanquantity::parseDuration(QStringLiteral("3:45")), qint64(225000));
        QCOMPARE(humanquantity::parseDuration(QStringLiteral("1:02:30")), qint64(3750000));
        QCOMPARE(humanquantity::parseDuration(QStringLiteral("45")), qint64(45000));
        QCOMPARE(humanquantity::parseDuration(QString()), qint64(0));
        // Blank components (from a colon-prefilled mask) count as zero.
        QCOMPARE(humanquantity::parseDuration(QStringLiteral(":30")), qint64(30000));
    }

    void sizeFormatting()
    {
        QCOMPARE(humanquantity::formatSize(0), QString());
        QCOMPARE(humanquantity::formatSize(512), QStringLiteral("512 B"));
        QCOMPARE(humanquantity::formatSize(1024), QStringLiteral("1.0 KB"));
        QCOMPARE(humanquantity::formatSize(5 * 1024 * 1024), QStringLiteral("5.0 MB"));
    }

    void notableThresholds()
    {
        Track hires;
        hires.sampleRateHz = 96000;
        hires.bitDepth = 24;
        hires.channels = 6;
        Track cd;
        cd.sampleRateHz = 44100;
        cd.bitDepth = 16;
        cd.channels = 2;

        QVERIFY(metadataItemPassesMode(hires, QStringLiteral("sampleRate"), QStringLiteral("notable"), 48));
        QVERIFY(!metadataItemPassesMode(cd, QStringLiteral("sampleRate"), QStringLiteral("notable"), 48));
        QVERIFY(metadataItemPassesMode(hires, QStringLiteral("bitDepth"), QStringLiteral("notable"), 16));
        QVERIFY(!metadataItemPassesMode(cd, QStringLiteral("bitDepth"), QStringLiteral("notable"), 16));
        QVERIFY(metadataItemPassesMode(hires, QStringLiteral("channels"), QStringLiteral("notable"), 2));
        QVERIFY(!metadataItemPassesMode(cd, QStringLiteral("channels"), QStringLiteral("notable"), 2));

        // A custom threshold flips the verdict.
        QVERIFY(!metadataItemPassesMode(hires, QStringLiteral("sampleRate"), QStringLiteral("notable"), 96));
        QVERIFY(metadataItemPassesMode(cd, QStringLiteral("sampleRate"), QStringLiteral("notable"), 40));
        // "always" never filters.
        QVERIFY(metadataItemPassesMode(cd, QStringLiteral("sampleRate"), QStringLiteral("always"), 48));
    }

    void durationAndSizeConditions()
    {
        Track shortSmall;
        shortSmall.durationMs = 120000;     // 2:00
        shortSmall.fileSize = 3 * 1000000;  // 3 MB
        Track longBig;
        longBig.durationMs = 600000;        // 10:00
        longBig.fileSize = 80 * 1000000;    // 80 MB

        QVERIFY(metadataItemPassesMode(longBig, QStringLiteral("duration"), QStringLiteral("durationOver"), 300000));
        QVERIFY(!metadataItemPassesMode(shortSmall, QStringLiteral("duration"), QStringLiteral("durationOver"), 300000));
        QVERIFY(metadataItemPassesMode(longBig, QStringLiteral("size"), QStringLiteral("sizeOver"), 50));
        QVERIFY(!metadataItemPassesMode(shortSmall, QStringLiteral("size"), QStringLiteral("sizeOver"), 50));
    }

    void formatClassConditions()
    {
        Track flac;
        flac.path = QStringLiteral("/m/a.flac");
        Track mp3;
        mp3.path = QStringLiteral("/m/a.mp3");
        Track alacInM4a;
        alacInM4a.path = QStringLiteral("/m/a.m4a");
        alacInM4a.bitDepth = 24;  // lossless payload in an ambiguous container

        QVERIFY(isLosslessFormat(flac));
        QVERIFY(!isLosslessFormat(mp3));
        QVERIFY(isLosslessFormat(alacInM4a));

        QVERIFY(metadataItemPassesMode(flac, QStringLiteral("format"), QStringLiteral("formatLossless"), 0));
        QVERIFY(!metadataItemPassesMode(flac, QStringLiteral("format"), QStringLiteral("formatLossy"), 0));
        QVERIFY(metadataItemPassesMode(mp3, QStringLiteral("format"), QStringLiteral("formatLossy"), 0));
    }

    void contextualModeOptions()
    {
        // Codec offers only "Always"; format offers lossy/lossless variants.
        QCOMPARE(metadataModeOptions(QStringLiteral("codec")).size(), 1);
        const auto formatModes = metadataModeOptions(QStringLiteral("format"));
        QStringList tokens;
        for (const auto &option : formatModes) {
            tokens.push_back(option.token);
        }
        QVERIFY(tokens.contains(QStringLiteral("formatLossy")));
        QVERIFY(tokens.contains(QStringLiteral("formatLossless")));

        QCOMPARE(metadataCondition(QStringLiteral("duration")).editor, ConditionEditorKind::Duration);
        QCOMPARE(metadataCondition(QStringLiteral("sampleRate")).editor, ConditionEditorKind::IntSpin);
        QCOMPARE(metadataCondition(QStringLiteral("codec")).editor, ConditionEditorKind::None);
    }

    void legacyNotableMinIsRead()
    {
        QJsonObject item;
        item.insert(QStringLiteral("key"), QStringLiteral("sampleRate"));
        item.insert(QStringLiteral("mode"), QStringLiteral("notable"));
        item.insert(QStringLiteral("notableMin"), 96);  // old key
        const QJsonArray normalized = normalizedMetadataItems(QJsonArray{item});
        QCOMPARE(normalized.at(0).toObject().value(QStringLiteral("condValue")).toInt(), 96);
    }

    void lossyMode()
    {
        Track lossy;
        lossy.bitrateKbps = 256;
        lossy.bitDepth = 0;
        Track lossless;
        lossless.bitrateKbps = 900;
        lossless.bitDepth = 16;

        QVERIFY(metadataItemPassesMode(lossy, QStringLiteral("bitrate"), QStringLiteral("lossy"), 0));
        QVERIFY(!metadataItemPassesMode(lossless, QStringLiteral("bitrate"), QStringLiteral("lossy"), 0));
    }

    void metadataTextRespectsModeAndVisibility()
    {
        Track cd;
        cd.path = QStringLiteral("/music/song.flac");
        cd.durationMs = 225000;
        cd.sampleRateHz = 44100;
        cd.bitDepth = 16;
        cd.channels = 2;

        QJsonArray items{
            metaItem(QStringLiteral("format")),
            metaItem(QStringLiteral("duration")),
            metaItem(QStringLiteral("sampleRate"), QStringLiteral("notable"), 48),
            metaItem(QStringLiteral("bitDepth"), QStringLiteral("notable"), 16),
            metaItem(QStringLiteral("codec"), QStringLiteral("always"), 0, /*visible=*/false),
        };
        // sampleRate/bitDepth are filtered out (CD quality), codec is hidden.
        const QString dot = QString::fromUtf8("\xc2\xb7");
        QCOMPARE(metadataText(cd, items, dot, 1),
                 QStringLiteral("FLAC ") + dot + QStringLiteral(" 3:45"));

        Track hires = cd;
        hires.sampleRateHz = 96000;
        hires.bitDepth = 24;
        QCOMPARE(metadataText(hires, items, QStringLiteral("|"), 1),
                 QStringLiteral("FLAC | 3:45 | 96 kHz | 24-bit"));
    }

    void normalizationDropsUnknownDedupsAndAppends()
    {
        QJsonArray source{
            metaItem(QStringLiteral("codec"), QStringLiteral("always"), 0, false),
            metaItem(QStringLiteral("bogus")),
            metaItem(QStringLiteral("codec")), // duplicate ignored
        };
        const QJsonArray normalized = normalizedMetadataItems(source);
        // Every known item appears exactly once; unknown dropped.
        QCOMPARE(normalized.size(), availableTrackInfoMetadataItems().size());
        QCOMPARE(normalized.at(0).toObject().value(QStringLiteral("key")).toString(), QStringLiteral("codec"));
        QVERIFY(!normalized.at(0).toObject().value(QStringLiteral("visible")).toBool());

        QStringList keys;
        for (const QJsonValue &value : normalized) {
            keys.push_back(value.toObject().value(QStringLiteral("key")).toString());
        }
        QVERIFY(!keys.contains(QStringLiteral("bogus")));
        QCOMPARE(keys.count(QStringLiteral("codec")), 1);
        QVERIFY(keys.contains(QStringLiteral("sampleRate")));
    }

    void normalizationFillsDefaults()
    {
        const QJsonArray normalized = normalizedMetadataItems(QJsonArray{});
        QCOMPARE(normalized.size(), availableTrackInfoMetadataItems().size());
        for (const QJsonValue &value : normalized) {
            const QJsonObject item = value.toObject();
            const QString key = item.value(QStringLiteral("key")).toString();
            QCOMPARE(item.value(QStringLiteral("mode")).toString(), metadataDefaultMode(key));
            QCOMPARE(item.value(QStringLiteral("condValue")).toInt(), metadataDefaultCondValue(key));
        }
    }

    void separatorPresetLabelling()
    {
        QCOMPARE(separatorPresetLabel(QString()), QStringLiteral("None"));
        QCOMPARE(separatorPresetLabel(QString::fromUtf8("\xc2\xb7")), QStringLiteral("Middle dot"));
        QCOMPARE(separatorPresetLabel(QStringLiteral("/")), QStringLiteral("Custom"));
    }
};

QTEST_GUILESS_MAIN(TestTrackInfoSettings)
#include "test_track_info_settings.moc"
