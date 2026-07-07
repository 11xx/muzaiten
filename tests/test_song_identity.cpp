#include "features/SongIdentity.h"

#include <QTest>

class SongIdentityTest final : public QObject {
    Q_OBJECT

private slots:
    void contentGroupBeatsMbidInequality();
    void mbidEqualityMerges();
    void foldedFallbackMergesMbidlessRows();
    void disjointRowsStayDisjoint();
    void closedFeatureStoreShapeMatchesOldKeys();
};

void SongIdentityTest::contentGroupBeatsMbidInequality()
{
    const QHash<QString, QString> keys = SongIdentity::resolvedSongKeys({
        {QStringLiteral("/a.flac"), QStringLiteral("Artist"), QStringLiteral("Song"), QStringLiteral("mbid-a"), 42},
        {QStringLiteral("/b.flac"), QStringLiteral("Different"), QStringLiteral("Tags"), QStringLiteral("mbid-b"), 42},
    });
    QCOMPARE(keys.value(QStringLiteral("/a.flac")), QStringLiteral("cg:42"));
    QCOMPARE(keys.value(QStringLiteral("/b.flac")), QStringLiteral("cg:42"));
}

void SongIdentityTest::mbidEqualityMerges()
{
    const QHash<QString, QString> keys = SongIdentity::resolvedSongKeys({
        {QStringLiteral("/a.flac"), QStringLiteral("Artist"), QStringLiteral("Song"), QStringLiteral("same"), -1},
        {QStringLiteral("/b.flac"), QStringLiteral("Other"), QStringLiteral("Title"), QStringLiteral("same"), -1},
    });
    QCOMPARE(keys.value(QStringLiteral("/a.flac")), QStringLiteral("mbid:same"));
    QCOMPARE(keys.value(QStringLiteral("/b.flac")), QStringLiteral("mbid:same"));
}

void SongIdentityTest::foldedFallbackMergesMbidlessRows()
{
    const QHash<QString, QString> keys = SongIdentity::resolvedSongKeys({
        {QStringLiteral("/a.flac"), QStringLiteral(" Artist "), QStringLiteral("Song"), {}, -1},
        {QStringLiteral("/b.flac"), QStringLiteral("artist"), QStringLiteral(" song "), {}, -1},
    });
    QCOMPARE(keys.value(QStringLiteral("/a.flac")), QStringLiteral("at:artist\nsong"));
    QCOMPARE(keys.value(QStringLiteral("/b.flac")), QStringLiteral("at:artist\nsong"));
}

void SongIdentityTest::disjointRowsStayDisjoint()
{
    const QHash<QString, QString> keys = SongIdentity::resolvedSongKeys({
        {QStringLiteral("/a.flac"), QStringLiteral("Artist"), QStringLiteral("Song A"), {}, -1},
        {QStringLiteral("/b.flac"), QStringLiteral("Artist"), QStringLiteral("Song B"), {}, -1},
    });
    QVERIFY(keys.value(QStringLiteral("/a.flac")) != keys.value(QStringLiteral("/b.flac")));
}

void SongIdentityTest::closedFeatureStoreShapeMatchesOldKeys()
{
    const QHash<QString, QString> keys = SongIdentity::resolvedSongKeys({
        {QStringLiteral("/a.flac"), QStringLiteral("Artist"), QStringLiteral("Song"), QStringLiteral("mbid-a"), -1},
        {QStringLiteral("/b.flac"), QStringLiteral("Artist"), QStringLiteral("Song"), QStringLiteral("mbid-b"), -1},
        {QStringLiteral("/c.flac"), QStringLiteral("Artist"), QStringLiteral("Song"), {}, -1},
    });
    QCOMPARE(keys.value(QStringLiteral("/a.flac")), QStringLiteral("mbid:mbid-a"));
    QCOMPARE(keys.value(QStringLiteral("/b.flac")), QStringLiteral("mbid:mbid-b"));
    QCOMPARE(keys.value(QStringLiteral("/c.flac")), QStringLiteral("at:artist\nsong"));
}

QTEST_MAIN(SongIdentityTest)
#include "test_song_identity.moc"
