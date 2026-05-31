#include <QTest>

#include "search/Exclusion.h"
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

    void interiorWildcardUsesRegex()
    {
        ExcludeMatcher m({QStringLiteral("live*2003"), ExcludeScope::AnyField});
        QVERIFY(m.isValid());
        QVERIFY(m.matches(rec(QStringLiteral("Live in London 2003"), QStringLiteral("/m/a.flac"))));
        QVERIFY(!m.matches(rec(QStringLiteral("Studio 2003"), QStringLiteral("/m/a.flac"))));
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
};

QTEST_MAIN(TestExclusion)
#include "test_exclusion.moc"
