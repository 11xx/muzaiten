#include "db/Database.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

class SchemaTest final : public QObject {
    Q_OBJECT

private slots:
    void migratesFreshDatabase();
    void upsertsTrackAndQueriesArtist();
    void userTrackRatingOverridesScannedRating();
    void userAlbumRatingOverridesAverageRating();
    void appSettingRoundTrips();
    void linkRootRoundTrips();
};

namespace {

Track makeTrack(const QTemporaryDir &temp, const QString &filename, int rating)
{
    Track track;
    track.path = temp.filePath(QStringLiteral("Artist/Album/%1").arg(filename));
    track.parentDir = temp.filePath(QStringLiteral("Artist/Album"));
    track.filename = filename;
    track.title = filename;
    track.artistName = QStringLiteral("Artist");
    track.albumArtistName = QStringLiteral("Album Artist");
    track.albumTitle = QStringLiteral("Album");
    track.trackNumber = filename.left(2).toInt();
    track.rating0To100 = rating;
    track.ratingSource = Rating::Source::VorbisRating;
    track.fileSize = 10;
    track.fileMtime = 20;
    return track;
}

} // namespace

void SchemaTest::migratesFreshDatabase()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    const QString connectionName = QStringLiteral("schema-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    Database database(connectionName);
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    QSqlQuery query(QSqlDatabase::database(connectionName));
    QVERIFY(query.exec(QStringLiteral("SELECT MAX(version) FROM schema_migrations")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), 3);
}

void SchemaTest::upsertsTrackAndQueriesArtist()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    Database database(QStringLiteral("schema-track-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    Track track = makeTrack(temp, QStringLiteral("01.flac"), 80);
    track.title = QStringLiteral("Track");

    QVERIFY2(database.upsertTrack(track), qPrintable(database.lastError()));
    const QVector<Artist> artists = database.albumArtists();
    QCOMPARE(artists.size(), 1);
    QCOMPARE(artists.first().name, QStringLiteral("Album Artist"));
    QCOMPARE(artists.first().albumCount, 1);

    const QVector<Track> tracks = database.tracksForArtist(QStringLiteral("Album Artist"));
    QCOMPARE(tracks.size(), 1);
    QCOMPARE(tracks.first().rating0To100, 80);
    QCOMPARE(tracks.first().effectiveRating0To100, 80);
}

void SchemaTest::userTrackRatingOverridesScannedRating()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    Database database(QStringLiteral("schema-user-track-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    const Track track = makeTrack(temp, QStringLiteral("01.flac"), 80);
    QVERIFY2(database.upsertTrack(track), qPrintable(database.lastError()));
    QVERIFY2(database.setUserTrackRating(track.path, 30), qPrintable(database.lastError()));

    QVector<Track> tracks = database.tracksForArtist(QStringLiteral("Album Artist"));
    QCOMPARE(tracks.size(), 1);
    QCOMPARE(tracks.first().rating0To100, 80);
    QCOMPARE(tracks.first().effectiveRating0To100, 30);
    QVERIFY(tracks.first().hasUserRating);

    QVERIFY2(database.clearUserTrackRating(track.path), qPrintable(database.lastError()));
    tracks = database.tracksForArtist(QStringLiteral("Album Artist"));
    QCOMPARE(tracks.first().effectiveRating0To100, 80);
    QVERIFY(!tracks.first().hasUserRating);
}

void SchemaTest::userAlbumRatingOverridesAverageRating()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    Database database(QStringLiteral("schema-user-album-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    QVERIFY2(database.upsertTrack(makeTrack(temp, QStringLiteral("01.flac"), 80)), qPrintable(database.lastError()));
    QVERIFY2(database.upsertTrack(makeTrack(temp, QStringLiteral("02.flac"), 40)), qPrintable(database.lastError()));

    QVector<Album> albums = database.albumsForArtist(QStringLiteral("Album Artist"));
    QCOMPARE(albums.size(), 1);
    QCOMPARE(albums.first().effectiveRating0To100, 60);

    QVERIFY2(database.setUserAlbumRating(QStringLiteral("Album Artist"), QStringLiteral("Album"), 90), qPrintable(database.lastError()));
    albums = database.albumsForArtist(QStringLiteral("Album Artist"));
    QCOMPARE(albums.first().effectiveRating0To100, 90);
    QVERIFY(albums.first().hasUserRating);
}

void SchemaTest::appSettingRoundTrips()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    Database database(QStringLiteral("schema-setting-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));
    QCOMPARE(database.setting(QStringLiteral("missing"), QStringLiteral("fallback")), QStringLiteral("fallback"));
    QVERIFY2(database.setSetting(QStringLiteral("trackTable.view"), QStringLiteral("{\"rowHeight\":24}")), qPrintable(database.lastError()));
    QCOMPARE(database.setting(QStringLiteral("trackTable.view")), QStringLiteral("{\"rowHeight\":24}"));
}

void SchemaTest::linkRootRoundTrips()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    Database database(QStringLiteral("schema-link-root-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    LinkRoot root;
    root.name = QStringLiteral("Writable mirror");
    root.sourcePrefix = QStringLiteral("/gak/music");
    root.targetPrefix = QStringLiteral("/gak/turak/music");
    root.priority = 100;
    root.readable = true;
    root.writable = true;
    QVERIFY2(database.saveLinkRoot(root), qPrintable(database.lastError()));

    const QVector<LinkRoot> roots = database.linkRoots();
    QCOMPARE(roots.size(), 1);
    QCOMPARE(roots.first().name, QStringLiteral("Writable mirror"));
    QCOMPARE(roots.first().sourcePrefix, QStringLiteral("/gak/music"));
    QCOMPARE(roots.first().targetPrefix, QStringLiteral("/gak/turak/music"));
    QCOMPARE(roots.first().priority, 100);
    QVERIFY(roots.first().readable);
    QVERIFY(roots.first().writable);
    QVERIFY(roots.first().enabled);
}

QTEST_MAIN(SchemaTest)
#include "test_schema.moc"
