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
};

void SchemaTest::migratesFreshDatabase()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    Database database(QStringLiteral("schema-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));
}

void SchemaTest::upsertsTrackAndQueriesArtist()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    Database database(QStringLiteral("schema-track-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(database.open(temp.filePath(QStringLiteral("library.sqlite"))), qPrintable(database.lastError()));

    Track track;
    track.path = temp.filePath(QStringLiteral("Artist/Album/01.flac"));
    track.parentDir = temp.filePath(QStringLiteral("Artist/Album"));
    track.filename = QStringLiteral("01.flac");
    track.title = QStringLiteral("Track");
    track.artistName = QStringLiteral("Artist");
    track.albumArtistName = QStringLiteral("Album Artist");
    track.albumTitle = QStringLiteral("Album");
    track.trackNumber = 1;
    track.rating0To100 = 80;
    track.ratingSource = Rating::Source::VorbisRating;
    track.fileSize = 10;
    track.fileMtime = 20;

    QVERIFY2(database.upsertTrack(track), qPrintable(database.lastError()));
    const QVector<Artist> artists = database.albumArtists();
    QCOMPARE(artists.size(), 1);
    QCOMPARE(artists.first().name, QStringLiteral("Album Artist"));
    QCOMPARE(artists.first().albumCount, 1);

    const QVector<Track> tracks = database.tracksForArtist(QStringLiteral("Album Artist"));
    QCOMPARE(tracks.size(), 1);
    QCOMPARE(tracks.first().rating0To100, 80);
}

QTEST_MAIN(SchemaTest)
#include "test_schema.moc"

