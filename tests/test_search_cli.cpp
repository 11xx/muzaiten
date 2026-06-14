#include "cli/SearchCli.h"
#include "core/Rating.h"
#include "core/Track.h"
#include "db/Database.h"
#include "search/SearchIndex.h"
#include "search/SearchQuery.h"

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

namespace {

Track makeTrack(const QString &dir, const QString &filename, const QString &title, int rating)
{
    Track track;
    track.path = dir + QStringLiteral("/Artist/Album/") + filename;
    track.parentDir = dir + QStringLiteral("/Artist/Album");
    track.filename = filename;
    track.title = title;
    track.artistName = QStringLiteral("BEGIN");
    track.albumArtistName = QStringLiteral("BEGIN");
    track.albumTitle = QStringLiteral("Best");
    track.trackNumber = filename.left(2).toInt();
    track.rating0To100 = rating;
    track.ratingSource = Rating::Source::VorbisRating;
    track.fileSize = 10;
    track.fileMtime = 20;
    return track;
}

} // namespace

// End-to-end exercise of the standalone CLI search index path: build-from-DB on
// a cache miss, reuse on a hit, detect staleness after a change, and rebuild on
// --refresh.
class SearchCliTest : public QObject {
    Q_OBJECT

    QTemporaryDir m_temp;

private slots:
    void initTestCase()
    {
        QVERIFY(m_temp.isValid());
        // Isolate AppPaths to the temp dir so we never touch real data/cache.
        qputenv("MUZAITEN_DATA_DIR", (m_temp.path() + QStringLiteral("/data")).toUtf8());
        qputenv("MUZAITEN_CACHE_DIR", (m_temp.path() + QStringLiteral("/cache")).toUtf8());
    }

    void buildsReusesAndRefreshes()
    {
        const QString dbPath = SearchCli::libraryDbPath();
        {
            Database db(QStringLiteral("cli-setup-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
            QVERIFY2(db.open(dbPath), qPrintable(db.lastError()));
            Track t = makeTrack(m_temp.path(), QStringLiteral("01.flac"), QString::fromUtf8("三線の花"), 80);
            t.titleSort = QStringLiteral("Sanshin no Hana");
            QVERIFY2(db.upsertTrack(t), qPrintable(db.lastError()));
            QVERIFY2(db.upsertTrack(makeTrack(m_temp.path(), QStringLiteral("02.flac"), QStringLiteral("Hello"), 60)),
                     qPrintable(db.lastError()));
        }

        QVERIFY(!QFile::exists(SearchCli::cachePath())); // nothing cached yet

        // Cold: builds from the DB, writes the cache, romaji query matches kanji.
        Search::SearchIndex idx;
        const SearchCli::LoadResult cold = SearchCli::loadIndex(idx, /*forceRefresh=*/false);
        QVERIFY2(cold.ok, qPrintable(cold.error));
        QVERIFY(cold.rebuilt);
        QVERIFY(!cold.usedCache);
        QCOMPARE(idx.size(), 2);
        QVERIFY(QFile::exists(SearchCli::cachePath()));
        int total = 0;
        QCOMPARE(idx.match(Search::SearchQuery::parse(QStringLiteral("sanshin")), false, {}, &total).size(), 1);

        // Warm: loads from the cache, not stale.
        Search::SearchIndex idx2;
        const SearchCli::LoadResult warm = SearchCli::loadIndex(idx2, false);
        QVERIFY(warm.ok);
        QVERIFY(warm.usedCache);
        QVERIFY(!warm.wasStale);
        QVERIFY(!warm.rebuilt);
        QCOMPARE(idx2.size(), 2);

        // Add a track → cache is stale but still used (shows the old 2).
        {
            Database db(QStringLiteral("cli-add-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
            QVERIFY2(db.open(dbPath), qPrintable(db.lastError()));
            QVERIFY2(db.upsertTrack(makeTrack(m_temp.path(), QStringLiteral("03.flac"), QStringLiteral("Third"), 40)),
                     qPrintable(db.lastError()));
        }
        Search::SearchIndex idx3;
        const SearchCli::LoadResult stale = SearchCli::loadIndex(idx3, false);
        QVERIFY(stale.usedCache);
        QVERIFY(stale.wasStale);
        QCOMPARE(idx3.size(), 2);

        // --refresh rebuilds from the DB and clears staleness.
        Search::SearchIndex idx4;
        const SearchCli::LoadResult refreshed = SearchCli::loadIndex(idx4, /*forceRefresh=*/true);
        QVERIFY(refreshed.rebuilt);
        QVERIFY(!refreshed.wasStale);
        QCOMPARE(idx4.size(), 3);

        // After the refresh write, a plain load is fresh again.
        Search::SearchIndex idx5;
        const SearchCli::LoadResult afterRefresh = SearchCli::loadIndex(idx5, false);
        QVERIFY(afterRefresh.usedCache);
        QVERIFY(!afterRefresh.wasStale);
        QCOMPARE(idx5.size(), 3);
    }

    void clearCacheRemovesFile()
    {
        // The previous test left a cache in place.
        QVERIFY(QFile::exists(SearchCli::cachePath()));
        QString path;
        QVERIFY(SearchCli::clearCache(&path));
        QCOMPARE(path, SearchCli::cachePath());
        QVERIFY(!QFile::exists(SearchCli::cachePath()));
        QVERIFY(!SearchCli::clearCache(nullptr)); // already gone
    }
};

QTEST_MAIN(SearchCliTest)
#include "test_search_cli.moc"
