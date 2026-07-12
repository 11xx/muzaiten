#include "features/QueryVectorCache.h"

#include <QTemporaryDir>
#include <QtTest>

namespace {

QueryVectorCache::Identity identity()
{
    return {QStringLiteral("clap"), QStringLiteral("laion-clap-music-audioset"),
            QStringLiteral("fae3e9c0"), QStringLiteral("clap-htsat-base-audio-window-v1"), 4};
}

QVector<float> sampleVector()
{
    return {0.5F, -0.25F, 0.125F, 0.8125F};
}

} // namespace

class TestQueryVectorCache : public QObject
{
    Q_OBJECT

private slots:
    void missThenStoreThenHit();
    void normalizesWhitespace();
    void rejectsIdentityMismatch();
    void rejectsWrongDimension();
    void survivesReopen();
};

void TestQueryVectorCache::missThenStoreThenHit()
{
    QTemporaryDir dir;
    QueryVectorCache cache(dir.filePath(QStringLiteral("q.sqlite")));
    QVERIFY(cache.isOpen());

    QVERIFY(cache.lookup(identity(), QStringLiteral("melancholic shoegaze")).isEmpty());
    QVERIFY(cache.store(identity(), QStringLiteral("melancholic shoegaze"), sampleVector()));
    QCOMPARE(cache.lookup(identity(), QStringLiteral("melancholic shoegaze")), sampleVector());
}

void TestQueryVectorCache::normalizesWhitespace()
{
    QTemporaryDir dir;
    QueryVectorCache cache(dir.filePath(QStringLiteral("q.sqlite")));
    QVERIFY(cache.store(identity(), QStringLiteral("  piano   jazz  "), sampleVector()));
    QCOMPARE(cache.lookup(identity(), QStringLiteral("piano jazz")), sampleVector());
}

void TestQueryVectorCache::rejectsIdentityMismatch()
{
    QTemporaryDir dir;
    QueryVectorCache cache(dir.filePath(QStringLiteral("q.sqlite")));
    QVERIFY(cache.store(identity(), QStringLiteral("dark ambient"), sampleVector()));

    QueryVectorCache::Identity revised = identity();
    revised.featureRevision = QStringLiteral("clap-htsat-base-audio-window-v2");
    QVERIFY(cache.lookup(revised, QStringLiteral("dark ambient")).isEmpty());

    QueryVectorCache::Identity otherModel = identity();
    otherModel.model = QStringLiteral("other-model");
    QVERIFY(cache.lookup(otherModel, QStringLiteral("dark ambient")).isEmpty());
}

void TestQueryVectorCache::rejectsWrongDimension()
{
    QTemporaryDir dir;
    QueryVectorCache cache(dir.filePath(QStringLiteral("q.sqlite")));
    QVERIFY(!cache.store(identity(), QStringLiteral("dark ambient"), {0.5F, 0.5F}));

    QVERIFY(cache.store(identity(), QStringLiteral("dark ambient"), sampleVector()));
    QueryVectorCache::Identity wider = identity();
    wider.vectorDimension = 8;
    QVERIFY(cache.lookup(wider, QStringLiteral("dark ambient")).isEmpty());
}

void TestQueryVectorCache::survivesReopen()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("q.sqlite"));
    {
        QueryVectorCache cache(path);
        QVERIFY(cache.store(identity(), QStringLiteral("rainy morning"), sampleVector()));
    }
    QueryVectorCache cache(path);
    QCOMPARE(cache.lookup(identity(), QStringLiteral("rainy morning")), sampleVector());
}

QTEST_MAIN(TestQueryVectorCache)
#include "test_query_vector_cache.moc"
