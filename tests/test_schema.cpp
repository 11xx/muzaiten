#include "db/Database.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QDir>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

class SchemaTest final : public QObject {
    Q_OBJECT

private slots:
    void migratesFreshDatabase();
    void upsertsTrackAndQueriesArtist();
    void scannedRatingOverridesUserRating();
    void pendingUserRatingOverridesScannedRating();
    void pendingRatingWritesRoundTrip();
    void tracksWithUserRatingsRoundTrip();
    void userAlbumRatingOverridesAverageRating();
    void pendingTrackRatingAffectsAlbumAverage();
    void appSettingRoundTrips();
    void linkRootRoundTrips();
    void sourceRootRoundTrips();
    void sourceRootVisibilityFiltersLocalLibrary();
    void mpdTracksRoundTrip();
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
    QCOMPARE(query.value(0).toInt(), 6);
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

void SchemaTest::scannedRatingOverridesUserRating()
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
    QCOMPARE(tracks.first().effectiveRating0To100, 80);
    QVERIFY(tracks.first().hasUserRating);

    QVERIFY2(database.clearUserTrackRating(track.path), qPrintable(database.lastError()));
    tracks = database.tracksForArtist(QStringLiteral("Album Artist"));
    QCOMPARE(tracks.first().effectiveRating0To100, 80);
    QVERIFY(!tracks.first().hasUserRating);
}

void SchemaTest::pendingUserRatingOverridesScannedRating()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    Database database(QStringLiteral("schema-pending-user-track-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    const Track track = makeTrack(temp, QStringLiteral("01.flac"), 80);
    QVERIFY2(database.upsertTrack(track), qPrintable(database.lastError()));
    QVERIFY2(database.setUserTrackRating(track.path, 30), qPrintable(database.lastError()));
    QVERIFY2(database.setPendingTrackRatingWrite(track.path, 30, QStringLiteral("pending")), qPrintable(database.lastError()));

    QVector<Track> tracks = database.tracksForArtist(QStringLiteral("Album Artist"));
    QCOMPARE(tracks.size(), 1);
    QCOMPARE(tracks.first().rating0To100, 80);
    QCOMPARE(tracks.first().effectiveRating0To100, 30);

    QVERIFY2(database.clearPendingTrackRatingWrite(track.path), qPrintable(database.lastError()));
    tracks = database.tracksForArtist(QStringLiteral("Album Artist"));
    QCOMPARE(tracks.first().effectiveRating0To100, 80);
}

void SchemaTest::pendingRatingWritesRoundTrip()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    Database database(QStringLiteral("schema-pending-rating-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    const Track track = makeTrack(temp, QStringLiteral("01.flac"), Rating::unset);
    QVERIFY2(database.upsertTrack(track), qPrintable(database.lastError()));
    QVERIFY2(database.setPendingTrackRatingWrite(track.path, 70, QStringLiteral("pending")), qPrintable(database.lastError()));

    QVector<Track> tracks = database.tracksWithPendingRatingWrites();
    QCOMPARE(tracks.size(), 1);
    QCOMPARE(tracks.first().path, track.path);
    QCOMPARE(tracks.first().effectiveRating0To100, 70);

    QVERIFY2(database.clearPendingTrackRatingWrite(track.path), qPrintable(database.lastError()));
    tracks = database.tracksWithPendingRatingWrites();
    QCOMPARE(tracks.size(), 0);
}

void SchemaTest::tracksWithUserRatingsRoundTrip()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    Database database(QStringLiteral("schema-user-ratings-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    Track track = makeTrack(temp, QStringLiteral("01.flac"), Rating::unset);
    QVERIFY2(database.upsertTrack(track), qPrintable(database.lastError()));
    QVERIFY2(database.setUserTrackRating(track.path, 60), qPrintable(database.lastError()));

    QVector<Track> tracks = database.tracksWithUserRatings();
    QCOMPARE(tracks.size(), 1);
    QCOMPARE(tracks.first().path, track.path);
    QCOMPARE(tracks.first().effectiveRating0To100, 60);

    QVERIFY2(database.updateScannedTrackRating(track.path, 90, Rating::Source::MusicBeeCompatible, 100, 200), qPrintable(database.lastError()));
    tracks = database.tracksWithUserRatings();
    QCOMPARE(tracks.first().rating0To100, 90);
    QCOMPARE(tracks.first().effectiveRating0To100, 90);
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

void SchemaTest::pendingTrackRatingAffectsAlbumAverage()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    Database database(QStringLiteral("schema-pending-album-average-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    const Track first = makeTrack(temp, QStringLiteral("01.flac"), 80);
    const Track second = makeTrack(temp, QStringLiteral("02.flac"), 40);
    QVERIFY2(database.upsertTrack(first), qPrintable(database.lastError()));
    QVERIFY2(database.upsertTrack(second), qPrintable(database.lastError()));
    QVERIFY2(database.setUserTrackRating(first.path, 20), qPrintable(database.lastError()));
    QVERIFY2(database.setPendingTrackRatingWrite(first.path, 20, QStringLiteral("pending")), qPrintable(database.lastError()));

    QVector<Album> albums = database.albumsForArtist(QStringLiteral("Album Artist"));
    QCOMPARE(albums.size(), 1);
    QCOMPARE(albums.first().effectiveRating0To100, 30);

    QVERIFY2(database.clearPendingTrackRatingWrite(first.path), qPrintable(database.lastError()));
    albums = database.albumsForArtist(QStringLiteral("Album Artist"));
    QCOMPARE(albums.first().effectiveRating0To100, 60);
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

void SchemaTest::sourceRootRoundTrips()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QVERIFY(QDir().mkpath(temp.filePath(QStringLiteral("music"))));

    Database database(QStringLiteral("schema-source-root-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    ScanRoot root;
    root.name = QStringLiteral("Local Music");
    root.path = temp.filePath(QStringLiteral("music/"));
    root.scanEnabled = true;
    root.libraryEnabled = false;
    QVERIFY2(database.saveScanRoot(root), qPrintable(database.lastError()));

    QVector<ScanRoot> roots = database.scanRoots();
    QCOMPARE(roots.size(), 1);
    QCOMPARE(roots.first().name, QStringLiteral("Local Music"));
    QCOMPARE(roots.first().path, QDir::cleanPath(temp.filePath(QStringLiteral("music"))));
    QVERIFY(roots.first().scanEnabled);
    QVERIFY(!roots.first().libraryEnabled);

    root = roots.first();
    root.scanEnabled = false;
    root.libraryEnabled = true;
    QVERIFY2(database.saveScanRoot(root), qPrintable(database.lastError()));
    QVERIFY2(database.setScanRootLastScanned(root.id), qPrintable(database.lastError()));

    roots = database.scanRoots();
    QCOMPARE(roots.size(), 1);
    QVERIFY(!roots.first().scanEnabled);
    QVERIFY(roots.first().libraryEnabled);
    QVERIFY(!roots.first().lastScannedAt.isEmpty());

    QVERIFY2(database.removeScanRoot(roots.first().id), qPrintable(database.lastError()));
    QVERIFY(database.scanRoots().isEmpty());
}

void SchemaTest::sourceRootVisibilityFiltersLocalLibrary()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QVERIFY(QDir().mkpath(temp.filePath(QStringLiteral("music/Artist/Album"))));
    QVERIFY(QDir().mkpath(temp.filePath(QStringLiteral("other/Other/Album"))));

    Database database(QStringLiteral("schema-source-filter-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    Track visible = makeTrack(temp, QStringLiteral("01.flac"), 80);
    visible.path = temp.filePath(QStringLiteral("music/Artist/Album/01.flac"));
    visible.parentDir = temp.filePath(QStringLiteral("music/Artist/Album"));
    Track hidden = makeTrack(temp, QStringLiteral("02.flac"), 70);
    hidden.path = temp.filePath(QStringLiteral("other/Other/Album/02.flac"));
    hidden.parentDir = temp.filePath(QStringLiteral("other/Other/Album"));
    hidden.albumArtistName = QStringLiteral("Other Artist");
    hidden.artistName = QStringLiteral("Other Artist");

    QVERIFY2(database.upsertTrack(visible), qPrintable(database.lastError()));
    QVERIFY2(database.upsertTrack(hidden), qPrintable(database.lastError()));
    QCOMPARE(database.albumArtists().size(), 2);

    ScanRoot root;
    root.path = temp.filePath(QStringLiteral("music"));
    root.libraryEnabled = true;
    QVERIFY2(database.saveScanRoot(root), qPrintable(database.lastError()));

    QVector<Artist> artists = database.albumArtists();
    QCOMPARE(artists.size(), 1);
    QCOMPARE(artists.first().name, QStringLiteral("Album Artist"));
    QCOMPARE(database.tracksForArtist(QStringLiteral("Album Artist")).size(), 1);
    QVERIFY(database.tracksForArtist(QStringLiteral("Other Artist")).isEmpty());

    QVector<ScanRoot> roots = database.scanRoots();
    roots.first().libraryEnabled = false;
    QVERIFY2(database.saveScanRoot(roots.first()), qPrintable(database.lastError()));
    QVERIFY(database.albumArtists().isEmpty());

    roots = database.scanRoots();
    roots.first().libraryEnabled = true;
    QVERIFY2(database.saveScanRoot(roots.first()), qPrintable(database.lastError()));
    QCOMPARE(database.localLibraryDirectories().value(0), QDir::cleanPath(temp.filePath(QStringLiteral("music"))));
    QCOMPARE(database.localLibraryDirectories(temp.filePath(QStringLiteral("music"))).value(0),
             QDir::cleanPath(temp.filePath(QStringLiteral("music/Artist"))));
}

void SchemaTest::mpdTracksRoundTrip()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    Database database(QStringLiteral("schema-mpd-track-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    const qint64 sourceId = database.upsertMediaSource(QStringLiteral("mpd"),
                                                       QStringLiteral("local"),
                                                       QStringLiteral("/gak/music"),
                                                       QStringLiteral("/home/lobo/.config/mpd/mpd.conf"));
    QVERIFY2(sourceId > 0, qPrintable(database.lastError()));

    MpdTrack track;
    track.uri = QStringLiteral("Artist/Album/01.flac");
    track.title = QStringLiteral("Track");
    track.artistName = QStringLiteral("Artist");
    track.albumArtistName = QStringLiteral("Album Artist");
    track.albumTitle = QStringLiteral("Album");
    track.trackNumber = 1;
    track.durationMs = 123000;
    track.musicBrainz.recordingId = QStringLiteral("recording-id");
    QVERIFY2(database.upsertMpdTrack(sourceId, track), qPrintable(database.lastError()));
    QCOMPARE(database.mpdTrackCount(sourceId), 1);
    QCOMPARE(database.mpdSourceId(), sourceId);

    const QVector<Artist> artists = database.mpdAlbumArtists();
    QCOMPARE(artists.size(), 1);
    QCOMPARE(artists.first().name, QStringLiteral("Album Artist"));
    QCOMPARE(artists.first().albumCount, 1);

    const QVector<Album> albums = database.mpdAlbumsForArtist(QStringLiteral("Album Artist"), QStringLiteral("/gak/music"));
    QCOMPARE(albums.size(), 1);
    QCOMPARE(albums.first().title, QStringLiteral("Album"));
    QCOMPARE(albums.first().representativeDir, QStringLiteral("/gak/music/Artist/Album"));

    const QVector<Track> tracks = database.mpdTracksForArtist(QStringLiteral("Album Artist"), QStringLiteral("/gak/music"));
    QCOMPARE(tracks.size(), 1);
    QCOMPARE(tracks.first().path, QStringLiteral("/gak/music/Artist/Album/01.flac"));
    QCOMPARE(tracks.first().title, QStringLiteral("Track"));
    QCOMPARE(tracks.first().durationMs, 123000);

    track.title = QStringLiteral("Updated Track");
    QVERIFY2(database.upsertMpdTrack(sourceId, track), qPrintable(database.lastError()));
    QCOMPARE(database.mpdTrackCount(sourceId), 1);
    QVERIFY2(database.clearMpdTracksForSource(sourceId), qPrintable(database.lastError()));
    QCOMPARE(database.mpdTrackCount(sourceId), 0);
}

QTEST_MAIN(SchemaTest)
#include "test_schema.moc"
