#include "core/Rating.h"
#include "ui/StarRating.h"

#include <QRect>
#include <QTest>

class RatingTest final : public QObject {
    Q_OBJECT

private slots:
    void normalizesToHalfStarBuckets();
    void validatesStoredValues();
    void mapsClickPositionToHalfStars();
};

void RatingTest::normalizesToHalfStarBuckets()
{
    QCOMPARE(Rating::normalized0To100(-10), Rating::unset);
    QCOMPARE(Rating::normalized0To100(0), 0);
    QCOMPARE(Rating::normalized0To100(4), 0);
    QCOMPARE(Rating::normalized0To100(5), 10);
    QCOMPARE(Rating::normalized0To100(96), 100);
    QCOMPARE(Rating::normalized0To100(120), 100);
}

void RatingTest::validatesStoredValues()
{
    QVERIFY(Rating::isValidStoredValue(Rating::unset));
    QVERIFY(Rating::isValidStoredValue(0));
    QVERIFY(Rating::isValidStoredValue(100));
    QVERIFY(!Rating::isValidStoredValue(95));
    QVERIFY(!Rating::isValidStoredValue(110));
}

void RatingTest::mapsClickPositionToHalfStars()
{
    QCOMPARE(Rating::normalized0To100(10), 10);
    QCOMPARE(Rating::normalized0To100(90), 90);

    const QRect rect(0, 0, 100, 20);
    QCOMPARE(StarRating::ratingFromPosition(rect, QPoint(5, 10)), 10);
    QCOMPARE(StarRating::ratingFromPosition(rect, QPoint(15, 10)), 20);
    QCOMPARE(StarRating::ratingFromPosition(rect, QPoint(85, 10)), 90);
    QCOMPARE(StarRating::ratingFromPosition(rect, QPoint(95, 10)), 100);
}

QTEST_MAIN(RatingTest)
#include "test_rating.moc"
