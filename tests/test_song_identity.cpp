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
    void narrowTraversalMatchesFullResolution();
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

void SongIdentityTest::narrowTraversalMatchesFullResolution()
{
    const QList<SongIdentity::TrackIdentity> tracks{
        // Same MBID despite different titles; the second row also bridges into
        // a content group, making the component transitive rather than a
        // one-shot union of the target's own keys.
        {QStringLiteral("/mbid-a.flac"), QStringLiteral("Artist A"), QStringLiteral("Title A"),
         QStringLiteral("shared-mbid"), -1},
        {QStringLiteral("/mbid-b.flac"), QStringLiteral("Artist B"), QStringLiteral("Different title"),
         QStringLiteral("shared-mbid"), 42},
        {QStringLiteral("/group-c.flac"), QStringLiteral("Artist C"), QStringLiteral("Third title"),
         QStringLiteral("other-mbid"), 42},
        // Equal titles alone do not merge different artists.
        {QStringLiteral("/same-title-a.flac"), QStringLiteral("Alpha"), QStringLiteral("Same title"), {}, -1},
        {QStringLiteral("/same-title-b.flac"), QStringLiteral("Beta"), QStringLiteral("Same title"), {}, -1},
        // Folded artist/title fallback still merges MBID-less copies.
        {QStringLiteral("/folded-a.flac"), QStringLiteral(" Folded  Artist "), QStringLiteral(" Song "), {}, -1},
        {QStringLiteral("/folded-b.flac"), QStringLiteral("folded artist"), QStringLiteral("song"), {}, -1},
        {QStringLiteral("/singleton.flac"), QStringLiteral("Solo"), QStringLiteral("Only"), {}, -1},
    };

    const QHash<QString, QString> fullKeys = SongIdentity::resolvedSongKeys(tracks);
    const auto fullPaths = [&fullKeys](const QString &targetPath) {
        QStringList paths;
        const QString key = fullKeys.value(targetPath);
        for (auto it = fullKeys.cbegin(); it != fullKeys.cend(); ++it) {
            if (it.value() == key) {
                paths.push_back(it.key());
            }
        }
        paths.sort(Qt::CaseInsensitive);
        return paths;
    };

    for (const SongIdentity::TrackIdentity &track : tracks) {
        QCOMPARE(SongIdentity::pathsConnectedToTrack(tracks, track.path), fullPaths(track.path));
    }
}

QTEST_MAIN(SongIdentityTest)
#include "test_song_identity.moc"
