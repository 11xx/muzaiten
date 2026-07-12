#include "ui/SemanticSearchData.h"

#include <QtTest>

using SemanticSearchData::GroupScore;

class TestSemanticSearchData : public QObject
{
    Q_OBJECT

private slots:
    void ranksBestFirstAndCaps();
    void skipsDimensionMismatchesAndTiesDeterministically();
    void formatsQuality();
    void formatsStarsAndYears();
};

void TestSemanticSearchData::ranksBestFirstAndCaps()
{
    QHash<qint64, QVector<float>> embeddings;
    embeddings.insert(1, {1.0F, 0.0F});
    embeddings.insert(2, {0.0F, 1.0F});
    embeddings.insert(3, {0.7071F, 0.7071F});

    const QVector<GroupScore> ranked = SemanticSearchData::rankEmbeddings({1.0F, 0.0F}, embeddings, 2);
    QCOMPARE(ranked.size(), 2);
    QCOMPARE(ranked.at(0).groupId, 1);
    QCOMPARE(ranked.at(1).groupId, 3);
    QVERIFY(ranked.at(0).score > ranked.at(1).score);
}

void TestSemanticSearchData::skipsDimensionMismatchesAndTiesDeterministically()
{
    QHash<qint64, QVector<float>> embeddings;
    embeddings.insert(7, {1.0F, 0.0F});
    embeddings.insert(4, {1.0F, 0.0F});
    embeddings.insert(9, {1.0F, 0.0F, 0.0F}); // wrong dimension: skipped

    const QVector<GroupScore> ranked = SemanticSearchData::rankEmbeddings({1.0F, 0.0F}, embeddings, 10);
    QCOMPARE(ranked.size(), 2);
    QCOMPARE(ranked.at(0).groupId, 4); // equal score, lower id first
    QCOMPARE(ranked.at(1).groupId, 7);
}

void TestSemanticSearchData::formatsQuality()
{
    Track lossless;
    lossless.codec = QStringLiteral("flac");
    lossless.bitDepth = 16;
    lossless.sampleRateHz = 44100;
    QCOMPARE(SemanticSearchData::formatQuality(lossless), QStringLiteral("FLAC 16bit/44.1kHz"));

    Track lossy;
    lossy.codec = QStringLiteral("mp3");
    lossy.bitrateKbps = 320;
    QCOMPARE(SemanticSearchData::formatQuality(lossy), QStringLiteral("MP3 320kbps"));

    QCOMPARE(SemanticSearchData::formatQuality(Track{}), QString());
}

void TestSemanticSearchData::formatsStarsAndYears()
{
    QCOMPARE(SemanticSearchData::starText(-1), QString());
    QCOMPARE(SemanticSearchData::starText(100), QStringLiteral("★★★★★"));
    QCOMPARE(SemanticSearchData::starText(60), QStringLiteral("★★★☆☆"));

    QCOMPARE(SemanticSearchData::yearText(QStringLiteral("2021-05-01")), QStringLiteral("2021"));
    QCOMPARE(SemanticSearchData::yearText(QStringLiteral("197")), QString());
    QCOMPARE(SemanticSearchData::yearText(QStringLiteral("abcd-ef")), QString());
}

QTEST_MAIN(TestSemanticSearchData)
#include "test_semantic_search_data.moc"
