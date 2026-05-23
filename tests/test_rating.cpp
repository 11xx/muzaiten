#include "core/Rating.h"

#include <QTest>

class RatingTest final : public QObject {
    Q_OBJECT

private slots:
    void normalizesToHalfStarBuckets();
    void validatesStoredValues();
    void displaysUnsetAsBlank();
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

void RatingTest::displaysUnsetAsBlank()
{
    QCOMPARE(Rating::displayText(Rating::unset), QString());
    QCOMPARE(Rating::displayText(0), QString());
    QCOMPARE(Rating::displayText(10), QStringLiteral("1/2"));
    QCOMPARE(Rating::displayText(20), QString(QChar(0x2605)));
}

QTEST_MAIN(RatingTest)
#include "test_rating.moc"

