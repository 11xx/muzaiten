#include "features/QualityRank.h"

#include <QTest>

class QualityRankTest final : public QObject {
    Q_OBJECT

private slots:
    void ranksLosslessOverLossyAndHiResOverCd();
    void vinylPenaltyDropsHiResBelowCleanCd();
    void dsdBonusWins();
    void pinOverridesScore();
};

void QualityRankTest::ranksLosslessOverLossyAndHiResOverCd()
{
    const QualityRank::Copy mp3{QStringLiteral("/mp3"), QStringLiteral("mp3"), 0, 44100, 320, {}};
    const QualityRank::Copy cd{QStringLiteral("/cd"), QStringLiteral("flac"), 16, 44100, 0, {}};
    const QualityRank::Copy hires{QStringLiteral("/hires"), QStringLiteral("flac"), 24, 96000, 0, {}};
    QVERIFY(QualityRank::score(cd) > QualityRank::score(mp3));
    QVERIFY(QualityRank::score(hires) > QualityRank::score(cd));
    QCOMPARE(QualityRank::bestPath({mp3, cd, hires}), QStringLiteral("/hires"));
}

void QualityRankTest::vinylPenaltyDropsHiResBelowCleanCd()
{
    const QualityRank::Copy cd{QStringLiteral("/cd"), QStringLiteral("flac"), 16, 44100, 0, {}};
    const QualityRank::Copy vinyl{QStringLiteral("/vinyl"), QStringLiteral("flac"), 24, 96000, 0,
                                  {QStringLiteral("Vinyl")}};
    QVERIFY(QualityRank::score(vinyl) < QualityRank::score(cd));
    QCOMPARE(QualityRank::bestPath({vinyl, cd}), QStringLiteral("/cd"));
}

void QualityRankTest::dsdBonusWins()
{
    const QualityRank::Copy hires{QStringLiteral("/hires"), QStringLiteral("flac"), 24, 192000, 0, {}};
    const QualityRank::Copy dsd{QStringLiteral("/dsd"), QStringLiteral("dsf"), 1, 2822400, 0, {}};
    QVERIFY(QualityRank::score(dsd) > QualityRank::score(hires));
    QCOMPARE(QualityRank::bestPath({hires, dsd}), QStringLiteral("/dsd"));
}

void QualityRankTest::pinOverridesScore()
{
    const QualityRank::Copy mp3{QStringLiteral("/mp3"), QStringLiteral("mp3"), 0, 44100, 320, {}};
    const QualityRank::Copy hires{QStringLiteral("/hires"), QStringLiteral("flac"), 24, 96000, 0, {}};
    QCOMPARE(QualityRank::bestPath({mp3, hires}, QStringLiteral("/mp3")), QStringLiteral("/mp3"));
}

QTEST_MAIN(QualityRankTest)
#include "test_quality_rank.moc"
