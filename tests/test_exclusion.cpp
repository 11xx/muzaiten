#include <QTest>

#include <utility>

#include "search/Exclusion.h"
#include "search/NarrowingCache.h"
#include "search/SearchIndex.h"
#include "search/SearchQuery.h"
#include "search/SearchRecord.h"

using namespace Search;

static SearchRecord rec(const QString &title, const QString &path)
{
    SearchRecord r;
    r.title = title;
    r.path = path;
    r.filename = path.section(QLatin1Char('/'), -1);
    r.normTitle = title.toLower();
    r.normPath = path.toLower();
    r.normFilename = r.filename.toLower();
    return r;
}

static SearchRecord foldedRec(const QString &title, const QString &path)
{
    SearchRecord r = rec(title, path);
    foldRecordNorms(r);
    return r;
}

class TestExclusion : public QObject {
    Q_OBJECT

private slots:
    void substringFromBareWord()
    {
        ExcludeMatcher m({QStringLiteral("podcast"), ExcludeScope::Path});
        QVERIFY(m.isValid());
        QVERIFY(m.matches(rec(QStringLiteral("Ep 1"), QStringLiteral("/gak/Podcasts/ep1.mp3"))));
        QVERIFY(!m.matches(rec(QStringLiteral("Song"), QStringLiteral("/gak/Music/song.flac"))));
    }

    void surroundedGlobIsSubstring()
    {
        ExcludeMatcher m({QStringLiteral("*/Podcasts/*"), ExcludeScope::Path});
        QVERIFY(m.matches(rec(QStringLiteral("x"), QStringLiteral("/gak/Podcasts/a.mp3"))));
        QVERIFY(!m.matches(rec(QStringLiteral("x"), QStringLiteral("/gak/Music/a.mp3"))));
    }

    void suffixGlob()
    {
        ExcludeMatcher m({QStringLiteral("*.m4b"), ExcludeScope::Path});
        QVERIFY(m.matches(rec(QStringLiteral("Book"), QStringLiteral("/gak/audiobooks/b.m4b"))));
        QVERIFY(!m.matches(rec(QStringLiteral("Song"), QStringLiteral("/gak/m/s.flac"))));
    }

    void prefixGlob()
    {
        ExcludeMatcher m({QStringLiteral("/mnt/temp*"), ExcludeScope::Path});
        QVERIFY(m.matches(rec(QStringLiteral("x"), QStringLiteral("/mnt/temp/a.flac"))));
        QVERIFY(!m.matches(rec(QStringLiteral("x"), QStringLiteral("/mnt/music/a.flac"))));
    }

    void prefixAndSuffixGlobsApplyToEachFoldedForm()
    {
        SearchRecord r = rec(QStringLiteral("The Wall"), QStringLiteral("/m/a.flac"));
        r.titleSort = QStringLiteral("Wall, The");
        foldRecordNorms(r);

        QVERIFY(ExcludeMatcher({QStringLiteral("wall*"), ExcludeScope::AnyField}).matches(r));
        QVERIFY(ExcludeMatcher({QStringLiteral("*wall"), ExcludeScope::AnyField}).matches(r));
        QVERIFY(!ExcludeMatcher({QStringLiteral("*floyd"), ExcludeScope::AnyField}).matches(r));
    }

    void interiorWildcardsMatchOneFoldedFormAtATime()
    {
        // Müller folds to muller, mueller and miller; no single form matches
        // r*m or r?m, only a span across two of them would.
        SearchRecord r = rec(QString::fromUtf8("Müller"), QStringLiteral("/m/a.flac"));
        foldRecordNorms(r);
        QVERIFY(!ExcludeMatcher({QStringLiteral("r*m"), ExcludeScope::AnyField}).matches(r));
        QVERIFY(!ExcludeMatcher({QStringLiteral("er?mi"), ExcludeScope::AnyField}).matches(r));
        QVERIFY(ExcludeMatcher({QStringLiteral("mi*er"), ExcludeScope::AnyField}).matches(r));
        QVERIFY(ExcludeMatcher({QStringLiteral("mue?ler"), ExcludeScope::AnyField}).matches(r));
    }

    void interiorWildcardUsesRegex()
    {
        ExcludeMatcher m({QStringLiteral("live*2003"), ExcludeScope::AnyField});
        QVERIFY(m.isValid());
        QVERIFY(m.matches(rec(QStringLiteral("Live in London 2003"), QStringLiteral("/m/a.flac"))));
        QVERIFY(!m.matches(rec(QStringLiteral("Studio 2003"), QStringLiteral("/m/a.flac"))));
    }

    void accentedGlobLiteralsUseQueryFolding()
    {
        const SearchRecord r = foldedRec(QString::fromUtf8("Café Society"), QStringLiteral("/m/a.flac"));
        QVERIFY(ExcludeMatcher({QString::fromUtf8("*café*"), ExcludeScope::AnyField}).matches(r));
        QVERIFY(ExcludeMatcher({QString::fromUtf8("café*"), ExcludeScope::AnyField}).matches(r));
        QVERIFY(ExcludeMatcher({QString::fromUtf8("CAFÉ*"), ExcludeScope::AnyField}).matches(r));
    }

    void hanGlobLiteralsUseQueryFolding()
    {
        const SearchRecord r = foldedRec(QString::fromUtf8("東京"), QStringLiteral("/m/a.flac"));
        QVERIFY(ExcludeMatcher({QString::fromUtf8("*東京*"), ExcludeScope::AnyField}).matches(r));
    }

    void questionWildcardStillMatchesFoldedText()
    {
        const SearchRecord r = foldedRec(QString::fromUtf8("Café"), QStringLiteral("/m/a.flac"));
        QVERIFY(ExcludeMatcher({QStringLiteral("caf?"), ExcludeScope::AnyField}).matches(r));
    }

    void emptyFoldedLiteralRunDoesNotBecomeAWildcard()
    {
        // A run that folds to nothing (a zero-width space) disappears: the glob
        // below is the suffix pattern *ab, not a wildcard at its start.
        const ExcludeMatcher suffix({QStringLiteral("\u200B*ab"), ExcludeScope::AnyField});
        QVERIFY(suffix.matches(foldedRec(QStringLiteral("xab"), QStringLiteral("/m/a.flac"))));
        QVERIFY(!suffix.matches(foldedRec(QStringLiteral("abx"), QStringLiteral("/m/b.flac"))));
        QVERIFY(!ExcludeMatcher({QStringLiteral("\u200B"), ExcludeScope::AnyField}).isValid());
    }

    void foldedWildcardLookalikesRemainLiteral()
    {
        const ExcludeMatcher substring({QString::fromUtf8("a＊b"), ExcludeScope::AnyField});
        QVERIFY(substring.matches(foldedRec(QStringLiteral("a*b"), QStringLiteral("/m/a.flac"))));
        QVERIFY(!substring.matches(foldedRec(QStringLiteral("axb"), QStringLiteral("/m/b.flac"))));
        // With ? the glob becomes a regex, where the folded * must stay escaped.
        const ExcludeMatcher regex({QString::fromUtf8("a＊b?"), ExcludeScope::AnyField});
        QVERIFY(regex.matches(foldedRec(QStringLiteral("a*bX"), QStringLiteral("/m/c.flac"))));
        QVERIFY(!regex.matches(foldedRec(QStringLiteral("axbX"), QStringLiteral("/m/d.flac"))));
    }

    void asciiGlobMatchingRemainsCaseInsensitive()
    {
        const SearchRecord r = foldedRec(QStringLiteral("Live in London 2003"), QStringLiteral("/m/a.flac"));
        QVERIFY(ExcludeMatcher({QStringLiteral("LIVE*2003"), ExcludeScope::AnyField}).matches(r));
        QVERIFY(!ExcludeMatcher({QStringLiteral("STUDIO*2003"), ExcludeScope::AnyField}).matches(r));
    }

    void pathScopeDoesNotMatchTitle()
    {
        ExcludeMatcher m({QStringLiteral("live"), ExcludeScope::Path});
        QVERIFY(!m.matches(rec(QStringLiteral("Live Forever"), QStringLiteral("/m/song.flac"))));
    }

    void anyFieldScopeMatchesTitle()
    {
        ExcludeMatcher m({QStringLiteral("live"), ExcludeScope::AnyField});
        QVERIFY(m.matches(rec(QStringLiteral("Live Forever"), QStringLiteral("/m/song.flac"))));
    }

    void matchAppliesExclusionsAndLowersTotal()
    {
        SearchIndex idx;
        QVector<SearchRecord> recs;
        for (int i = 0; i < 3; ++i) {
            recs.push_back(rec(QStringLiteral("Blue Song"), QStringLiteral("/gak/Music/blue%1.flac").arg(i)));
        }
        recs.push_back(rec(QStringLiteral("Blue Pod"), QStringLiteral("/gak/Podcasts/blue.mp3")));
        idx.build(recs);

        const ExclusionSet ex = compileExcludes({{QStringLiteral("*/Podcasts/*"), ExcludeScope::Path}});
        int total = -1;
        const auto results = idx.match(SearchQuery::parse(QStringLiteral("blue")), false, ex, &total);
        QCOMPARE(total, 3);          // the podcast was excluded before scoring
        QCOMPARE(results.size(), 3);
    }

    void hiddenMatchesFollowTheQueryAndCountEachRecordOnce()
    {
        SearchIndex idx;
        idx.build({
            rec(QStringLiteral("Blue One"), QStringLiteral("/music/blue.flac")),
            rec(QStringLiteral("Blue Two"), QStringLiteral("/hidden/blue.flac")),
            rec(QStringLiteral("Blue Three"), QStringLiteral("/hidden/blue3.flac")),
            rec(QStringLiteral("Green Four"), QStringLiteral("/hidden/green.flac")),
        });
        const ExclusionSet excludes = compileExcludes({
            {QStringLiteral("*/hidden/*"), ExcludeScope::Path},
            {QStringLiteral("*/hidden/blue*"), ExcludeScope::Path},
        });
        int visible = -1;
        int hidden = -1;
        const auto blue = idx.match(SearchQuery::parse(QStringLiteral("blue")), false,
                                    excludes, &visible, nullptr, nullptr, {}, &hidden);
        QCOMPARE(visible, 1);
        QCOMPARE(hidden, 2);
        QCOMPARE(blue.size(), 1);

        const auto green = idx.match(SearchQuery::parse(QStringLiteral("green")), false,
                                     excludes, &visible, nullptr, nullptr, {}, &hidden);
        QCOMPARE(visible, 0);
        QCOMPARE(hidden, 1);
        QVERIFY(green.isEmpty());

        idx.match(SearchQuery::parse(QString()), false, excludes,
                  &visible, nullptr, nullptr, {}, &hidden);
        QCOMPARE(visible, 0);
        QCOMPARE(hidden, 0);
    }

    void hiddenCountIsNotLimitedByDisplayedResults()
    {
        QVector<SearchRecord> records;
        records.reserve(kMaxResults + 2);
        records.push_back(rec(QStringLiteral("Blue Visible"), QStringLiteral("/music/visible.flac")));
        for (int i = 0; i <= kMaxResults; ++i) {
            records.push_back(rec(QStringLiteral("Blue Hidden"),
                                  QStringLiteral("/hidden/%1.flac").arg(i)));
        }
        SearchIndex idx;
        idx.build(std::move(records));
        const ExclusionSet excludes = compileExcludes({
            {QStringLiteral("*/hidden/*"), ExcludeScope::Path},
        });
        int visible = -1;
        int hidden = -1;
        const auto results = idx.match(SearchQuery::parse(QStringLiteral("blue")), false,
                                       excludes, &visible, nullptr, nullptr, {}, &hidden);
        QCOMPARE(visible, 1);
        QCOMPARE(hidden, kMaxResults + 1);
        QCOMPARE(results.size(), 1);
    }

    void narrowingRetainsHiddenCandidates()
    {
        SearchIndex idx;
        idx.build({
            rec(QStringLiteral("Blue Alpha"), QStringLiteral("/music/a.flac")),
            rec(QStringLiteral("Blue Alpha"), QStringLiteral("/hidden/a.flac")),
            rec(QStringLiteral("Blue Beta"), QStringLiteral("/hidden/b.flac")),
        });
        const ExclusionSet excludes = compileExcludes({
            {QStringLiteral("*/hidden/*"), ExcludeScope::Path},
        });
        NarrowingCache cache;
        int visible = -1;
        int hidden = -1;
        cache.run(idx, SearchQuery::parse(QStringLiteral("blue")), false,
                  excludes, 1, &visible, {}, &hidden);
        QCOMPARE(visible, 1);
        QCOMPARE(hidden, 2);
        cache.run(idx, SearchQuery::parse(QStringLiteral("blue alpha")), false,
                  excludes, 1, &visible, {}, &hidden);
        QCOMPARE(visible, 1);
        QCOMPARE(hidden, 1);
        cache.run(idx, SearchQuery::parse(QStringLiteral("blue")), false,
                  {}, 2, &visible, {}, &hidden);
        QCOMPARE(visible, 3);
        QCOMPARE(hidden, 0);
        cache.run(idx, SearchQuery::parse(QStringLiteral("blue")), true,
                  excludes, 3, &visible, {}, &hidden);
        QCOMPARE(visible, 1);
        QCOMPARE(hidden, 2);
    }
};

QTEST_MAIN(TestExclusion)
#include "test_exclusion.moc"
