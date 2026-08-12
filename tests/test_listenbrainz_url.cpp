#include "scrobble/ListenBrainzUrl.h"

#include <QTest>

class ListenBrainzUrlTest final : public QObject {
    Q_OBJECT

private slots:
    void appendsVersionSegmentToServerRoot();
    void absorbsExplicitVersionSegment();
    void preservesPathPrefix();
    void preservesPort();
    void lowercasesHost();
    void rejectsInvalidBases_data();
    void rejectsInvalidBases();
    void buildsEndpointsFromApiRoot();
};

void ListenBrainzUrlTest::appendsVersionSegmentToServerRoot()
{
    const auto result = ListenBrainzUrl::normalizeBase(QStringLiteral("https://koito.example"));
    QVERIFY(result.valid);
    QCOMPARE(result.apiRoot, QStringLiteral("https://koito.example/1"));

    // Surrounding whitespace and a trailing slash are noise, not meaning.
    QCOMPARE(ListenBrainzUrl::normalizeBase(QStringLiteral("  https://koito.example/  ")).apiRoot,
             QStringLiteral("https://koito.example/1"));
}

void ListenBrainzUrlTest::absorbsExplicitVersionSegment()
{
    // Pointing at the server root and at the API root must land on one root, so
    // that the same server entered either way stays a single destination.
    QCOMPARE(ListenBrainzUrl::normalizeBase(QStringLiteral("https://api.listenbrainz.org/1")).apiRoot,
             ListenBrainzUrl::officialApiRoot());
    QCOMPARE(ListenBrainzUrl::normalizeBase(QStringLiteral("https://api.listenbrainz.org")).apiRoot,
             ListenBrainzUrl::officialApiRoot());
    QCOMPARE(ListenBrainzUrl::normalizeBase(QStringLiteral("https://api.listenbrainz.org/1/")).apiRoot,
             ListenBrainzUrl::officialApiRoot());
}

void ListenBrainzUrlTest::preservesPathPrefix()
{
    QCOMPARE(ListenBrainzUrl::normalizeBase(QStringLiteral("https://example.test/music")).apiRoot,
             QStringLiteral("https://example.test/music/1"));
    QCOMPARE(ListenBrainzUrl::normalizeBase(QStringLiteral("https://example.test/music/1")).apiRoot,
             QStringLiteral("https://example.test/music/1"));
    QCOMPARE(ListenBrainzUrl::normalizeBase(QStringLiteral("https://example.test/a/b/c")).apiRoot,
             QStringLiteral("https://example.test/a/b/c/1"));
    // Only a trailing version segment is absorbed; one inside the prefix stays.
    QCOMPARE(ListenBrainzUrl::normalizeBase(QStringLiteral("https://example.test/1/koito")).apiRoot,
             QStringLiteral("https://example.test/1/koito/1"));
}

void ListenBrainzUrlTest::preservesPort()
{
    QCOMPARE(ListenBrainzUrl::normalizeBase(QStringLiteral("http://127.0.0.1:4110")).apiRoot,
             QStringLiteral("http://127.0.0.1:4110/1"));
    QCOMPARE(ListenBrainzUrl::normalizeBase(QStringLiteral("http://127.0.0.1:4110/koito/1")).apiRoot,
             QStringLiteral("http://127.0.0.1:4110/koito/1"));
}

void ListenBrainzUrlTest::lowercasesHost()
{
    QCOMPARE(ListenBrainzUrl::normalizeBase(QStringLiteral("https://Koito.EXAMPLE/Music")).apiRoot,
             QStringLiteral("https://koito.example/Music/1"));
}

void ListenBrainzUrlTest::rejectsInvalidBases_data()
{
    QTest::addColumn<QString>("input");

    QTest::newRow("empty") << QString();
    QTest::newRow("blank") << QStringLiteral("   ");
    QTest::newRow("no scheme") << QStringLiteral("koito.example");
    QTest::newRow("ftp scheme") << QStringLiteral("ftp://koito.example");
    QTest::newRow("file scheme") << QStringLiteral("file:///tmp/koito");
    QTest::newRow("no host") << QStringLiteral("https:///1");
    QTest::newRow("credentials") << QStringLiteral("https://user:pass@koito.example");
    QTest::newRow("user only") << QStringLiteral("https://user@koito.example");
    QTest::newRow("query") << QStringLiteral("https://koito.example/1?token=secret");
    QTest::newRow("fragment") << QStringLiteral("https://koito.example/1#anchor");
    QTest::newRow("dot segment") << QStringLiteral("https://koito.example/./1");
    QTest::newRow("dotdot segment") << QStringLiteral("https://koito.example/a/../1");
}

void ListenBrainzUrlTest::rejectsInvalidBases()
{
    QFETCH(QString, input);

    const auto result = ListenBrainzUrl::normalizeBase(input);
    QVERIFY(!result.valid);
    QVERIFY(result.apiRoot.isEmpty());
    QVERIFY(!result.error.isEmpty());
}

void ListenBrainzUrlTest::buildsEndpointsFromApiRoot()
{
    const QString root = ListenBrainzUrl::normalizeBase(QStringLiteral("https://example.test/music")).apiRoot;
    QCOMPARE(ListenBrainzUrl::submitListensUrl(root), QStringLiteral("https://example.test/music/1/submit-listens"));
    QCOMPARE(ListenBrainzUrl::validateTokenUrl(root), QStringLiteral("https://example.test/music/1/validate-token"));

    QCOMPARE(ListenBrainzUrl::submitListensUrl(ListenBrainzUrl::officialApiRoot()),
             QStringLiteral("https://api.listenbrainz.org/1/submit-listens"));
    QCOMPARE(ListenBrainzUrl::validateTokenUrl(ListenBrainzUrl::officialApiRoot()),
             QStringLiteral("https://api.listenbrainz.org/1/validate-token"));
}

QTEST_MAIN(ListenBrainzUrlTest)
#include "test_listenbrainz_url.moc"
