#include <QTest>
#include <QSet>
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

    void refactorStillScoreSortsInsteadOfDisplayOrder()
    {
        SearchIndex idx;
        idx.build({
            makeRecord(QStringLiteral("Path Only"), QStringLiteral("Artist"),
                       QStringLiteral("Album"), QStringLiteral("/music/blue/path_only.flac")),
            makeRecord(QStringLiteral("Blue Title"), QStringLiteral("Artist"),
                       QStringLiteral("Album"), QStringLiteral("/music/zzz/blue_title.flac")),
        });
        const auto results = idx.match(SearchQuery::parse(QStringLiteral("blue")), false);
        QCOMPARE(results.size(), 2);
        QCOMPARE(results[0].rec.title, QStringLiteral("Blue Title"));
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

    void highlightPositionsPopulated()
    {
        // "what" matches title "So What" at positions 3-6
        const QVector<int> pos = Search::highlightPositions(
            QStringLiteral("So What"),
            SearchQuery::parse(QStringLiteral("what")),
            Search::HighlightField::Title, false);
        QVERIFY(!pos.isEmpty());
        QVERIFY(pos.contains(3));
        QVERIFY(pos.contains(6));
    }

    void multipleTermsHighlightAllMatchesPerField()
    {
        // Regression: every matching term must contribute its highlight ranges
        // to a field, not just the last one.
        const QVector<int> pos = Search::highlightPositions(
            QStringLiteral("The Nation Of One"),
            SearchQuery::parse(QStringLiteral("the nation of one")),
            Search::HighlightField::Title, false);
        // "The Nation Of One" has 14 non-space characters; all should be highlighted.
        QSet<int> unique(pos.begin(), pos.end());
        QVERIFY(unique.size() >= 14);
    }

    void backgroundUpgradeEnablesRomajiMatching()
    {
        // A kana/kanji title with a kana sort reading: under the basic fold both
        // pass through, so a romaji query misses.
        SearchRecord r = makeRecord(QString::fromUtf8("三線の花"), QStringLiteral("BEGIN"), QStringLiteral("Best"));
        r.titleSort = QString::fromUtf8("サンシンノハナ");
        foldRecordNorms(r, /*extended=*/false);

        SearchIndex idx;
        idx.build({r});
        QCOMPARE(idx.match(SearchQuery::parse(QStringLiteral("sanshin")), false).size(), 0);

        // The background upgrade re-folds to the extended tier (romaji), so the
        // same query now matches — and the raw sort name is freed.
        QHash<QString, QString> pool;
        idx.upgradeFold(0, idx.size(), pool);
        QCOMPARE(idx.match(SearchQuery::parse(QStringLiteral("sanshin")), false).size(), 1);
    }

    void highlightMapsFoldedPositionsToDisplayChars()
    {
        // Accented title, plain query — highlight the original (accented) chars.
        const QVector<int> cafe = Search::highlightPositions(
            QString::fromUtf8("Café"), SearchQuery::parse(QStringLiteral("cafe")),
            Search::HighlightField::Title, false);
        QCOMPARE(cafe, (QVector<int>{0, 1, 2, 3}));

        // Expansion (ß→ss): both folded chars map back to the single source char.
        const QVector<int> strasse = Search::highlightPositions(
            QString::fromUtf8("Straße"), SearchQuery::parse(QStringLiteral("strasse")),
            Search::HighlightField::Title, false);
        QCOMPARE(strasse, (QVector<int>{0, 1, 2, 3, 4, 5}));

        // Romaji query highlights the underlying kana it romanizes.
        const QVector<int> kana = Search::highlightPositions(
            QString::fromUtf8("さんしん"), SearchQuery::parse(QStringLiteral("shin")),
            Search::HighlightField::Title, false);
        QVERIFY(kana.contains(2)); // し
        QVERIFY(kana.contains(3)); // ん

        // Dictionary word: a romaji query bolds the underlying kanji, spread
        // across the surface (三 and 線 both lit for "sanshin").
        const QVector<int> kanji = Search::highlightPositions(
            QString::fromUtf8("三線"), SearchQuery::parse(QStringLiteral("sanshin")),
            Search::HighlightField::Title, false);
        QCOMPARE(kanji, (QVector<int>{0, 1}));
    }

    void largeIndex_parallelScanCoversEveryRowOnce()
    {
        // Large indexes are scanned by several threads over contiguous slices.
        // Plant known matches straddling chunk boundaries (multiples of the
        // per-thread chunk) and at the first/last rows, then confirm each is
        // found exactly once — no record skipped or double-scored at a seam.
        const int N = 20000;
        QVector<SearchRecord> recs;
        recs.reserve(N);
        for (int i = 0; i < N; ++i) {
            // Filler that never matches "zylophone"; carries its row in `date`.
            recs.push_back(makeRecord(QStringLiteral("Filler Song"), QStringLiteral("Nobody"),
                                      QStringLiteral("Misc"), {}, QString::number(i)));
        }
        QList<int> hitRows = {0, 1, N - 2, N - 1};
        for (int b = 2500; b < N; b += 2500) {
            hitRows << (b - 1) << b << (b + 1); // straddle each likely seam
        }
        for (int row : hitRows) {
            recs[row] = makeRecord(QStringLiteral("Zylophone Dreams"), QStringLiteral("Nobody"),
                                   QStringLiteral("Misc"), {}, QString::number(row));
        }

        SearchIndex idx;
        idx.build(recs);

        for (bool fuzzy : {false, true}) {
            int total = -1;
            const auto res = idx.match(SearchQuery::parse(QStringLiteral("zylophone")), fuzzy, {}, &total);
            QCOMPARE(total, static_cast<int>(hitRows.size()));
            QCOMPARE(res.size(), static_cast<int>(hitRows.size()));

            // Exactly the planted rows, each once (set equality catches any
            // boundary skip or duplicate).
            QSet<int> found;
            for (const auto &r : res) {
                found.insert(r.rec.date.toInt());
            }
            QCOMPARE(found, QSet<int>(hitRows.begin(), hitRows.end()));

            // Equal-score ties resolve by ascending original row, and the merge
            // must preserve that across slices.
            for (int k = 1; k < res.size(); ++k) {
                QVERIFY(res[k].rec.date.toInt() > res[k - 1].rec.date.toInt());
            }
        }
    }

    void totalMatchesReported()
    {
        SearchIndex idx;
        idx.build({
            makeRecord(QStringLiteral("Blue One"),   QStringLiteral("A"), QStringLiteral("X")),
            makeRecord(QStringLiteral("Blue Two"),   QStringLiteral("A"), QStringLiteral("X")),
            makeRecord(QStringLiteral("Green Three"),QStringLiteral("A"), QStringLiteral("X")),
        });
        int total = -1;
        const auto results = idx.match(SearchQuery::parse(QStringLiteral("blue")), false, {}, &total);
        QCOMPARE(results.size(), 2);
        QCOMPARE(total, 2);
    }
};

QTEST_MAIN(TestSearchIndex)
#include "test_search_index.moc"
