#include "db/SqlUtil.h"

#include <QTest>

class SqlUtilTest : public QObject {
    Q_OBJECT

private slots:
    void likeEscaped_data();
    void likeEscaped();
};

void SqlUtilTest::likeEscaped_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("expected");

    QTest::newRow("backslash") << QStringLiteral("a\\b") << QStringLiteral("a\\\\b");
    QTest::newRow("percent") << QStringLiteral("a%b") << QStringLiteral("a\\%b");
    QTest::newRow("underscore") << QStringLiteral("a_b") << QStringLiteral("a\\_b");
    QTest::newRow("plain") << QStringLiteral("plain") << QStringLiteral("plain");
}

void SqlUtilTest::likeEscaped()
{
    QFETCH(QString, input);
    QFETCH(QString, expected);

    QCOMPARE(SqlUtil::likeEscaped(input), expected);
}

QTEST_APPLESS_MAIN(SqlUtilTest)

#include "test_sql_util.moc"
