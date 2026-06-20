#include <QTest>
#include <QString>
#include <QVector>

#include "search/SearchQuery.h"

using namespace Search;

class TestSearchQuery : public QObject {
    Q_OBJECT

private slots:
    void emptyQuery()
    {
        const SearchQuery q = SearchQuery::parse(QStringLiteral(""));
        QVERIFY(q.isEmpty());
    }

    void whitespaceOnlyQuery()
    {
        const SearchQuery q = SearchQuery::parse(QStringLiteral("   "));
        QVERIFY(q.isEmpty());
    }

    void singleFreeTerm()
    {
        const SearchQuery q = SearchQuery::parse(QStringLiteral("miles"));
        QCOMPARE(q.terms.size(), 1);
        QCOMPARE(q.terms[0].kind, TermKind::FreeText);
        QCOMPARE(q.terms[0].text, QStringLiteral("miles"));
        QVERIFY(!q.terms[0].negate);
        QVERIFY(!q.terms[0].prefixAnchor);
        QVERIFY(!q.terms[0].suffixAnchor);
        QVERIFY(!q.terms[0].forceExact);
    }

    void twoFreeTermsAndedTogether()
    {
        const SearchQuery q = SearchQuery::parse(QStringLiteral("miles blue"));
        QCOMPARE(q.terms.size(), 2);
        QCOMPARE(q.terms[0].kind, TermKind::FreeText);
        QCOMPARE(q.terms[0].text, QStringLiteral("miles"));
        QCOMPARE(q.terms[1].kind, TermKind::FreeText);
        QCOMPARE(q.terms[1].text, QStringLiteral("blue"));
    }

    void negatedTerm()
    {
        const SearchQuery q = SearchQuery::parse(QStringLiteral("!miles"));
        QCOMPARE(q.terms.size(), 1);
        QVERIFY(q.terms[0].negate);
        QCOMPARE(q.terms[0].text, QStringLiteral("miles"));
    }

    void prefixAnchor()
    {
        const SearchQuery q = SearchQuery::parse(QStringLiteral("^miles"));
        QCOMPARE(q.terms.size(), 1);
        QVERIFY(q.terms[0].prefixAnchor);
    }

    void suffixAnchor()
    {
        const SearchQuery q = SearchQuery::parse(QStringLiteral("blue$"));
        QCOMPARE(q.terms.size(), 1);
        QVERIFY(q.terms[0].suffixAnchor);
    }

    void forceExact()
    {
        const SearchQuery q = SearchQuery::parse(QStringLiteral("'miles"));
        QCOMPARE(q.terms.size(), 1);
        QVERIFY(q.terms[0].forceExact);
    }

    void artistFieldToken()
    {
        const SearchQuery q = SearchQuery::parse(QStringLiteral("artist:davis"));
        QCOMPARE(q.terms.size(), 1);
        QCOMPARE(q.terms[0].kind, TermKind::ArtistText);
        QCOMPARE(q.terms[0].text, QStringLiteral("davis"));
    }

    void albumFieldToken()
    {
        const SearchQuery q = SearchQuery::parse(QStringLiteral("album:blue"));
        QCOMPARE(q.terms.size(), 1);
        QCOMPARE(q.terms[0].kind, TermKind::AlbumText);
        QCOMPARE(q.terms[0].text, QStringLiteral("blue"));
    }

    void titleFieldToken()
    {
        const SearchQuery q = SearchQuery::parse(QStringLiteral("title:what"));
        QCOMPARE(q.terms.size(), 1);
        QCOMPARE(q.terms[0].kind, TermKind::TitleText);
        QCOMPARE(q.terms[0].text, QStringLiteral("what"));
    }

    void extToken()
    {
        const SearchQuery q = SearchQuery::parse(QStringLiteral("ext:flac"));
        QCOMPARE(q.terms.size(), 1);
        QCOMPARE(q.terms[0].kind, TermKind::Extension);
        QCOMPARE(q.terms[0].text, QStringLiteral("flac"));
    }

    void khzToken_ge()
    {
        const SearchQuery q = SearchQuery::parse(QStringLiteral("khz:>=96"));
        QCOMPARE(q.terms.size(), 1);
        QCOMPARE(q.terms[0].kind, TermKind::SampleRateHz);
        QCOMPARE(q.terms[0].op, CompareOp::Ge);
        QCOMPARE(q.terms[0].numericValue, 96000LL);
    }

    void khzToken_exact()
    {
        const SearchQuery q = SearchQuery::parse(QStringLiteral("khz:=44"));
        QCOMPARE(q.terms.size(), 1);
        QCOMPARE(q.terms[0].kind, TermKind::SampleRateHz);
        QCOMPARE(q.terms[0].op, CompareOp::Eq);
        QCOMPARE(q.terms[0].numericValue, 44000LL);
    }

    void hzToken()
    {
        const SearchQuery q = SearchQuery::parse(QStringLiteral("hz:>=96000"));
        QCOMPARE(q.terms.size(), 1);
        QCOMPARE(q.terms[0].kind, TermKind::SampleRateHz);
        QCOMPARE(q.terms[0].op, CompareOp::Ge);
        QCOMPARE(q.terms[0].numericValue, 96000LL);
    }

    void kbpsToken()
    {
        const SearchQuery q = SearchQuery::parse(QStringLiteral("kbps:>320"));
        QCOMPARE(q.terms.size(), 1);
        QCOMPARE(q.terms[0].kind, TermKind::BitrateKbps);
        QCOMPARE(q.terms[0].op, CompareOp::Gt);
        QCOMPARE(q.terms[0].numericValue, 320LL);
    }

    void ratingToken()
    {
        const SearchQuery q = SearchQuery::parse(QStringLiteral("rating:>=80"));
        QCOMPARE(q.terms.size(), 1);
        QCOMPARE(q.terms[0].kind, TermKind::Rating);
        QCOMPARE(q.terms[0].op, CompareOp::Ge);
        QCOMPARE(q.terms[0].numericValue, 80LL);
    }

    void yearToken()
    {
        const SearchQuery q = SearchQuery::parse(QStringLiteral("year:>=2000"));
        QCOMPARE(q.terms.size(), 1);
        QCOMPARE(q.terms[0].kind, TermKind::Year);
        QCOMPARE(q.terms[0].op, CompareOp::Ge);
        QCOMPARE(q.terms[0].numericValue, 2000LL);
    }

    void durTokenMSS()
    {
        const SearchQuery q = SearchQuery::parse(QStringLiteral("dur:>3:30"));
        QCOMPARE(q.terms.size(), 1);
        QCOMPARE(q.terms[0].kind, TermKind::DurationMs);
        QCOMPARE(q.terms[0].op, CompareOp::Gt);
        QCOMPARE(q.terms[0].numericValue, (3 * 60 + 30) * 1000LL);
    }

    void channelsToken()
    {
        const SearchQuery q = SearchQuery::parse(QStringLiteral("ch:2"));
        QCOMPARE(q.terms.size(), 1);
        QCOMPARE(q.terms[0].kind, TermKind::Channels);
        QCOMPARE(q.terms[0].numericValue, 2LL);
    }

    void mixedQuery()
    {
        const SearchQuery q = SearchQuery::parse(QStringLiteral("miles artist:davis ext:flac rating:>=80"));
        QCOMPARE(q.terms.size(), 4);
        QCOMPARE(q.terms[0].kind, TermKind::FreeText);
        QCOMPARE(q.terms[1].kind, TermKind::ArtistText);
        QCOMPARE(q.terms[2].kind, TermKind::Extension);
        QCOMPARE(q.terms[3].kind, TermKind::Rating);
    }
};

    void quotedFieldValuePhrase()
    {
        // artist:"the beatles" → ONE artist term holding the whole phrase (space
        // kept), not two AND-ed words.
        const SearchQuery q = SearchQuery::parse(QStringLiteral("artist:\"the beatles\""));
        QCOMPARE(q.terms.size(), 1);
        QCOMPARE(q.terms[0].kind, TermKind::ArtistText);
        QCOMPARE(q.terms[0].text, QStringLiteral("the beatles"));
    }

    void quotedValuesAcrossFields()
    {
        const SearchQuery q = SearchQuery::parse(
            QStringLiteral("artist:\"suni clay\" title:\"my hood\""));
        QCOMPARE(q.terms.size(), 2);
        QCOMPARE(q.terms[0].kind, TermKind::ArtistText);
        QCOMPARE(q.terms[0].text, QStringLiteral("suni clay"));
        QCOMPARE(q.terms[1].kind, TermKind::TitleText);
        QCOMPARE(q.terms[1].text, QStringLiteral("my hood"));
    }

    void quotedValueCondensesWhitespaceAndFolds()
    {
        // Internal whitespace condenses to single spaces; folding still applies.
        const SearchQuery q = SearchQuery::parse(QStringLiteral("artist:\"  Beyoncé   Knowles \""));
        QCOMPARE(q.terms.size(), 1);
        QCOMPARE(q.terms[0].text, QStringLiteral("beyonce knowles"));
    }

    void quotedValueEscapedQuote()
    {
        // A literal double quote inside the value via \".
        const SearchQuery q = SearchQuery::parse(QStringLiteral("title:\"say \\\"hi\\\"\""));
        QCOMPARE(q.terms.size(), 1);
        QCOMPARE(q.terms[0].kind, TermKind::TitleText);
        QCOMPARE(q.terms[0].text, QStringLiteral("say \"hi\""));
    }

    void quoteFieldValueRoundTrips()
    {
        // quoteFieldValue() builds a value that parses back to the same phrase.
        const QString built = QStringLiteral("artist:%1").arg(quoteFieldValue(QStringLiteral("suni clay")));
        const SearchQuery q = SearchQuery::parse(built);
        QCOMPARE(q.terms.size(), 1);
        QCOMPARE(q.terms[0].kind, TermKind::ArtistText);
        QCOMPARE(q.terms[0].text, QStringLiteral("suni clay"));
    }

QTEST_MAIN(TestSearchQuery)
#include "test_search_query.moc"
