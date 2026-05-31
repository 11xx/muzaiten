#include <QTest>

#include "search/RankConfig.h"

using namespace Search;

class TestRankConfig : public QObject {
    Q_OBJECT

private slots:
    void defaultHasRelevanceQualityLibrary()
    {
        const RankConfig c = RankConfig::defaultConfig();
        QCOMPARE(c.rules.size(), 3);
        QCOMPARE(c.rules[0].kind, RankKind::Relevance);
        QCOMPARE(c.rules[1].kind, RankKind::AudioQuality);
        QCOMPARE(c.rules[2].kind, RankKind::LibraryOrder);
        QVERIFY(c.excludes.isEmpty());
    }

    void jsonRoundTrip()
    {
        RankConfig c;
        c.rules.push_back({RankKind::AudioQuality, MusicSort::SortField::AlbumArtist, {},
                           MusicSort::SortDirection::Descending, true});
        c.rules.push_back({RankKind::PreferredDirectory, MusicSort::SortField::AlbumArtist,
                           QStringLiteral("/gak/hires"), MusicSort::SortDirection::Descending, true});
        c.rules.push_back({RankKind::MusicField, MusicSort::SortField::Year, {},
                           MusicSort::SortDirection::Ascending, false});
        c.excludes.push_back({QStringLiteral("*/Podcasts/*"), ExcludeScope::Path});
        c.excludes.push_back({QStringLiteral("*live*"), ExcludeScope::AnyField});

        const RankConfig back = RankConfig::fromJsonString(c.toJsonString());
        QCOMPARE(back.rules.size(), 3);
        QCOMPARE(back.rules[0].kind, RankKind::AudioQuality);
        QCOMPARE(back.rules[1].kind, RankKind::PreferredDirectory);
        QCOMPARE(back.rules[1].param, QStringLiteral("/gak/hires"));
        QCOMPARE(back.rules[2].kind, RankKind::MusicField);
        QCOMPARE(back.rules[2].field, MusicSort::SortField::Year);
        QCOMPARE(back.rules[2].enabled, false);
        QCOMPARE(back.excludes.size(), 2);
        QCOMPARE(back.excludes[0].glob, QStringLiteral("*/Podcasts/*"));
        QCOMPARE(back.excludes[1].scope, ExcludeScope::AnyField);
    }

    void emptyStringFallsBackToDefault()
    {
        const RankConfig c = RankConfig::fromJsonString(QString());
        QCOMPARE(c.rules.size(), 3);
        QCOMPARE(c.rules[0].kind, RankKind::Relevance);
    }
};

QTEST_MAIN(TestRankConfig)
#include "test_rank_config.moc"
