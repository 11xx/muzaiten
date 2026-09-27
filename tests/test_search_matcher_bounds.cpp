#include "search/SearchIndex.h"
#include "search/SearchMatcher.h"
#include "search/SearchRecord.h"

#include <QTest>
#include <climits>
#include <random>

using namespace Search;

class SearchMatcherBoundsTest : public QObject {
    Q_OBJECT
private slots:
    void masksAndIndex();
};

void SearchMatcherBoundsTest::masksAndIndex()
{
    QVector<SearchRecord> records;
    const QStringList artists = {QStringLiteral("amber river"), QStringLiteral("Борис Гребенщиков"),
                                 QStringLiteral("Їжак Єнот"), QStringLiteral("東京 雨"),
                                 QStringLiteral("alpha.beta"), QStringLiteral("résumé echo")};
    const QStringList titles = {QStringLiteral("ab"), QStringLiteral("blue moon"),
                                QStringLiteral("a/b + c"), QStringLiteral("Москва night"),
                                QStringLiteral("星の歌"), QStringLiteral("green 42")};
    for (int i = 0; i < 2600; ++i) {
        SearchRecord rec;
        rec.title = titles[i % titles.size()] + QStringLiteral(" ") + QString::number(i);
        rec.artistName = artists[(i / 3) % artists.size()];
        rec.albumArtistName = artists[(i / 5) % artists.size()];
        rec.albumTitle = titles[(i / 7) % titles.size()];
        rec.filename = QString::number(i) + QStringLiteral("-ab.flac");
        rec.path = QStringLiteral("/music/") + rec.artistName + QLatin1Char('/') + rec.filename;
        if (i % 11 == 0) rec.titleSort = QStringLiteral("other reading");
        if (i % 13 == 0) rec.artistSort = QStringLiteral("ukrainian reading");
        foldRecordNorms(rec);
        records.push_back(std::move(rec));
    }
    // A match across separate readings must not pass fuzzy membership.
    SearchRecord split;
    split.title = QStringLiteral("a");
    split.titleSort = QStringLiteral("b");
    foldRecordNorms(split);
    records.push_back(split);

    QStringList queries = {QStringLiteral("ab"), QStringLiteral("^ab"), QStringLiteral("'ab"),
                           QStringLiteral("!ab"), QStringLiteral("ab$"), QStringLiteral("a"),
                           QStringLiteral("4"), QStringLiteral("title:ab"), QStringLiteral("artist:ї"),
                           QStringLiteral("album:blue"), QStringLiteral("path:music"),
                           QStringLiteral("file:ab"), QStringLiteral("albumartist:river"),
                           QStringLiteral("ab river"), QStringLiteral("a !river"),
                           QStringLiteral("title:ab$"), QStringLiteral("title:ab artist:river")};
    std::mt19937 rng(20260927);
    for (int i = 0; i < 140; ++i) {
        const auto &rec = records[static_cast<int>(rng() % records.size())];
        const QStringList fields = {rec.normTitle, rec.normArtist, rec.normAlbum, rec.normPath};
        const QString field = fields[static_cast<int>(rng() % fields.size())].split(Fold::kSegmentSeparator).first();
        if (field.isEmpty()) continue;
        const int start = static_cast<int>(rng() % field.size());
        QString part = field.mid(start, 1 + static_cast<int>(rng() % 5)).trimmed();
        if (part.isEmpty()) continue;
        queries.append(part);
        if (part.size() > 2) { part.remove(1, 1); queries.append(part); }
    }
    SearchIndex built, appended, rebuilt;
    built.build(records);
    for (int i = 0; i < records.size(); i += 73) appended.append(records.mid(i, 73));
    rebuilt.build(records.mid(0, 2));
    rebuilt.clear();
    rebuilt.build(records);
    for (const QString &text : queries) {
        const SearchQuery query = SearchQuery::parse(text);
        const auto termMasks = termCharacterMasks(query);
        for (bool fuzzy : {false, true}) {
            for (const SearchRecord &rec : records) {
                const auto masks = recordNormMasks(rec);
                bool exact = false;
                const int bound = matchSearchRecordBound(rec, masks, query, termMasks, fuzzy, &exact);
                const int truth = matchSearchRecord(rec, query, fuzzy);
                QCOMPARE(bound == INT_MIN, truth == INT_MIN);
                if (truth != INT_MIN) {
                    QVERIFY2(bound >= truth, qPrintable(text));
                    if (exact) QCOMPARE(bound, truth);
                }
                QCOMPARE(scoreSearchRecord(rec, masks, query, termMasks, fuzzy), truth);
            }
            int builtCount = 0, appendCount = 0, rebuildCount = 0;
            const auto a = built.match(query, fuzzy, {}, &builtCount);
            const auto b = appended.match(query, fuzzy, {}, &appendCount);
            const auto c = rebuilt.match(query, fuzzy, {}, &rebuildCount);
            QCOMPARE(appendCount, builtCount);
            QCOMPARE(rebuildCount, builtCount);
            QCOMPARE(b.size(), a.size());
            QCOMPARE(c.size(), a.size());
            for (int i = 0; i < a.size(); ++i) {
                QCOMPARE(b[i].index, a[i].index);
                QCOMPARE(c[i].index, a[i].index);
                QCOMPARE(b[i].score, a[i].score);
                QCOMPARE(c[i].score, a[i].score);
            }
        }
    }
    SearchRecord longRecord;
    longRecord.title = QString(1'048'577, QLatin1Char('a'));
    foldRecordNorms(longRecord);
    const SearchQuery longQuery = SearchQuery::parse(longRecord.title);
    const auto longMasks = recordNormMasks(longRecord);
    const auto longTermMasks = termCharacterMasks(longQuery);
    bool longExact = false;
    const int longBound = matchSearchRecordBound(longRecord, longMasks, longQuery,
                                                  longTermMasks, true, &longExact);
    const int longScore = matchSearchRecord(longRecord, longQuery, true);
    QVERIFY(longScore != INT_MIN);
    QVERIFY(longBound >= longScore);
    QCOMPARE(scoreSearchRecord(longRecord, longMasks, longQuery, longTermMasks, true), longScore);
    QVERIFY(normCharacterMask(split.normTitle) & (quint64{1} << 63));
}

QTEST_GUILESS_MAIN(SearchMatcherBoundsTest)
#include "test_search_matcher_bounds.moc"
