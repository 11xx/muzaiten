#include "core/GenreTags.h"
#include "core/MetadataBlob.h"
#include "db/Database.h"
#include "db/Schema.h"
#include "search/IndexCache.h"
#include "search/SearchRecord.h"

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
    void enumeratedPlaceholdersStayIsolatedUntilScanned();
    void guessedPlaceholdersFollowVisibilitySetting();
    void upsertsTrackAndQueriesArtist();
    void sortTagsFoldIntoSearchIndex();
    void scannedRatingOverridesUserRating();
    void pendingUserRatingOverridesScannedRating();
    void trackFlagsRoundTrip();
    void searchTracksLikeUsesPendingRatingOverlay();
    void pendingRatingWritesRoundTrip();
    void tracksWithUserRatingsRoundTrip();
    void missingTrackCleanupReturnsPaths();
    void userAlbumRatingOverridesAverageRating();
    void pendingTrackRatingAffectsAlbumAverage();
    void appSettingRoundTrips();
    void linkRootRoundTrips();
    void sourceRootRoundTrips();
    void sourceRootVisibilityFiltersLocalLibrary();
    void mpdTracksRoundTrip();
    void searchCacheRoundTrips();
    void searchCacheSignatureDetectsChange();
    void databaseCacheMemoryCanBeReleasedAndRestored();
    void trackGenresPopulatedOnUpsert();
    void trackGenresBackfillOnceFromBlobs();
    void trackGenresCascadeOnTrackDelete();
    void genreAliasesRoundTripAndMigrationIsIdempotent();
    void genreTagsSplitsAndFolds();
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
    QCOMPARE(query.value(0).toInt(), Schema::currentVersion);
    QVERIFY(query.exec(QStringLiteral("SELECT 1 FROM radio_ignored_genres LIMIT 1")));
    QVERIFY(query.exec(QStringLiteral("SELECT 1 FROM radio_weight_profiles LIMIT 1")));
}

void SchemaTest::databaseCacheMemoryCanBeReleasedAndRestored()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    const QString connectionName = QStringLiteral("schema-cache-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    Database database(connectionName);
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    QSqlQuery query(QSqlDatabase::database(connectionName));
    QVERIFY(query.exec(QStringLiteral("PRAGMA cache_size")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), -65536);

    database.releaseCacheMemory();
    QVERIFY(query.exec(QStringLiteral("PRAGMA cache_size")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), -2000);

    database.restoreCacheMemory();
    QVERIFY(query.exec(QStringLiteral("PRAGMA cache_size")));
    QVERIFY(query.next());
    QCOMPARE(query.value(0).toInt(), -65536);
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

void SchemaTest::sortTagsFoldIntoSearchIndex()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    Database database(QStringLiteral("schema-sort-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    Track track = makeTrack(temp, QStringLiteral("01.flac"), 80);
    track.title = QString::fromUtf8("花");
    track.artistName = QString::fromUtf8("宇多田ヒカル");
    track.artistSort = QStringLiteral("Utada, Hikaru");
    QVERIFY2(database.upsertTrack(track), qPrintable(database.lastError()));

    const QVector<Search::SearchRecord> recs = database.allTracksForSearch();
    QCOMPARE(recs.size(), 1);
    // The artist's romaji sort name is folded into the artist field, so "utada"
    // matches the original-script name; the kanji title romanizes via the dict.
    QVERIFY(recs.first().normArtist.contains(QStringLiteral("utada")));
    QVERIFY(recs.first().normTitle.contains(QStringLiteral("hana")));
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

void SchemaTest::trackFlagsRoundTrip()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    Database database(QStringLiteral("schema-track-flags-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    const QString pathA = temp.filePath(QStringLiteral("Artist/Album/01.flac"));
    const QString pathB = temp.filePath(QStringLiteral("Artist/Album/02.flac"));
    QVERIFY(!database.trackFlag(pathA, Database::TrackFlag::NeverRadio));
    QVERIFY(!database.trackFlag(pathA, Database::TrackFlag::NoLearn));

    QVERIFY2(database.setTrackFlag(pathA, Database::TrackFlag::NeverRadio, true), qPrintable(database.lastError()));
    QVERIFY(database.trackFlag(pathA, Database::TrackFlag::NeverRadio));
    QVERIFY(!database.trackFlag(pathA, Database::TrackFlag::NoLearn));
    QCOMPARE(database.flaggedPaths(Database::TrackFlag::NeverRadio), QSet<QString>({pathA}));
    QVERIFY(database.flaggedPaths(Database::TrackFlag::NoLearn).isEmpty());

    QVERIFY2(database.setTrackFlag(pathA, Database::TrackFlag::NoLearn, true), qPrintable(database.lastError()));
    QVERIFY2(database.setTrackFlag(pathB, Database::TrackFlag::NoLearn, true), qPrintable(database.lastError()));
    QVERIFY(database.trackFlag(pathA, Database::TrackFlag::NeverRadio));
    QCOMPARE(database.flaggedPaths(Database::TrackFlag::NoLearn), QSet<QString>({pathA, pathB}));

    QVERIFY2(database.setTrackFlag(pathA, Database::TrackFlag::NeverRadio, false), qPrintable(database.lastError()));
    QVERIFY(!database.trackFlag(pathA, Database::TrackFlag::NeverRadio));
    QVERIFY(database.trackFlag(pathA, Database::TrackFlag::NoLearn));
    QVERIFY(database.flaggedPaths(Database::TrackFlag::NeverRadio).isEmpty());
}

void SchemaTest::searchTracksLikeUsesPendingRatingOverlay()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    Database database(QStringLiteral("schema-search-pending-rating-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    Track track = makeTrack(temp, QStringLiteral("01.flac"), 80);
    track.title = QStringLiteral("Searchable Song");
    QVERIFY2(database.upsertTrack(track), qPrintable(database.lastError()));
    QVERIFY2(database.setUserTrackRating(track.path, 30), qPrintable(database.lastError()));
    QVERIFY2(database.setPendingTrackRatingWrite(track.path, 30, QStringLiteral("failed")), qPrintable(database.lastError()));

    QVector<Track> tracks = database.searchTracksLike(QStringLiteral("Searchable"), 10);
    QCOMPARE(tracks.size(), 1);
    QCOMPARE(tracks.first().rating0To100, 80);
    QCOMPARE(tracks.first().effectiveRating0To100, 30);

    QVERIFY2(database.clearPendingTrackRatingWrite(track.path), qPrintable(database.lastError()));
    tracks = database.searchTracksLike(QStringLiteral("Searchable"), 10);
    QCOMPARE(tracks.size(), 1);
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

void SchemaTest::missingTrackCleanupReturnsPaths()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    Database database(QStringLiteral("schema-missing-cleanup-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    const Track missing = makeTrack(temp, QStringLiteral("01.flac"), 80);
    const Track kept = makeTrack(temp, QStringLiteral("02.flac"), 60);
    QVERIFY2(database.upsertTrack(missing), qPrintable(database.lastError()));
    QVERIFY2(database.upsertTrack(kept), qPrintable(database.lastError()));

    QCOMPARE(database.markTracksMissing({missing.path}), 1);
    QCOMPARE(database.missingTrackPaths(), QStringList{missing.path});
    QCOMPARE(database.missingTrackCount(), 1);
    QVERIFY(database.trackForPath(missing.path).missing);

    QCOMPARE(database.removeMissingTracks(), 1);
    QVERIFY(database.missingTrackPaths().isEmpty());
    QVERIFY(database.trackForPath(missing.path).path.isEmpty());
    QCOMPARE(database.trackForPath(kept.path).path, kept.path);
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

void SchemaTest::enumeratedPlaceholdersStayIsolatedUntilScanned()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString connectionName = QStringLiteral("schema-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    Database database(connectionName);
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    const Track full = makeTrack(temp, QStringLiteral("01 Song.flac"), 80);
    Track placeholder;
    placeholder.path = full.path;
    placeholder.parentDir = full.parentDir;
    placeholder.filename = full.filename;
    placeholder.title = full.filename;
    placeholder.fileSize = 111;
    placeholder.fileMtime = 222;

    QVERIFY2(database.insertEnumeratedPlaceholders({placeholder}), qPrintable(database.lastError()));

    // Visible in the directory/file view...
    const QVector<Track> dirTracks = database.tracksForDirectory(full.parentDir);
    QCOMPARE(dirTracks.size(), 1);
    QCOMPARE(dirTracks.first().path, full.path);
    // ...but isolated from the artist/album browse and the search index.
    QVERIFY(database.albumArtists().isEmpty());
    QVERIFY(database.tracksForArtist(QStringLiteral("Album Artist")).isEmpty());
    QVERIFY(database.allTracksForSearch().isEmpty());

    // The rescan diff must re-queue an enumerated-only placeholder.
    auto fingerprints = database.trackFingerprints();
    QVERIFY(fingerprints.contains(full.path));
    QVERIFY(!fingerprints.value(full.path).metadataScanned);

    // The metadata pass upserts full tags and flips the row to scanned.
    QVERIFY2(database.upsertTrack(full), qPrintable(database.lastError()));
    QCOMPARE(database.albumArtists().size(), 1);
    QCOMPARE(database.tracksForArtist(QStringLiteral("Album Artist")).size(), 1);
    fingerprints = database.trackFingerprints();
    QVERIFY(fingerprints.value(full.path).metadataScanned);

    // A late placeholder for an already-scanned path must not reset it (DO NOTHING).
    QVERIFY2(database.insertEnumeratedPlaceholders({placeholder}), qPrintable(database.lastError()));
    QCOMPARE(database.albumArtists().size(), 1);
    fingerprints = database.trackFingerprints();
    QVERIFY(fingerprints.value(full.path).metadataScanned);
}

void SchemaTest::guessedPlaceholdersFollowVisibilitySetting()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString connectionName = QStringLiteral("schema-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    Database database(connectionName);
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    // A path-guessed placeholder: enumerated-only (metadata_scanned=0) but carrying
    // a guessed album_artist_name.
    Track guessed;
    guessed.path = temp.filePath(QStringLiteral("Guessed Artist/Guessed Album/01 Song.flac"));
    guessed.parentDir = temp.filePath(QStringLiteral("Guessed Artist/Guessed Album"));
    guessed.filename = QStringLiteral("01 Song.flac");
    guessed.title = QStringLiteral("Song");
    guessed.artistName = QStringLiteral("Guessed Artist");
    guessed.albumArtistName = QStringLiteral("Guessed Artist");
    guessed.albumTitle = QStringLiteral("Guessed Album");
    guessed.trackNumber = 1;
    guessed.fileSize = 1;
    guessed.fileMtime = 1;
    QVERIFY2(database.insertEnumeratedPlaceholders({guessed}), qPrintable(database.lastError()));

    // A blank placeholder (no guessed artist) must never appear in the browse.
    Track blank;
    blank.path = temp.filePath(QStringLiteral("loose/track.flac"));
    blank.parentDir = temp.filePath(QStringLiteral("loose"));
    blank.filename = QStringLiteral("track.flac");
    blank.title = QStringLiteral("track");
    blank.fileSize = 1;
    blank.fileMtime = 1;
    QVERIFY2(database.insertEnumeratedPlaceholders({blank}), qPrintable(database.lastError()));

    // Setting off (default): guessed rows are hidden from the artist/album browse.
    QVERIFY(database.albumArtists().isEmpty());

    // Setting on: the guessed row shows (the blank one still does not).
    database.setGuessedPlaceholdersVisible(true);
    const QVector<Artist> artists = database.albumArtists();
    QCOMPARE(artists.size(), 1);
    QCOMPARE(artists.first().name, QStringLiteral("Guessed Artist"));
    QCOMPARE(database.tracksForArtist(QStringLiteral("Guessed Artist")).size(), 1);

    // Back off: hidden again.
    database.setGuessedPlaceholdersVisible(false);
    QVERIFY(database.albumArtists().isEmpty());
}

void SchemaTest::searchCacheRoundTrips()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    Database database(QStringLiteral("schema-cache-rt-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    Track track = makeTrack(temp, QStringLiteral("01.flac"), 80);
    track.title = QString::fromUtf8("三線の花");
    track.titleSort = QStringLiteral("Sanshin no Hana");
    QVERIFY2(database.upsertTrack(track), qPrintable(database.lastError()));

    const QVector<Search::SearchRecord> records = database.allTracksForSearch();
    QCOMPARE(records.size(), 1);
    const Search::CacheSignature sig = Search::IndexCache::currentSignature(database);

    const QString path = temp.filePath(QStringLiteral("idx.cache"));
    QVERIFY(Search::IndexCache::write(path, sig, records));

    const Search::IndexCache::Loaded loaded = Search::IndexCache::read(path);
    QVERIFY(loaded.ok);
    QVERIFY(loaded.signature == sig);
    QCOMPARE(loaded.records.size(), 1);
    // Folded norms survive the round-trip (so the romaji match still works).
    QCOMPARE(loaded.records.first().path, records.first().path);
    QCOMPARE(loaded.records.first().title, records.first().title);
    QCOMPARE(loaded.records.first().normTitle, records.first().normTitle);
    QVERIFY(loaded.records.first().normTitle.contains(QStringLiteral("sanshin")));
}

void SchemaTest::searchCacheSignatureDetectsChange()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    Database database(QStringLiteral("schema-cache-sig-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    QVERIFY2(database.upsertTrack(makeTrack(temp, QStringLiteral("01.flac"), 80)), qPrintable(database.lastError()));
    const Search::CacheSignature before = Search::IndexCache::currentSignature(database);

    // Adding a track must change the signature (count moves), so a stale cache
    // is detected and rebuilt.
    QVERIFY2(database.upsertTrack(makeTrack(temp, QStringLiteral("02.flac"), 60)), qPrintable(database.lastError()));
    const Search::CacheSignature after = Search::IndexCache::currentSignature(database);
    QVERIFY(!(before == after));

    // A read whose stored signature no longer matches the current one is stale.
    const QString path = temp.filePath(QStringLiteral("idx.cache"));
    QVERIFY(Search::IndexCache::write(path, before, database.allTracksForSearch()));
    const Search::IndexCache::Loaded loaded = Search::IndexCache::read(path);
    QVERIFY(loaded.ok);
    QVERIFY(!(loaded.signature == after)); // detected as stale vs current
}

void SchemaTest::trackGenresPopulatedOnUpsert()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    Database database(QStringLiteral("schema-genre-upsert-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    Track track = makeTrack(temp, QStringLiteral("01.flac"), 80);
    {
        MetadataBlob::FullMetadata meta;
        meta.tags.insert(QStringLiteral("GENRE"), {
            QStringLiteral("Dream Pop; Shoegaze"),
            QStringLiteral("dream pop"),
            QStringLiteral("Rock/Post-Rock"),
        });
        const MetadataBlob::Encoded encoded = MetadataBlob::encode(meta);
        track.fullMetadataBlob = encoded.data;
        track.fullMetadataRawSize = encoded.rawSize;
    }
    QVERIFY2(database.upsertTrack(track), qPrintable(database.lastError()));

    QStringList genres = database.genresForTrack(track.path);
    QCOMPARE(genres.size(), 4);
    // "Dream Pop" (first-seen casing) wins over the later lowercase duplicate.
    QVERIFY(genres.contains(QStringLiteral("Dream Pop")));
    QVERIFY(!genres.contains(QStringLiteral("dream pop")));
    QVERIFY(genres.contains(QStringLiteral("Shoegaze")));
    QVERIFY(genres.contains(QStringLiteral("Rock")));
    QVERIFY(genres.contains(QStringLiteral("Post-Rock")));

    // Re-upserting the same path with a different GENRE tag must replace, not
    // merge with, the previous genre set (delete-then-insert).
    {
        MetadataBlob::FullMetadata meta;
        meta.tags.insert(QStringLiteral("GENRE"), {QStringLiteral("Ambient")});
        const MetadataBlob::Encoded encoded = MetadataBlob::encode(meta);
        track.fullMetadataBlob = encoded.data;
        track.fullMetadataRawSize = encoded.rawSize;
    }
    QVERIFY2(database.upsertTrack(track), qPrintable(database.lastError()));
    genres = database.genresForTrack(track.path);
    QCOMPARE(genres, QStringList{QStringLiteral("Ambient")});
}

void SchemaTest::trackGenresBackfillOnceFromBlobs()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString dbPath = temp.filePath(QStringLiteral("library.sqlite"));
    const QString connectionName = QStringLiteral("schema-genre-backfill-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));

    Track track = makeTrack(temp, QStringLiteral("01.flac"), 80);
    {
        MetadataBlob::FullMetadata meta;
        meta.tags.insert(QStringLiteral("GENRE"), {QStringLiteral("Ambient")});
        const MetadataBlob::Encoded encoded = MetadataBlob::encode(meta);
        track.fullMetadataBlob = encoded.data;
        track.fullMetadataRawSize = encoded.rawSize;
    }

    {
        Database database(connectionName);
        QVERIFY2(database.open(dbPath), qPrintable(database.lastError()));
        QVERIFY2(database.upsertTrack(track), qPrintable(database.lastError()));
        QCOMPARE(database.genresForTrack(track.path), QStringList{QStringLiteral("Ambient")});
    }

    // Simulate a pre-migration-10 database: wipe the genre rows and the
    // version-10 marker, then reopen. The backfill guard should fire once
    // and repopulate track_genres from the blob already stored in the DB.
    {
        const QString rawConnection = QStringLiteral("schema-genre-backfill-raw-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        {
            QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), rawConnection);
            raw.setDatabaseName(dbPath);
            QVERIFY(raw.open());
            QSqlQuery q(raw);
            QVERIFY(q.exec(QStringLiteral("DELETE FROM track_genres")));
            QVERIFY(q.exec(QStringLiteral("DELETE FROM schema_migrations WHERE version = 10")));
            raw.close();
        }
        QSqlDatabase::removeDatabase(rawConnection);
    }

    {
        Database database(connectionName);
        QVERIFY2(database.open(dbPath), qPrintable(database.lastError()));
        QCOMPARE(database.genresForTrack(track.path), QStringList{QStringLiteral("Ambient")});
    }

    // Now delete only the track_genres rows but leave the version-10 marker in
    // place: the guard must hold, so reopening must not repopulate.
    {
        const QString rawConnection = QStringLiteral("schema-genre-backfill-raw2-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        {
            QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), rawConnection);
            raw.setDatabaseName(dbPath);
            QVERIFY(raw.open());
            QSqlQuery q(raw);
            QVERIFY(q.exec(QStringLiteral("DELETE FROM track_genres")));
            raw.close();
        }
        QSqlDatabase::removeDatabase(rawConnection);
    }

    {
        Database database(connectionName);
        QVERIFY2(database.open(dbPath), qPrintable(database.lastError()));
        QVERIFY(database.genresForTrack(track.path).isEmpty());
    }
}

void SchemaTest::trackGenresCascadeOnTrackDelete()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString connectionName = QStringLiteral("schema-genre-cascade-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    Database database(connectionName);
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    Track track = makeTrack(temp, QStringLiteral("01.flac"), 80);
    {
        MetadataBlob::FullMetadata meta;
        meta.tags.insert(QStringLiteral("GENRE"), {QStringLiteral("Ambient")});
        const MetadataBlob::Encoded encoded = MetadataBlob::encode(meta);
        track.fullMetadataBlob = encoded.data;
        track.fullMetadataRawSize = encoded.rawSize;
    }
    QVERIFY2(database.upsertTrack(track), qPrintable(database.lastError()));
    QCOMPARE(database.genresForTrack(track.path), QStringList{QStringLiteral("Ambient")});

    QSqlQuery deleteTrack(QSqlDatabase::database(connectionName));
    deleteTrack.prepare(QStringLiteral("DELETE FROM tracks WHERE path = ?"));
    deleteTrack.addBindValue(track.path);
    QVERIFY(deleteTrack.exec());

    QVERIFY(database.genresForTrack(track.path).isEmpty());

    QSqlQuery countQuery(QSqlDatabase::database(connectionName));
    QVERIFY(countQuery.exec(QStringLiteral("SELECT COUNT(*) FROM track_genres")));
    QVERIFY(countQuery.next());
    QCOMPARE(countQuery.value(0).toInt(), 0);
}

void SchemaTest::genreAliasesRoundTripAndMigrationIsIdempotent()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString connectionName = QStringLiteral("schema-genre-alias-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    Database database(connectionName);
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    QHash<QString, QString> aliases = database.genreAliases();
    QCOMPARE(aliases.value(QStringLiteral("clássica")), QStringLiteral("classical"));
    QCOMPARE(aliases.value(QStringLiteral("classique")), QStringLiteral("classical"));

    QVERIFY2(database.setGenreAlias(QStringLiteral("  Électro  "), QStringLiteral(" Electronic ")),
             qPrintable(database.lastError()));
    aliases = database.genreAliases();
    QCOMPARE(aliases.value(QStringLiteral("électro")), QStringLiteral("electronic"));

    QVERIFY2(database.removeGenreAlias(QStringLiteral("Électro")), qPrintable(database.lastError()));
    aliases = database.genreAliases();
    QVERIFY(!aliases.contains(QStringLiteral("électro")));

    QVERIFY2(database.migrate(), qPrintable(database.lastError()));
    aliases = database.genreAliases();
    QCOMPARE(aliases.value(QStringLiteral("clássica")), QStringLiteral("classical"));
    QCOMPARE(aliases.value(QStringLiteral("classique")), QStringLiteral("classical"));
}

void SchemaTest::genreTagsSplitsAndFolds()
{
    MetadataBlob::FullMetadata meta;
    meta.tags.insert(QStringLiteral("GENRE"), {
        QStringLiteral("Dream Pop; Shoegaze"),
        QStringLiteral("dream pop"),
        QStringLiteral("Rock/Post-Rock,Ambient"),
        QStringLiteral("Noise") + QChar(u'\0') + QStringLiteral("Drone"),
        QStringLiteral("   "),
    });

    const QStringList genres = GenreTags::fromMetadata(meta);
    // 7 unique genres: the lowercase "dream pop" duplicate and the
    // all-whitespace value are both dropped.
    QCOMPARE(genres.size(), 7);
    QVERIFY(genres.contains(QStringLiteral("Dream Pop")));
    QVERIFY(genres.contains(QStringLiteral("Shoegaze")));
    QVERIFY(genres.contains(QStringLiteral("Rock")));
    QVERIFY(genres.contains(QStringLiteral("Post-Rock")));
    QVERIFY(genres.contains(QStringLiteral("Ambient")));
    QVERIFY(genres.contains(QStringLiteral("Noise")));
    QVERIFY(genres.contains(QStringLiteral("Drone")));

    QCOMPARE(GenreTags::folded(QStringLiteral("  Dream   Pop ")), QStringLiteral("dream pop"));
    QVERIFY(GenreTags::folded(QStringLiteral("   ")).isEmpty());

    QVERIFY(GenreTags::fromMetadata(MetadataBlob::FullMetadata{}).isEmpty());
}

QTEST_MAIN(SchemaTest)
#include "test_schema.moc"
