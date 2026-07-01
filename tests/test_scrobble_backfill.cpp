#include "scrobble/BackfillParse.h"
#include "scrobble/ListenHistoryStore.h"
#include "scrobble/ScrobbleBackfill.h"

#include <QTemporaryDir>
#include <QtTest>

using ImportedListen = ListenHistoryStore::ImportedListen;
using PlaycountBaseline = ListenHistoryStore::PlaycountBaseline;
using LibraryIndex = ScrobbleBackfill::LibraryIndex;

class TestScrobbleBackfill final : public QObject {
    Q_OBJECT

private slots:
    // BackfillParse
    void lbPageWithMbidMapping();
    void lbPageMbidAbsentOrNull();
    void lbPageEmpty();
    void lbPageMalformed();
    void lbPageDropsIncompleteRows();
    void tokenValidationBothShapes();
    void lastFmTrackArray();
    void lastFmSingleObjectTrack();
    void lastFmStringNumbers();
    void lastFmErrorShape();

    // Store
    void importedRoundTripAndDedupe();
    void importedCrossDedupSkipsOwnScrobbles();
    void playcountBaselineUpsert();
    void metaCursorRoundTrip();

    // Matching
    void matchMbidBeatsArtistTitle();
    void matchFoldedIsCaseAndWhitespaceInsensitive();
    void matchUnmatchedStaysEmpty();
};

// --- BackfillParse: ListenBrainz ----------------------------------------

void TestScrobbleBackfill::lbPageWithMbidMapping()
{
    const QByteArray json = R"({"payload":{"count":2,"listens":[
        {"listened_at":2000,"track_metadata":{"artist_name":"A","track_name":"T1",
            "release_name":"Rel","mbid_mapping":{"recording_mbid":"mbid-1"}}},
        {"listened_at":1000,"track_metadata":{"artist_name":"B","track_name":"T2",
            "release_name":"Rel2","mbid_mapping":{"recording_mbid":"mbid-2"}}}
    ]}})";
    const BackfillParse::ListenBrainzPage page = BackfillParse::parseListenBrainzPage(json);
    QVERIFY(page.ok);
    QCOMPARE(page.listens.size(), 2);
    QCOMPARE(page.listens.at(0).artist, QStringLiteral("A"));
    QCOMPARE(page.listens.at(0).title, QStringLiteral("T1"));
    QCOMPARE(page.listens.at(0).album, QStringLiteral("Rel"));
    QCOMPARE(page.listens.at(0).mbRecordingId, QStringLiteral("mbid-1"));
    // Oldest listened_at across the page is the next max_ts cursor.
    QCOMPARE(page.oldestListenedAt, static_cast<qint64>(1000));
}

void TestScrobbleBackfill::lbPageMbidAbsentOrNull()
{
    const QByteArray json = R"({"payload":{"listens":[
        {"listened_at":500,"track_metadata":{"artist_name":"A","track_name":"T"}},
        {"listened_at":400,"track_metadata":{"artist_name":"B","track_name":"U",
            "release_name":null,"mbid_mapping":null}}
    ]}})";
    const BackfillParse::ListenBrainzPage page = BackfillParse::parseListenBrainzPage(json);
    QVERIFY(page.ok);
    QCOMPARE(page.listens.size(), 2);
    QVERIFY(page.listens.at(0).mbRecordingId.isEmpty());
    QVERIFY(page.listens.at(0).album.isEmpty());
    QVERIFY(page.listens.at(1).mbRecordingId.isEmpty());
    QVERIFY(page.listens.at(1).album.isEmpty());
}

void TestScrobbleBackfill::lbPageEmpty()
{
    const BackfillParse::ListenBrainzPage page =
        BackfillParse::parseListenBrainzPage(R"({"payload":{"count":0,"listens":[]}})");
    QVERIFY(page.ok);   // well-formed empty page: end-of-history signal
    QVERIFY(page.listens.isEmpty());
    QCOMPARE(page.oldestListenedAt, static_cast<qint64>(0));
}

void TestScrobbleBackfill::lbPageMalformed()
{
    QVERIFY(!BackfillParse::parseListenBrainzPage("not json").ok);
    QVERIFY(!BackfillParse::parseListenBrainzPage("{}").ok);
    QVERIFY(!BackfillParse::parseListenBrainzPage("[]").ok);
}

void TestScrobbleBackfill::lbPageDropsIncompleteRows()
{
    const QByteArray json = R"({"payload":{"listens":[
        {"listened_at":900,"track_metadata":{"artist_name":"","track_name":"T"}},
        {"listened_at":800,"track_metadata":{"artist_name":"A","track_name":""}},
        {"listened_at":0,"track_metadata":{"artist_name":"A","track_name":"T"}},
        {"listened_at":700,"track_metadata":{"artist_name":"Good","track_name":"Row"}}
    ]}})";
    const BackfillParse::ListenBrainzPage page = BackfillParse::parseListenBrainzPage(json);
    QVERIFY(page.ok);
    QCOMPARE(page.listens.size(), 1);
    QCOMPARE(page.listens.at(0).artist, QStringLiteral("Good"));
    QCOMPARE(page.oldestListenedAt, static_cast<qint64>(700));
}

void TestScrobbleBackfill::tokenValidationBothShapes()
{
    const BackfillParse::TokenValidation valid =
        BackfillParse::parseTokenValidation(R"({"valid":true,"user_name":"alice"})");
    QVERIFY(valid.valid);
    QCOMPARE(valid.username, QStringLiteral("alice"));

    const BackfillParse::TokenValidation invalid =
        BackfillParse::parseTokenValidation(R"({"valid":false,"message":"Token invalid."})");
    QVERIFY(!invalid.valid);
    QVERIFY(invalid.username.isEmpty());
}

// --- BackfillParse: Last.fm ---------------------------------------------

void TestScrobbleBackfill::lastFmTrackArray()
{
    const QByteArray json = R"({"toptracks":{"track":[
        {"name":"T1","playcount":"10","mbid":"m1","artist":{"name":"A"}},
        {"name":"T2","playcount":"5","mbid":"","artist":{"name":"B"}}
    ],"@attr":{"page":"1","totalPages":"3"}}})";
    const BackfillParse::LastFmTopTracksPage page = BackfillParse::parseLastFmTopTracks(json);
    QVERIFY(page.ok);
    QCOMPARE(page.errorCode, 0);
    QCOMPARE(page.page, 1);
    QCOMPARE(page.totalPages, 3);
    QCOMPARE(page.tracks.size(), 2);
    QCOMPARE(page.tracks.at(0).title, QStringLiteral("T1"));
    QCOMPARE(page.tracks.at(0).count, static_cast<qint64>(10));
    QCOMPARE(page.tracks.at(0).artist, QStringLiteral("A"));
    QCOMPARE(page.tracks.at(0).mbRecordingId, QStringLiteral("m1"));
}

void TestScrobbleBackfill::lastFmSingleObjectTrack()
{
    const QByteArray json = R"({"toptracks":{"track":
        {"name":"Solo","playcount":"42","mbid":"m","artist":{"name":"X"}},
        "@attr":{"page":"1","totalPages":"1"}}})";
    const BackfillParse::LastFmTopTracksPage page = BackfillParse::parseLastFmTopTracks(json);
    QVERIFY(page.ok);
    QCOMPARE(page.tracks.size(), 1);
    QCOMPARE(page.tracks.at(0).title, QStringLiteral("Solo"));
    QCOMPARE(page.tracks.at(0).count, static_cast<qint64>(42));
}

void TestScrobbleBackfill::lastFmStringNumbers()
{
    // Numbers arrive as JSON strings; page/totalPages/playcount must parse.
    const QByteArray json = R"({"toptracks":{"track":[
        {"name":"T","playcount":"999","artist":{"name":"A"}}
    ],"@attr":{"page":"2","totalPages":"7"}}})";
    const BackfillParse::LastFmTopTracksPage page = BackfillParse::parseLastFmTopTracks(json);
    QVERIFY(page.ok);
    QCOMPARE(page.page, 2);
    QCOMPARE(page.totalPages, 7);
    QCOMPARE(page.tracks.at(0).count, static_cast<qint64>(999));
}

void TestScrobbleBackfill::lastFmErrorShape()
{
    const BackfillParse::LastFmTopTracksPage page =
        BackfillParse::parseLastFmTopTracks(R"({"error":6,"message":"User not found"})");
    QVERIFY(!page.ok);
    QCOMPARE(page.errorCode, 6);
    QCOMPARE(page.errorMessage, QStringLiteral("User not found"));
}

// --- Store --------------------------------------------------------------

namespace {
ImportedListen makeImported(qint64 ts, const QString &artist, const QString &title)
{
    ImportedListen row;
    row.source = ListenHistoryStore::ListenBrainz;
    row.listenedAtSecs = ts;
    row.artist = artist;
    row.title = title;
    return row;
}
} // namespace

void TestScrobbleBackfill::importedRoundTripAndDedupe()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));
    QVERIFY(store.isOpen());

    QList<ImportedListen> rows{
        makeImported(1000, QStringLiteral("A"), QStringLiteral("T1")),
        makeImported(2000, QStringLiteral("B"), QStringLiteral("T2")),
    };
    QCOMPARE(store.recordImportedListens(rows), 2);
    QCOMPARE(store.importedListenCount(ListenHistoryStore::ListenBrainz), 2);

    // Re-inserting the same rows plus a new one: UNIQUE ignores duplicates.
    rows.push_back(makeImported(3000, QStringLiteral("C"), QStringLiteral("T3")));
    QCOMPARE(store.recordImportedListens(rows), 1);
    QCOMPARE(store.importedListenCount(ListenHistoryStore::ListenBrainz), 3);
}

void TestScrobbleBackfill::importedCrossDedupSkipsOwnScrobbles()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));
    QVERIFY(store.isOpen());

    // The user's own scrobble that muzaiten submitted lives in `listens`.
    Track track;
    track.path = QStringLiteral("/music/mine.flac");
    track.title = QStringLiteral("Mine");
    track.artistName = QStringLiteral("Me");
    QVERIFY(store.recordListen(track, 5000, true, true) > 0);

    QList<ImportedListen> rows{
        makeImported(5000, QStringLiteral("Me"), QStringLiteral("Mine")),   // echo of own scrobble
        makeImported(6000, QStringLiteral("Other"), QStringLiteral("Song")), // genuinely new
    };
    QCOMPARE(store.recordImportedListens(rows), 1);
    QCOMPARE(store.importedListenCount(ListenHistoryStore::ListenBrainz), 1);
    QCOMPARE(store.playcountBaselines(ListenHistoryStore::ListenBrainz).size(), 0);
}

void TestScrobbleBackfill::playcountBaselineUpsert()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));
    QVERIFY(store.isOpen());

    PlaycountBaseline row;
    row.source = ListenHistoryStore::LastFm;
    row.artist = QStringLiteral("A");
    row.title = QStringLiteral("T");
    row.count = 10;
    row.syncedAtSecs = 100;
    QVERIFY(store.upsertPlaycountBaseline(row));

    QList<PlaycountBaseline> first = store.playcountBaselines(ListenHistoryStore::LastFm);
    QCOMPARE(first.size(), 1);
    QCOMPARE(first.at(0).count, static_cast<qint64>(10));

    // Same (source, artist, title): count and synced_at are overwritten.
    row.count = 25;
    row.syncedAtSecs = 200;
    row.matchedTrackPath = QStringLiteral("/music/a.flac");
    QVERIFY(store.upsertPlaycountBaseline(row));

    QList<PlaycountBaseline> second = store.playcountBaselines(ListenHistoryStore::LastFm);
    QCOMPARE(second.size(), 1);
    QCOMPARE(second.at(0).count, static_cast<qint64>(25));
    QCOMPARE(second.at(0).syncedAtSecs, static_cast<qint64>(200));
    QCOMPARE(second.at(0).matchedTrackPath, QStringLiteral("/music/a.flac"));
}

void TestScrobbleBackfill::metaCursorRoundTrip()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));
    QVERIFY(store.isOpen());

    QVERIFY(store.metaValue(QStringLiteral("backfill.listenbrainz.oldest_ts")).isEmpty());
    store.setMetaValue(QStringLiteral("backfill.listenbrainz.oldest_ts"), QStringLiteral("12345"));
    QCOMPARE(store.metaValue(QStringLiteral("backfill.listenbrainz.oldest_ts")), QStringLiteral("12345"));
    store.setMetaValue(QStringLiteral("backfill.listenbrainz.oldest_ts"), QStringLiteral("999"));
    QCOMPARE(store.metaValue(QStringLiteral("backfill.listenbrainz.oldest_ts")), QStringLiteral("999"));
}

// --- Matching -----------------------------------------------------------

void TestScrobbleBackfill::matchMbidBeatsArtistTitle()
{
    LibraryIndex index;
    index.byRecordingMbid.insert(QStringLiteral("rec-1"), QStringLiteral("/by/mbid.flac"));
    index.byArtistTitle.insert(ScrobbleBackfill::foldedArtistTitleKey(QStringLiteral("A"), QStringLiteral("T")),
                               QStringLiteral("/by/name.flac"));
    // MBID hit wins even though the artist+title would also match a different row.
    QCOMPARE(ScrobbleBackfill::matchTrackPath(index, QStringLiteral("rec-1"), QStringLiteral("A"), QStringLiteral("T")),
             QStringLiteral("/by/mbid.flac"));
    // No MBID hit falls back to artist+title.
    QCOMPARE(ScrobbleBackfill::matchTrackPath(index, QStringLiteral("nope"), QStringLiteral("A"), QStringLiteral("T")),
             QStringLiteral("/by/name.flac"));
}

void TestScrobbleBackfill::matchFoldedIsCaseAndWhitespaceInsensitive()
{
    LibraryIndex index;
    index.byArtistTitle.insert(ScrobbleBackfill::foldedArtistTitleKey(QStringLiteral("The Beatles"), QStringLiteral("Let It Be")),
                               QStringLiteral("/beatles.flac"));
    QCOMPARE(ScrobbleBackfill::matchTrackPath(index, QString(),
                                              QStringLiteral("  the   BEATLES "), QStringLiteral("let it be")),
             QStringLiteral("/beatles.flac"));
}

void TestScrobbleBackfill::matchUnmatchedStaysEmpty()
{
    LibraryIndex index;
    index.byArtistTitle.insert(ScrobbleBackfill::foldedArtistTitleKey(QStringLiteral("A"), QStringLiteral("T")),
                               QStringLiteral("/a.flac"));
    QVERIFY(ScrobbleBackfill::matchTrackPath(index, QStringLiteral("x"),
                                             QStringLiteral("Unknown"), QStringLiteral("Nope")).isEmpty());
}

QTEST_MAIN(TestScrobbleBackfill)
#include "test_scrobble_backfill.moc"
