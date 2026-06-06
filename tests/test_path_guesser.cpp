#include "scanner/PathMetadataGuesser.h"

#include <QTest>

class PathGuesserTest final : public QObject {
    Q_OBJECT

private slots:
    void artistAlbumTrackFromHierarchy();
    void singleFolderIsArtist();
    void flatFileSplitsArtistTitle();
    void stripsTrackNumberVariants();
    void doesNotStripThreeDigitTitle();
    void cleansSeparatorsAndUnderscores();
    void emptyWhenNoStructure();
};

void PathGuesserTest::artistAlbumTrackFromHierarchy()
{
    const GuessedMetadata g = PathMetadataGuesser::guess(
        QStringLiteral("/music/Miles Davis/Kind of Blue/01 - So What.flac"), QStringLiteral("/music"));
    QCOMPARE(g.artist, QStringLiteral("Miles Davis"));
    QCOMPARE(g.albumArtist, QStringLiteral("Miles Davis"));
    QCOMPARE(g.album, QStringLiteral("Kind of Blue"));
    QCOMPARE(g.title, QStringLiteral("So What"));
    QCOMPARE(g.trackNumber, 1);
}

void PathGuesserTest::singleFolderIsArtist()
{
    const GuessedMetadata g = PathMetadataGuesser::guess(
        QStringLiteral("/music/Aphex Twin/07. Xtal.flac"), QStringLiteral("/music"));
    QCOMPARE(g.artist, QStringLiteral("Aphex Twin"));
    QVERIFY(g.album.isEmpty());
    QCOMPARE(g.title, QStringLiteral("Xtal"));
    QCOMPARE(g.trackNumber, 7);
}

void PathGuesserTest::flatFileSplitsArtistTitle()
{
    const GuessedMetadata g = PathMetadataGuesser::guess(
        QStringLiteral("/music/Boards of Canada - Roygbiv.mp3"), QStringLiteral("/music"));
    QCOMPARE(g.artist, QStringLiteral("Boards of Canada"));
    QCOMPARE(g.title, QStringLiteral("Roygbiv"));
}

void PathGuesserTest::stripsTrackNumberVariants()
{
    QCOMPARE(PathMetadataGuesser::guess(QStringLiteral("/m/A/B/3. Song.flac"), QStringLiteral("/m")).trackNumber, 3);
    QCOMPARE(PathMetadataGuesser::guess(QStringLiteral("/m/A/B/04) Song.flac"), QStringLiteral("/m")).trackNumber, 4);
    QCOMPARE(PathMetadataGuesser::guess(QStringLiteral("/m/A/B/12 Song.flac"), QStringLiteral("/m")).trackNumber, 12);
    const GuessedMetadata g = PathMetadataGuesser::guess(QStringLiteral("/m/A/B/05 - Song.flac"), QStringLiteral("/m"));
    QCOMPARE(g.trackNumber, 5);
    QCOMPARE(g.title, QStringLiteral("Song"));
}

void PathGuesserTest::doesNotStripThreeDigitTitle()
{
    // "100 Years" is a title, not "track 100" — a 3-digit run with only a space is
    // left intact.
    const GuessedMetadata g = PathMetadataGuesser::guess(
        QStringLiteral("/m/Artist/Album/100 Years.flac"), QStringLiteral("/m"));
    QCOMPARE(g.trackNumber, 0);
    QCOMPARE(g.title, QStringLiteral("100 Years"));
}

void PathGuesserTest::cleansSeparatorsAndUnderscores()
{
    const GuessedMetadata g = PathMetadataGuesser::guess(
        QStringLiteral("/m/The_Artist/The_Album/02_-_The_Track.flac"), QStringLiteral("/m"));
    QCOMPARE(g.artist, QStringLiteral("The Artist"));
    QCOMPARE(g.album, QStringLiteral("The Album"));
    QCOMPARE(g.title, QStringLiteral("The Track"));
    QCOMPARE(g.trackNumber, 2);
}

void PathGuesserTest::emptyWhenNoStructure()
{
    // A bare file at the library root with no separators yields just a title.
    const GuessedMetadata g = PathMetadataGuesser::guess(
        QStringLiteral("/music/loose.flac"), QStringLiteral("/music"));
    QVERIFY(g.artist.isEmpty());
    QVERIFY(g.album.isEmpty());
    QCOMPARE(g.title, QStringLiteral("loose"));
}

QTEST_MAIN(PathGuesserTest)
#include "test_path_guesser.moc"
