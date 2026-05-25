#include "scanner/TagRating.h"

#include <QTest>

#include <taglib/tstring.h>
#include <taglib/tstringlist.h>

class TagRatingTest final : public QObject {
    Q_OBJECT

private slots:
    void ratingReadsAsInteger();
    void ratingNormalizesToNearestTen();
    void fmpsRatingNormalizes();
    void ratingWinsOverFmpsRating();
    void invalidRatingFallsBackToFmpsRating();
    void setMusicBeeRatingWritesRatingProperty();
};

namespace {

void putProperty(TagLib::PropertyMap &properties, const char *key, const char *value)
{
    properties.replace(TagLib::String(key, TagLib::String::UTF8),
                       TagLib::StringList(TagLib::String(value, TagLib::String::UTF8)));
}

} // namespace

void TagRatingTest::ratingReadsAsInteger()
{
    TagLib::PropertyMap properties;
    putProperty(properties, "RATING", "80");
    const TagRatingReadResult rating = readRating(properties);
    QCOMPARE(rating.rating0To100, 80);
    QCOMPARE(rating.source, Rating::Source::MusicBeeCompatible);
}

void TagRatingTest::ratingNormalizesToNearestTen()
{
    TagLib::PropertyMap properties;
    putProperty(properties, "RATING", "87");
    QCOMPARE(readRating(properties).rating0To100, 90);
}

void TagRatingTest::fmpsRatingNormalizes()
{
    TagLib::PropertyMap properties;
    putProperty(properties, "FMPS_RATING", "0.75");
    const TagRatingReadResult rating = readRating(properties);
    QCOMPARE(rating.rating0To100, 80);
    QCOMPARE(rating.source, Rating::Source::VorbisRating);
}

void TagRatingTest::ratingWinsOverFmpsRating()
{
    TagLib::PropertyMap properties;
    putProperty(properties, "RATING", "60");
    putProperty(properties, "FMPS_RATING", "0.90");
    const TagRatingReadResult rating = readRating(properties);
    QCOMPARE(rating.rating0To100, 60);
    QCOMPARE(rating.source, Rating::Source::MusicBeeCompatible);
}

void TagRatingTest::invalidRatingFallsBackToFmpsRating()
{
    TagLib::PropertyMap properties;
    putProperty(properties, "RATING", "not-a-rating");
    putProperty(properties, "FMPS_RATING", "0.90");
    const TagRatingReadResult rating = readRating(properties);
    QCOMPARE(rating.rating0To100, 90);
    QCOMPARE(rating.source, Rating::Source::VorbisRating);
}

void TagRatingTest::setMusicBeeRatingWritesRatingProperty()
{
    TagLib::PropertyMap properties;
    setMusicBeeRating(properties, 70);
    const TagLib::StringList values = properties[TagLib::String("RATING", TagLib::String::UTF8)];
    QVERIFY(!values.isEmpty());
    QCOMPARE(QString::fromStdString(values.front().to8Bit(true)), QStringLiteral("70"));
}

QTEST_MAIN(TagRatingTest)
#include "test_tag_rating.moc"
