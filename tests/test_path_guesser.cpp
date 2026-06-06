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
    void initialArtistAlbumHierarchyUsesLastDirs();
    void albumFolderCanCarryArtistPrefix();
    void utilityDirectoriesStayOutOfBrowseGuesses();
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
        QStringLiteral("/m/@The_Artist/[2020] The_Album [24bit 96kHz FLAC]/02_-_The_Track.flac"), QStringLiteral("/m"));
    QCOMPARE(g.artist, QStringLiteral("The Artist"));
    QCOMPARE(g.album, QStringLiteral("The Album"));
    QCOMPARE(g.title, QStringLiteral("The Track"));
    QCOMPARE(g.trackNumber, 2);
}

void PathGuesserTest::initialArtistAlbumHierarchyUsesLastDirs()
{
    const GuessedMetadata g = PathMetadataGuesser::guess(
        QStringLiteral("/music/D/Disturbed/[2010] Asylum (deluxe edition) [16bit 44.1kHz FLAC]/12. Innocence.flac"),
        QStringLiteral("/music"));
    QCOMPARE(g.artist, QStringLiteral("Disturbed"));
    QCOMPARE(g.album, QStringLiteral("Asylum (deluxe edition)"));
    QCOMPARE(g.title, QStringLiteral("Innocence"));
    QCOMPARE(g.trackNumber, 12);
}

void PathGuesserTest::albumFolderCanCarryArtistPrefix()
{
    const GuessedMetadata g = PathMetadataGuesser::guess(
        QStringLiteral("/music/dumps/FLAC Library/@Radiohead/Radiohead - Amnesiac (2001) [FLAC 24-192]/03. Pulk.flac"),
        QStringLiteral("/music"));
    QCOMPARE(g.artist, QStringLiteral("Radiohead"));
    QCOMPARE(g.album, QStringLiteral("Amnesiac (2001)"));
    QCOMPARE(g.title, QStringLiteral("Pulk"));
    QCOMPARE(g.trackNumber, 3);
}

void PathGuesserTest::utilityDirectoriesStayOutOfBrowseGuesses()
{
    const GuessedMetadata g = PathMetadataGuesser::guess(
        QStringLiteral("/music/Artist/Album/Covers/cover.flac"), QStringLiteral("/music"));
    QVERIFY(g.artist.isEmpty());
    QVERIFY(g.album.isEmpty());
    QCOMPARE(g.title, QStringLiteral("cover"));
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
