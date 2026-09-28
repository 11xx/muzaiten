#include <atomic>
#include <QTest>

#include "search/NarrowingCache.h"
#include "search/SearchRecord.h"

using namespace Search;

static SearchRecord record(const QString &title, const QString &path)
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

class TestNarrowingCache : public QObject {
    Q_OBJECT
private slots:
    void queryRule()
    {
        const auto check = [](const char *before, const char *after, bool expected) {
            QCOMPARE(narrows(SearchQuery::parse(QString::fromLatin1(before)),
                             SearchQuery::parse(QString::fromLatin1(after))), expected);
        };
        check("blue", "blues", true);
        check("blues", "blue", false);
        check("blue", "blue miles", true);
        check("blue", "blue !live", true);
        check("!blue", "!blues", false);
        check("blue$", "blues$", false);
        check("^blue", "^blues", true);
        check("artist", "artist:", false);
        check("blue miles", "blues milesd", false);
        check("year:2000", "year:2001", false);
        check("title:\"blue", "title:\"blues", true);
        check("title:blue", "title:\"blue\"", true);
        check("blue", "'blue", false);
        check("'blue", "'blues", true);
        check("ab", "ab/", false);
        check("ab/", "ab/c", true);
    }

    // A fuzzy term gaining '/' leaves per-folder path matching for the whole
    // path, which can match more: the cached run must not reuse candidates.
    void slashLeavesComponentMatching()
    {
        SearchIndex index;
        index.build({record(QString(), QStringLiteral("/a/b/zzz.bin"))});
        for (const QString &slashed : {QStringLiteral("ab/"), QString::fromUtf8("ab\xef\xbc\x8f")}) {
            NarrowingCache cache;
            int total = -1;
            cache.run(index, SearchQuery::parse(QStringLiteral("ab")), true, {}, 0, &total);
            QCOMPARE(total, 0);
            int cached = -1, fresh = -1;
            cache.run(index, SearchQuery::parse(slashed), true, {}, 0, &cached);
            index.match(SearchQuery::parse(slashed), true, {}, &fresh);
            QCOMPARE(fresh, 1);
            QCOMPARE(cached, fresh);
        }
    }

    // Cancelling in the middle of a multi-worker scan leaves no partial state:
    // the next query equals a fresh scan.
    void cancelMidScan()
    {
        QVector<SearchRecord> records;
        for (int i = 0; i < 42000; ++i) {
            records.push_back(record(QStringLiteral("blue %1").arg(i), QStringLiteral("/music/%1.ogg").arg(i)));
        }
        SearchIndex index;
        index.build(records);
        NarrowingCache cache;
        std::atomic<int> polls{0};
        int total = -1;
        const auto cancelled = cache.run(index, SearchQuery::parse(QStringLiteral("blu")), false, {}, 0,
                                         &total, [&] { return ++polls > 8; });
        QVERIFY(polls > 8);
        QVERIFY(cancelled.isEmpty());
        QCOMPARE(total, 0);
        int cachedTotal = -1, freshTotal = -1;
        const auto cachedRun = cache.run(index, SearchQuery::parse(QStringLiteral("blue")), false, {}, 0, &cachedTotal);
        const auto fresh = index.match(SearchQuery::parse(QStringLiteral("blue")), false, {}, &freshTotal);
        QCOMPARE(cachedTotal, freshTotal);
        QCOMPARE(cachedRun.size(), fresh.size());
        for (qsizetype i = 0; i < fresh.size(); ++i) QCOMPARE(cachedRun[i].index, fresh[i].index);
    }

    void freshParity()
    {
        SearchIndex index;
        index.build({record(QStringLiteral("Blue Moon"), QStringLiteral("/music/blue.ogg")),
                     record(QStringLiteral("Blues Night"), QStringLiteral("/music/blues.ogg")),
                     record(QStringLiteral("Other"), QStringLiteral("/music/other.ogg"))});
        NarrowingCache cache;
        ExclusionSet excludes;
        quint64 version = 0;
        const auto check = [&](const QString &text, bool fuzzy, const std::function<bool()> &cancelled = {}) {
            const SearchQuery q = SearchQuery::parse(text);
            int cachedTotal = -1, freshTotal = -1;
            const auto cached = cache.run(index, q, fuzzy, excludes, version, &cachedTotal, cancelled);
            if (cancelled && cancelled()) {
                QCOMPARE(cachedTotal, 0);
                QVERIFY(cached.isEmpty());
                return;
            }
            const auto fresh = index.match(q, fuzzy, excludes, &freshTotal);
            QCOMPARE(cachedTotal, freshTotal);
            QCOMPARE(cached.size(), fresh.size());
            for (qsizetype i = 0; i < fresh.size(); ++i) {
                QCOMPARE(cached[i].index, fresh[i].index);
                QCOMPARE(cached[i].score, fresh[i].score);
            }
        };
        for (bool fuzzy : {false, true}) {
            for (const auto &query : {QStringLiteral("b"), QStringLiteral("bl"),
                                      QStringLiteral("blu"), QStringLiteral("blue")}) check(query, fuzzy);
        }
        check(QStringLiteral("b"), false);
        check(QStringLiteral("bm"), true);
        check(QStringLiteral("blue"), true);
        index.append({record(QStringLiteral("Bluest Day"), QStringLiteral("/music/bluest.ogg"))});
        check(QStringLiteral("blues"), true);
        check(QStringLiteral("bluest"), false);
        excludes = compileExcludes({{QStringLiteral("*bluest*"), ExcludeScope::AnyField}});
        ++version;
        check(QStringLiteral("bluest d"), false);
        excludes.clear();
        ++version;
        check(QStringLiteral("bluest da"), false);
        index.clear();
        index.build({record(QStringLiteral("Bluest Dawn"), QStringLiteral("/music/dawn.ogg"))});
        check(QStringLiteral("bluest da"), false);
        check(QStringLiteral("bluest daw"), false, [] { return true; });
        check(QStringLiteral("bluest dawn"), false);

        // A background refresh moves a staging index into place; its records
        // must be scanned even though the query only narrowed.
        check(QStringLiteral("blu"), false);
        SearchIndex staging;
        staging.append({record(QStringLiteral("Blue Train"), QStringLiteral("/music/train.ogg"))});
        staging.append({record(QStringLiteral("Bluebird"), QStringLiteral("/music/bird.ogg"))});
        index = std::move(staging);
        staging.clear();
        check(QStringLiteral("blue"), false);
    }
};

QTEST_MAIN(TestNarrowingCache)
#include "test_narrowing_cache.moc"
