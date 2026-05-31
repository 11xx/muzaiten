#include <QTest>

#include "search/RankConfig.h"
#include "search/ResultRanker.h"
#include "search/SearchIndex.h"
#include "search/SearchRecord.h"

using namespace Search;

static SearchRecord makeRec(const QString &title, const QString &codec, int sampleRate,
                            int bitrate, const QString &path = {}, const QString &artist = {})
{
    SearchRecord r;
    r.title = title;
    r.codec = codec;
    r.sampleRateHz = sampleRate;
    r.bitrateKbps = bitrate;
    r.channels = 2;
    r.artistName = artist;
    r.albumArtistName = artist;
    r.path = path;
    r.normTitle = title.toLower();
    r.normArtist = artist.toLower();
    r.normAlbumArtist = artist.toLower();
    r.normPath = path.toLower();
    return r;
}

static ScoredResult sr(const SearchRecord &rec, int score)
{
    ScoredResult s;
    s.rec = rec;
    s.score = score;
    return s;
}

static RankRule rule(RankKind kind, MusicSort::SortDirection dir = MusicSort::SortDirection::Descending,
                     MusicSort::SortField field = MusicSort::SortField::AlbumArtist,
                     const QString &param = {})
{
    return RankRule{kind, field, param, dir, true};
}

class TestResultRanker : public QObject {
    Q_OBJECT

private slots:
    void qualityScoreOrdering()
    {
        const int flac96 = audioQualityScore(makeRec("a", "flac", 96000, 0));
        const int flac44 = audioQualityScore(makeRec("a", "flac", 44100, 0));
        const int mp3_320 = audioQualityScore(makeRec("a", "mp3", 44100, 320));
        const int mp3_128 = audioQualityScore(makeRec("a", "mp3", 44100, 128));
        QVERIFY(flac96 > flac44);
        QVERIFY(flac44 > mp3_320);
        QVERIFY(mp3_320 > mp3_128);
    }

    void relevancePrimaryQualityTiebreak()
    {
        // Same relevance; quality should decide. Default order: Relevance, Quality.
        QVector<ScoredResult> results = {
            sr(makeRec("Song A", "mp3", 44100, 320), 100),
            sr(makeRec("Song B", "flac", 96000, 0), 100),
        };
        RankConfig cfg;
        cfg.rules = {rule(RankKind::Relevance), rule(RankKind::AudioQuality)};
        ResultRanker(cfg).sort(results);
        QCOMPARE(results[0].rec.title, QStringLiteral("Song B")); // hi-res first on the tie
    }

    void relevanceStillWinsWhenAbove()
    {
        // Different relevance with Relevance on top: relevance order preserved.
        QVector<ScoredResult> results = {
            sr(makeRec("Low rel hi-res", "flac", 96000, 0), 50),
            sr(makeRec("High rel lossy", "mp3", 44100, 128), 500),
        };
        RankConfig cfg;
        cfg.rules = {rule(RankKind::Relevance), rule(RankKind::AudioQuality)};
        ResultRanker(cfg).sort(results);
        QCOMPARE(results[0].rec.title, QStringLiteral("High rel lossy"));
    }

    void qualityAboveRelevanceReorders()
    {
        QVector<ScoredResult> results = {
            sr(makeRec("Low rel hi-res", "flac", 96000, 0), 50),
            sr(makeRec("High rel lossy", "mp3", 44100, 128), 500),
        };
        RankConfig cfg;
        cfg.rules = {rule(RankKind::AudioQuality), rule(RankKind::Relevance)};
        ResultRanker(cfg).sort(results);
        QCOMPARE(results[0].rec.title, QStringLiteral("Low rel hi-res"));
    }

    void preferredDirectoryBoost()
    {
        QVector<ScoredResult> results = {
            sr(makeRec("Outside", "flac", 96000, 0, QStringLiteral("/gak/other/a.flac")), 100),
            sr(makeRec("Inside",  "mp3",  44100, 128, QStringLiteral("/gak/fav/b.mp3")), 100),
        };
        RankConfig cfg;
        cfg.rules = {rule(RankKind::PreferredDirectory, MusicSort::SortDirection::Descending,
                          MusicSort::SortField::AlbumArtist, QStringLiteral("/gak/fav"))};
        ResultRanker(cfg).sort(results);
        QCOMPARE(results[0].rec.title, QStringLiteral("Inside"));
    }

    void musicFieldArtistSort()
    {
        QVector<ScoredResult> results = {
            sr(makeRec("t1", "flac", 44100, 0, {}, QStringLiteral("Zappa")), 100),
            sr(makeRec("t2", "flac", 44100, 0, {}, QStringLiteral("Adele")), 100),
        };
        RankConfig cfg;
        cfg.rules = {rule(RankKind::MusicField, MusicSort::SortDirection::Ascending,
                          MusicSort::SortField::Artist)};
        ResultRanker(cfg).sort(results);
        QCOMPARE(results[0].rec.artistName, QStringLiteral("Adele")); // A before Z ascending
    }

    void emptyRulesKeepsRelevanceOrder()
    {
        QVector<ScoredResult> results = {
            sr(makeRec("First", "mp3", 44100, 128), 500),
            sr(makeRec("Second", "flac", 96000, 0), 50),
        };
        RankConfig cfg; // no rules
        ResultRanker(cfg).sort(results);
        QCOMPARE(results[0].rec.title, QStringLiteral("First"));
    }
};

QTEST_MAIN(TestResultRanker)
#include "test_result_ranker.moc"
