#include <QTest>
#include <QString>
#include <QVector>

#include "search/SearchIndex.h"
#include "search/SearchQuery.h"
#include "search/SearchRecord.h"

using namespace Search;

static SearchRecord makeRecord(const QString &title,
                                const QString &artist,
                                const QString &album,
                                const QString &path = {},
                                const QString &date = {},
                                const QString &codec = {},
                                int sampleRateHz = 0,
                                int bitrateKbps = 0,
                                int channels = 0,
                                int rating = -1)
{
    SearchRecord r;
    r.title           = title;
    r.artistName      = artist;
    r.albumArtistName = artist;
    r.albumTitle      = album;
    r.date            = date;
    r.filename        = path.section(QLatin1Char('/'), -1);
    r.path            = path;
    r.codec           = codec;
    r.sampleRateHz    = sampleRateHz;
    r.bitrateKbps     = bitrateKbps;
    r.channels        = channels;
    r.rating0To100    = rating;
    r.normTitle        = title.toLower();
    r.normArtist       = artist.toLower();
    r.normAlbumArtist  = artist.toLower();
    r.normAlbum        = album.toLower();
    r.normFilename     = r.filename.toLower();
    r.normPath         = path.toLower();
    return r;
}

class TestSearchIndex : public QObject {
    Q_OBJECT

private slots:
    void emptyQuery_returnsNothing()
    {
        SearchIndex idx;
        idx.build({makeRecord(QStringLiteral("So What"), QStringLiteral("Miles Davis"),
                               QStringLiteral("Kind of Blue"))});
        const SearchQuery q = SearchQuery::parse(QStringLiteral(""));
        const auto results = idx.match(q, false);
        QVERIFY(results.isEmpty());
    }

    void singleTermMatchesTitle()
    {
        SearchIndex idx;
        idx.build({makeRecord(QStringLiteral("So What"), QStringLiteral("Miles Davis"),
                               QStringLiteral("Kind of Blue"))});
        const auto results = idx.match(SearchQuery::parse(QStringLiteral("what")), false);
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].rec.title, QStringLiteral("So What"));
    }

    void orderlessAnd_bothTermsMustMatch()
    {
        SearchIndex idx;
        idx.build({
            makeRecord(QStringLiteral("So What"), QStringLiteral("Miles Davis"),
                       QStringLiteral("Kind of Blue")),
            makeRecord(QStringLiteral("Autumn Leaves"), QStringLiteral("Miles Davis"),
                       QStringLiteral("Workin'")),
        });
        // "blue miles" — both must match somewhere
        const auto results = idx.match(SearchQuery::parse(QStringLiteral("blue miles")), false);
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].rec.title, QStringLiteral("So What"));
    }

    void noMatchIfOnlyPartialAndMatch()
    {
        SearchIndex idx;
        idx.build({makeRecord(QStringLiteral("So What"), QStringLiteral("Miles Davis"),
                               QStringLiteral("Kind of Blue"))});
        const auto results = idx.match(SearchQuery::parse(QStringLiteral("blue bowie")), false);
        QVERIFY(results.isEmpty());
    }

    void smartCase_lowercaseIsCI()
    {
        SearchIndex idx;
        idx.build({makeRecord(QStringLiteral("Kind of Blue"), QStringLiteral("Miles Davis"),
                               QStringLiteral("Kind of Blue"))});
        // lowercase query → case-insensitive
        const auto results = idx.match(SearchQuery::parse(QStringLiteral("kind of blue")), false);
        QCOMPARE(results.size(), 1);
    }

    void negatedTerm()
    {
        SearchIndex idx;
        idx.build({
            makeRecord(QStringLiteral("So What"), QStringLiteral("Miles Davis"),
                       QStringLiteral("Kind of Blue")),
            makeRecord(QStringLiteral("Autumn Leaves"), QStringLiteral("Miles Davis"),
                       QStringLiteral("Workin'")),
        });
        // All miles tracks except those matching "blue"
        const auto results = idx.match(SearchQuery::parse(QStringLiteral("miles !blue")), false);
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].rec.title, QStringLiteral("Autumn Leaves"));
    }

    void fieldToken_artist()
    {
        SearchIndex idx;
        idx.build({
            makeRecord(QStringLiteral("So What"), QStringLiteral("Miles Davis"),
                       QStringLiteral("Kind of Blue")),
            makeRecord(QStringLiteral("Hello"),   QStringLiteral("Adele"),
                       QStringLiteral("21")),
        });
        const auto results = idx.match(SearchQuery::parse(QStringLiteral("artist:davis")), false);
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].rec.artistName, QStringLiteral("Miles Davis"));
    }

    void fieldToken_ext()
    {
        SearchIndex idx;
        idx.build({
            makeRecord(QStringLiteral("Track 1"), QStringLiteral("Artist"), QStringLiteral("Album"),
                       QStringLiteral("/music/a.flac"), {}, QStringLiteral("flac")),
            makeRecord(QStringLiteral("Track 2"), QStringLiteral("Artist"), QStringLiteral("Album"),
                       QStringLiteral("/music/b.mp3"), {}, QStringLiteral("mp3")),
        });
        const auto results = idx.match(SearchQuery::parse(QStringLiteral("ext:flac")), false);
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].rec.codec, QStringLiteral("flac"));
    }

    void fieldToken_khz_ge()
    {
        SearchIndex idx;
        idx.build({
            makeRecord(QStringLiteral("HiRes"), QStringLiteral("A"), QStringLiteral("B"),
                       {}, {}, {}, 96000, 0),
            makeRecord(QStringLiteral("RedBook"), QStringLiteral("A"), QStringLiteral("B"),
                       {}, {}, {}, 44100, 0),
        });
        const auto results = idx.match(SearchQuery::parse(QStringLiteral("khz:>=96")), false);
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].rec.title, QStringLiteral("HiRes"));
    }

    void fieldToken_rating_ge()
    {
        SearchIndex idx;
        idx.build({
            makeRecord(QStringLiteral("Loved"), QStringLiteral("A"), QStringLiteral("B"),
                       {}, {}, {}, 0, 0, 0, 90),
            makeRecord(QStringLiteral("Meh"),   QStringLiteral("A"), QStringLiteral("B"),
                       {}, {}, {}, 0, 0, 0, 50),
        });
        const auto results = idx.match(SearchQuery::parse(QStringLiteral("rating:>=80")), false);
        QCOMPARE(results.size(), 1);
        QCOMPARE(results[0].rec.title, QStringLiteral("Loved"));
    }

    void rankingByScore()
    {
        // A title match should rank higher than a path-only match.
        SearchIndex idx;
        idx.build({
            makeRecord(QStringLiteral("Some Random Song"), QStringLiteral("Artist"),
                       QStringLiteral("Album"), QStringLiteral("/music/miles/some_random_song.flac")),
            makeRecord(QStringLiteral("Miles Davis Special"), QStringLiteral("Artist"),
                       QStringLiteral("Album"), QStringLiteral("/other/other.flac")),
        });
        const auto results = idx.match(SearchQuery::parse(QStringLiteral("miles")), false);
        // Both should match, title match should come first
        QCOMPARE(results.size(), 2);
        QCOMPARE(results[0].rec.title, QStringLiteral("Miles Davis Special"));
    }

    void fuzzyMode_matchesSubsequence()
    {
        SearchIndex idx;
        idx.build({makeRecord(QStringLiteral("So What"), QStringLiteral("Miles Davis"),
                               QStringLiteral("Kind of Blue"))});
        // "sw" should match "So What" in fuzzy mode (s from "So", w from "What")
        const auto results = idx.match(SearchQuery::parse(QStringLiteral("sw")), true);
        QCOMPARE(results.size(), 1);
    }

    void matchRangesArePopulated()
    {
        SearchIndex idx;
        idx.build({makeRecord(QStringLiteral("So What"), QStringLiteral("Miles Davis"),
                               QStringLiteral("Kind of Blue"))});
        const auto results = idx.match(SearchQuery::parse(QStringLiteral("what")), false);
        QCOMPARE(results.size(), 1);
        // "what" matches title "So What" at positions 3-6
        QVERIFY(!results[0].ranges.titlePositions.isEmpty());
    }
};

QTEST_MAIN(TestSearchIndex)
#include "test_search_index.moc"
