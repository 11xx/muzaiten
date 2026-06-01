#include <QTest>

#include "search/SearchMatcher.h"
#include "search/SearchQuery.h"

using namespace Search;

namespace {

MatchField field(MatchFieldRole role, const QString &text, int weight = 0)
{
    return {role, text, text.toLower(), weight};
}

MatchDocument doc(int row, QVector<MatchField> fields, QVector<MatchNumeric> numeric = {})
{
    return {row, std::move(fields), std::move(numeric)};
}

bool matches(const MatchDocument &document, const QString &query, bool fuzzy = false)
{
    return matchDocument(document, SearchQuery::parse(query), fuzzy) != INT_MIN;
}

} // namespace

class TestSearchMatcher : public QObject {
    Q_OBJECT

private slots:
    void freeTextMatchesAnyExposedField()
    {
        const MatchDocument document = doc(0, {
            field(MatchFieldRole::Title, QStringLiteral("So What")),
            field(MatchFieldRole::Artist, QStringLiteral("Miles Davis")),
        });
        QVERIFY(matches(document, QStringLiteral("miles")));
        QVERIFY(matches(document, QStringLiteral("what")));
    }

    void fieldPrefixesRouteToExpectedFields()
    {
        const MatchDocument document = doc(0, {
            field(MatchFieldRole::Title, QStringLiteral("Song Title")),
            field(MatchFieldRole::Artist, QStringLiteral("Track Artist")),
            field(MatchFieldRole::AlbumArtist, QStringLiteral("Album Artist")),
            field(MatchFieldRole::Album, QStringLiteral("Album Name")),
            field(MatchFieldRole::Path, QStringLiteral("/music/artist/album/song.flac")),
            field(MatchFieldRole::Filename, QStringLiteral("song.flac")),
            field(MatchFieldRole::Codec, QStringLiteral("flac")),
        });

        QVERIFY(matches(document, QStringLiteral("title:song")));
        QVERIFY(matches(document, QStringLiteral("artist:track")));
        QVERIFY(matches(document, QStringLiteral("artist:album")));
        QVERIFY(matches(document, QStringLiteral("albumartist:album")));
        QVERIFY(matches(document, QStringLiteral("aa:album")));
        QVERIFY(matches(document, QStringLiteral("album:name")));
        QVERIFY(matches(document, QStringLiteral("path:/music")));
        QVERIFY(matches(document, QStringLiteral("file:song")));
        QVERIFY(matches(document, QStringLiteral("codec:fla")));
        QVERIFY(matches(document, QStringLiteral("ext:flac")));
        QVERIFY(!matches(document, QStringLiteral("ext:mp3")));
    }

    void allTermsMustMatch()
    {
        const MatchDocument document = doc(0, {
            field(MatchFieldRole::Title, QStringLiteral("Blue in Green")),
            field(MatchFieldRole::Artist, QStringLiteral("Miles Davis")),
        });
        QVERIFY(matches(document, QStringLiteral("blue miles")));
        QVERIFY(!matches(document, QStringLiteral("blue coltrane")));
    }

    void negatedTermExcludesMatchingDocument()
    {
        const MatchDocument document = doc(0, {
            field(MatchFieldRole::Title, QStringLiteral("Blue in Green")),
            field(MatchFieldRole::Artist, QStringLiteral("Miles Davis")),
        });
        QVERIFY(matches(document, QStringLiteral("miles !red")));
        QVERIFY(!matches(document, QStringLiteral("miles !blue")));
    }

    void anchorsWork()
    {
        const MatchDocument document = doc(0, {
            field(MatchFieldRole::Title, QStringLiteral("Blue in Green")),
        });
        QVERIFY(matches(document, QStringLiteral("^blue")));
        QVERIFY(matches(document, QStringLiteral("green$")));
        QVERIFY(!matches(document, QStringLiteral("^green")));
    }

    void fuzzyModeMatchesSubsequence()
    {
        const MatchDocument document = doc(0, {
            field(MatchFieldRole::Title, QStringLiteral("So What")),
        });
        QVERIFY(matches(document, QStringLiteral("sw"), true));
        QVERIFY(!matches(document, QStringLiteral("sx"), true));
    }

    void numericTermsOnlyMatchExposedValues()
    {
        const MatchDocument document = doc(0,
            {field(MatchFieldRole::Title, QStringLiteral("HiRes Track"))},
            {
                {TermKind::Year, 2024},
                {TermKind::Rating, 90},
                {TermKind::SampleRateHz, 96000},
            });

        QVERIFY(matches(document, QStringLiteral("year:>=2020")));
        QVERIFY(matches(document, QStringLiteral("rating:>=80")));
        QVERIFY(matches(document, QStringLiteral("khz:>=96")));
        QVERIFY(!matches(document, QStringLiteral("dur:>3:00")));
    }

    void displayOrderIsPreserved()
    {
        const QVector<MatchDocument> documents = {
            doc(7, {field(MatchFieldRole::Title, QStringLiteral("Beta Match"))}),
            doc(2, {field(MatchFieldRole::Title, QStringLiteral("Alpha Match"))}),
            doc(5, {field(MatchFieldRole::Title, QStringLiteral("Nope"))}),
        };

        const QVector<PanelMatch> matches = matchDocumentsInDisplayOrder(documents, SearchQuery::parse(QStringLiteral("match")), false);
        QCOMPARE(matches.size(), 2);
        QCOMPARE(matches.at(0).row, 7);
        QCOMPARE(matches.at(1).row, 2);
    }
};

QTEST_MAIN(TestSearchMatcher)
#include "test_search_matcher.moc"
