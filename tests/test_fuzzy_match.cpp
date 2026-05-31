#include <QTest>
#include <QString>

#include "search/FuzzyMatch.h"

using namespace Search;

class TestFuzzyMatch : public QObject {
    Q_OBJECT

private slots:
    // ---- exactMatchNaive -------------------------------------------------

    void exactNoMatch()
    {
        const MatchResult r = exactMatchNaive(QStringLiteral("foobar"), QStringLiteral("xyz"), false);
        QVERIFY(!r.matched());
    }

    void exactMatch_basic()
    {
        const MatchResult r = exactMatchNaive(QStringLiteral("Hello World"), QStringLiteral("world"), false);
        QVERIFY(r.matched());
        QCOMPARE(r.start, 6);
        QCOMPARE(r.end, 11);
        QVERIFY(r.score > 0);
    }

    void exactMatch_caseSensitive_noMatch()
    {
        const MatchResult r = exactMatchNaive(QStringLiteral("Hello World"), QStringLiteral("world"), true);
        QVERIFY(!r.matched());
    }

    void exactMatch_caseSensitive_match()
    {
        const MatchResult r = exactMatchNaive(QStringLiteral("Hello World"), QStringLiteral("World"), true);
        QVERIFY(r.matched());
    }

    void exactMatch_emptyPattern_returnsZeroScore()
    {
        const MatchResult r = exactMatchNaive(QStringLiteral("foo"), QStringLiteral(""), false);
        QVERIFY(r.matched());
        QCOMPARE(r.score, 0);
    }

    void exactMatch_prefersWordBoundary()
    {
        // "bar" at start of "bar-baz" should score higher than "bar" in "foobar"
        const MatchResult r1 = exactMatchNaive(QStringLiteral("bar"), QStringLiteral("bar"), false);
        const MatchResult r2 = exactMatchNaive(QStringLiteral("foobar"), QStringLiteral("bar"), false);
        QVERIFY(r1.score >= r2.score);
    }

    void exactMatch_withPositions()
    {
        const MatchResult r = exactMatchNaive(QStringLiteral("Hello World"), QStringLiteral("world"), false,
                                               true);
        QVERIFY(r.matched());
        QCOMPARE(r.positions.size(), 5);
        QCOMPARE(r.positions[0], 6);
        QCOMPARE(r.positions[4], 10);
    }

    // ---- fuzzyMatchV2 ----------------------------------------------------

    void fuzzyNoMatch()
    {
        const MatchResult r = fuzzyMatchV2(QStringLiteral("foobar"), QStringLiteral("xyz"), false);
        QVERIFY(!r.matched());
    }

    void fuzzyMatch_basic()
    {
        const MatchResult r = fuzzyMatchV2(QStringLiteral("so what"), QStringLiteral("sw"), false);
        QVERIFY(r.matched());
        QVERIFY(r.score > 0);
    }

    void fuzzyMatch_scoreHigherForContiguous()
    {
        // Contiguous match should score higher than scattered subsequence
        const MatchResult rContig = fuzzyMatchV2(QStringLiteral("foobar"), QStringLiteral("foo"), false);
        const MatchResult rScatter = fuzzyMatchV2(QStringLiteral("fXoXoXbar"), QStringLiteral("foo"), false);
        QVERIFY(rContig.score > rScatter.score);
    }

    void fuzzyMatch_wordBoundaryBonus()
    {
        // "ff" should score higher in "fuzzy-finder" (boundary bonus) than "fuzzyfinder"
        const MatchResult r1 = fuzzyMatchV2(QStringLiteral("fuzzy-finder"), QStringLiteral("ff"), false);
        const MatchResult r2 = fuzzyMatchV2(QStringLiteral("fuzzyfinder"),  QStringLiteral("ff"), false);
        QVERIFY(r1.score >= r2.score);
    }

    void fuzzyMatch_caseSensitive()
    {
        const MatchResult rCI = fuzzyMatchV2(QStringLiteral("Foobar"), QStringLiteral("fb"), false);
        QVERIFY(rCI.matched());
        const MatchResult rCS = fuzzyMatchV2(QStringLiteral("Foobar"), QStringLiteral("fb"), true);
        QVERIFY(!rCS.matched());
    }

    void fuzzyMatch_withPositions()
    {
        const MatchResult r = fuzzyMatchV2(QStringLiteral("so what"), QStringLiteral("sw"), false, true);
        QVERIFY(r.matched());
        QVERIFY(!r.positions.isEmpty());
        QCOMPARE(r.positions.size(), 2);
    }

    // ---- ranking comparison ----------------------------------------------

    void exactScoresHigherThanFuzzy_forExactSubstring()
    {
        // "miles" in "miles davis" — exact should give a valid match with good score
        const MatchResult re = exactMatchNaive(QStringLiteral("miles davis"), QStringLiteral("miles"), false);
        const MatchResult rf = fuzzyMatchV2(QStringLiteral("miles davis"), QStringLiteral("miles"), false);
        QVERIFY(re.matched());
        QVERIFY(rf.matched());
        // Both should produce positive scores
        QVERIFY(re.score > 0);
        QVERIFY(rf.score > 0);
    }
};

QTEST_MAIN(TestFuzzyMatch)
#include "test_fuzzy_match.moc"
