#include "db/SqlUtil.h"

#include <QTest>
#include <QSqlQuery>

class SqlUtilTest : public QObject {
    Q_OBJECT

private slots:
    void nestedSavepointCommitStillRollsBackWithOuter()
    {
        const QString name = QStringLiteral("savepoint-test");
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
            db.setDatabaseName(QStringLiteral(":memory:"));
            QVERIFY(db.open());
            QString error;
            {
                SqlUtil::Savepoint outer(db, &error);
                QVERIFY(outer.active());
                QSqlQuery query(db);
                QVERIFY(query.exec(QStringLiteral("CREATE TABLE probe(value INTEGER)")));
                SqlUtil::Savepoint inner(db, &error);
                QVERIFY(inner.active());
                QVERIFY(query.exec(QStringLiteral("INSERT INTO probe VALUES(1)")));
                QVERIFY(inner.commit());
            }
            QVERIFY(!db.tables().contains(QStringLiteral("probe")));
        }
        QSqlDatabase::removeDatabase(name);
    }
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

QTEST_GUILESS_MAIN(SqlUtilTest)

#include "test_sql_util.moc"
