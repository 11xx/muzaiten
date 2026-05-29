#include "core/MetadataBlob.h"

#include <QTest>

class MetadataBlobTest final : public QObject {
    Q_OBJECT

private slots:
    void roundTripsTagsAndTech();
    void preservesMultiValuedTags();
    void emptyMetadataIsEmpty();
    void decodeRejectsGarbage();
};

void MetadataBlobTest::roundTripsTagsAndTech()
{
    MetadataBlob::FullMetadata original;
    original.tags.insert(QStringLiteral("TITLE"), {QStringLiteral("Björk — Jóga")});
    original.tags.insert(QStringLiteral("ALBUMARTIST"), {QStringLiteral("Björk")});
    original.tags.insert(QStringLiteral("MUSICBRAINZ_TRACKID"), {QStringLiteral("abc-123")});
    original.bitrateKbps = 1024;
    original.sampleRateHz = 44100;
    original.channels = 2;
    original.codec = QStringLiteral("flac");

    const MetadataBlob::Encoded encoded = MetadataBlob::encode(original);
    QVERIFY(!encoded.data.isEmpty());
    QVERIFY(encoded.rawSize > 0);

    const MetadataBlob::FullMetadata decoded = MetadataBlob::decode(encoded.data, encoded.rawSize);
    QCOMPARE(decoded.tags.value(QStringLiteral("TITLE")), QStringList{QStringLiteral("Björk — Jóga")});
    QCOMPARE(decoded.tags.value(QStringLiteral("ALBUMARTIST")), QStringList{QStringLiteral("Björk")});
    QCOMPARE(decoded.tags.value(QStringLiteral("MUSICBRAINZ_TRACKID")), QStringList{QStringLiteral("abc-123")});
    QCOMPARE(decoded.bitrateKbps, 1024);
    QCOMPARE(decoded.sampleRateHz, 44100);
    QCOMPARE(decoded.channels, 2);
    QCOMPARE(decoded.codec, QStringLiteral("flac"));
}

void MetadataBlobTest::preservesMultiValuedTags()
{
    MetadataBlob::FullMetadata original;
    original.tags.insert(QStringLiteral("ARTIST"), {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")});
    original.tags.insert(QStringLiteral("GENRE"), {QStringLiteral("Electronic"), QStringLiteral("Ambient")});

    const MetadataBlob::Encoded encoded = MetadataBlob::encode(original);
    const MetadataBlob::FullMetadata decoded = MetadataBlob::decode(encoded.data, encoded.rawSize);

    QCOMPARE(decoded.tags.value(QStringLiteral("ARTIST")).size(), 3);
    QCOMPARE(decoded.tags.value(QStringLiteral("ARTIST")).at(2), QStringLiteral("C"));
    QCOMPARE(decoded.tags.value(QStringLiteral("GENRE")).size(), 2);
}

void MetadataBlobTest::emptyMetadataIsEmpty()
{
    MetadataBlob::FullMetadata empty;
    QVERIFY(MetadataBlob::isEmpty(empty));

    MetadataBlob::FullMetadata withTech;
    withTech.sampleRateHz = 48000;
    QVERIFY(!MetadataBlob::isEmpty(withTech));
}

void MetadataBlobTest::decodeRejectsGarbage()
{
    const MetadataBlob::FullMetadata decoded = MetadataBlob::decode(QByteArrayLiteral("not a zstd frame"), 100);
    QVERIFY(MetadataBlob::isEmpty(decoded));
}

QTEST_MAIN(MetadataBlobTest)
#include "test_metadata_blob.moc"
