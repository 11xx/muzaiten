#include <QTest>

#include "playlist/PlaylistImport.h"
#include "playlist/PlaylistMatcher.h"
#include "search/SearchIndex.h"
#include "search/SearchRecord.h"

using namespace PlaylistImport;

static Search::SearchRecord makeRecord(const QString &title,
                                       const QString &artist,
                                       const QString &album,
                                       const QString &path,
                                       qint64 durationMs = 0)
{
    Search::SearchRecord r;
    r.title           = title;
    r.artistName      = artist;
    r.albumArtistName = artist;
    r.albumTitle      = album;
    r.filename        = path.section(QLatin1Char('/'), -1);
    r.path            = path;
    r.durationMs      = durationMs;
    r.normTitle       = title.toLower();
    r.normArtist      = artist.toLower();
    r.normAlbumArtist = artist.toLower();
    r.normAlbum       = album.toLower();
    r.normFilename    = r.filename.toLower();
    r.normPath        = path.toLower();
    return r;
}

class TestPlaylistImport : public QObject {
    Q_OBJECT

private:
    Search::SearchIndex m_index;

private slots:
    void initTestCase()
    {
        QVector<Search::SearchRecord> records;
        records.append(makeRecord(QStringLiteral("So What"), QStringLiteral("Miles Davis"),
                                  QStringLiteral("Kind of Blue"),
                                  QStringLiteral("/music/miles/kind-of-blue/01 So What.flac"),
                                  9 * 60000));
        records.append(makeRecord(QStringLiteral("Blue in Green"), QStringLiteral("Miles Davis"),
                                  QStringLiteral("Kind of Blue"),
                                  QStringLiteral("/music/miles/kind-of-blue/03 Blue in Green.flac"),
                                  5 * 60000 + 37000));
        // Same song twice (different albums) → MultiMatch unless a tiebreaker decides.
        records.append(makeRecord(QStringLiteral("Hey Jude"), QStringLiteral("The Beatles"),
                                  QStringLiteral("Past Masters"),
                                  QStringLiteral("/music/beatles/past-masters/hey jude.flac"),
                                  7 * 60000 + 11000));
        records.append(makeRecord(QStringLiteral("Hey Jude"), QStringLiteral("The Beatles"),
                                  QStringLiteral("1967-1970"),
                                  QStringLiteral("/music/beatles/1967-1970/hey jude.mp3"),
                                  7 * 60000 + 5000));
        records.append(makeRecord(QStringLiteral("Karma Police"), QStringLiteral("Radiohead"),
                                  QStringLiteral("OK Computer"),
                                  QStringLiteral("/music/radiohead/ok-computer/06 Karma Police.flac"),
                                  4 * 60000 + 21000));
        m_index.build(records);
    }

    // ---- parser ------------------------------------------------------------

    void parseLine_artistTitle()
    {
        const ImportEntry e = parseLine(QStringLiteral("Miles Davis - So What"));
        QCOMPARE(e.artist, QStringLiteral("Miles Davis"));
        QCOMPARE(e.title, QStringLiteral("So What"));
    }

    void parseLine_numberedWithDuration()
    {
        const ImportEntry e = parseLine(QStringLiteral("07. Radiohead - Karma Police (4:21)"));
        QCOMPARE(e.artist, QStringLiteral("Radiohead"));
        QCOMPARE(e.title, QStringLiteral("Karma Police"));
    }

    void parseLine_enDash()
    {
        const ImportEntry e = parseLine(QStringLiteral("The Beatles – Hey Jude"));
        QCOMPARE(e.artist, QStringLiteral("The Beatles"));
        QCOMPARE(e.title, QStringLiteral("Hey Jude"));
    }

    void parseLine_noSeparator()
    {
        const ImportEntry e = parseLine(QStringLiteral("Karma Police"));
        QVERIFY(e.artist.isEmpty());
        QCOMPARE(e.title, QStringLiteral("Karma Police"));
    }

    void parse_plainTextSkipsBlanksAndComments()
    {
        const auto entries = parse(QStringLiteral(
            "Miles Davis - So What\n\n# a comment\nRadiohead - Karma Police\n"));
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.at(1).title, QStringLiteral("Karma Police"));
    }

    void parse_m3uExtinf()
    {
        const auto entries = parse(QStringLiteral(
            "#EXTM3U\n"
            "#EXTINF:540,Miles Davis - So What\n"
            "/music/miles/kind-of-blue/01 So What.flac\n"
            "#EXTINF:261,Radiohead - Karma Police\n"
            "/elsewhere/karma.flac\n"));
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.first().artist, QStringLiteral("Miles Davis"));
        QCOMPARE(entries.first().directPath,
                 QStringLiteral("/music/miles/kind-of-blue/01 So What.flac"));
        QCOMPARE(entries.first().durationMs, qint64(540000));
        QCOMPARE(entries.at(1).title, QStringLiteral("Karma Police"));
    }

    void parse_csvOurExport()
    {
        const auto entries = parse(QStringLiteral(
            "ordinal,title,artist,album,duration_ms,path,status,query,comment\n"
            "1,\"So What\",\"Miles Davis\",\"Kind of Blue\",540000,\"/music/x.flac\",\"matched\",\"\",\"\"\n"));
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().title, QStringLiteral("So What"));
        QCOMPARE(entries.first().album, QStringLiteral("Kind of Blue"));
        QCOMPARE(entries.first().durationMs, qint64(540000));
        QCOMPARE(entries.first().directPath, QStringLiteral("/music/x.flac"));
    }

    void parse_csvForeignHeader()
    {
        const auto entries = parse(QStringLiteral(
            "Track name,Artist name,Album,Duration\n"
            "Karma Police,Radiohead,OK Computer,261\n"));
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().artist, QStringLiteral("Radiohead"));
        QCOMPARE(entries.first().durationMs, qint64(261000));  // seconds heuristic
    }

    void stripTitleNoise_packaging()
    {
        QCOMPARE(stripTitleNoise(QStringLiteral("Karma Police (Official Video) [Remastered 2017]")),
                 QStringLiteral("Karma Police"));
        QCOMPARE(stripTitleNoise(QStringLiteral("Hey Jude feat. Nobody")),
                 QStringLiteral("Hey Jude"));
        // Meaningful parentheses survive.
        QCOMPARE(stripTitleNoise(QStringLiteral("Time (Pink Floyd)")),
                 QStringLiteral("Time (Pink Floyd)"));
    }

    void normalizeForMatch_strips()
    {
        QCOMPARE(normalizeForMatch(QStringLiteral("  Beyoncé — HALO!! ")),
                 QStringLiteral("beyonce halo"));
    }

    // ---- matcher -----------------------------------------------------------

    void match_uniqueTitleArtist()
    {
        const auto outcome = PlaylistMatcher::match(m_index,
            parseLine(QStringLiteral("Miles Davis - So What")));
        QCOMPARE(outcome.decision, PlaylistMatcher::Decision::Matched);
        QCOMPARE(outcome.best.path,
                 QStringLiteral("/music/miles/kind-of-blue/01 So What.flac"));
        QVERIFY(!outcome.queryUsed.isEmpty());
    }

    void match_noisyTitleStillMatches()
    {
        const auto outcome = PlaylistMatcher::match(m_index,
            parseLine(QStringLiteral("Radiohead - Karma Police (Official Video)")));
        QCOMPARE(outcome.decision, PlaylistMatcher::Decision::Matched);
        QCOMPARE(outcome.best.title, QStringLiteral("Karma Police"));
    }

    void match_duplicateTrackIsMultiMatch()
    {
        const auto outcome = PlaylistMatcher::match(m_index,
            parseLine(QStringLiteral("The Beatles - Hey Jude")));
        QCOMPARE(outcome.decision, PlaylistMatcher::Decision::MultiMatch);
        QCOMPARE(outcome.candidatePaths.size(), 2);
    }

    void match_albumTiebreakerResolvesDuplicate()
    {
        ImportEntry entry = parseLine(QStringLiteral("The Beatles - Hey Jude"));
        entry.album = QStringLiteral("Past Masters");
        const auto outcome = PlaylistMatcher::match(m_index, entry);
        QCOMPARE(outcome.decision, PlaylistMatcher::Decision::Matched);
        QCOMPARE(outcome.best.path,
                 QStringLiteral("/music/beatles/past-masters/hey jude.flac"));
    }

    void match_unknownIsPending()
    {
        const auto outcome = PlaylistMatcher::match(m_index,
            parseLine(QStringLiteral("Aphex Twin - Windowlicker")));
        QCOMPARE(outcome.decision, PlaylistMatcher::Decision::Pending);
        QVERIFY(!outcome.queryUsed.isEmpty());  // edit modal can still re-run it
    }

    void match_directPathWins()
    {
        ImportEntry entry;
        entry.directPath = QStringLiteral("/music/radiohead/ok-computer/06 Karma Police.flac");
        const auto outcome = PlaylistMatcher::match(m_index, entry);
        QCOMPARE(outcome.decision, PlaylistMatcher::Decision::Matched);
        QCOMPARE(outcome.best.title, QStringLiteral("Karma Police"));
    }

    void match_foreignPathFallsBackToBasename()
    {
        ImportEntry entry = parseLine(QStringLiteral("Radiohead - Karma Police"));
        entry.directPath = QStringLiteral("C:/Users/other/Music/06 Karma Police.flac");
        const auto outcome = PlaylistMatcher::match(m_index, entry);
        QCOMPARE(outcome.decision, PlaylistMatcher::Decision::Matched);
        QCOMPARE(outcome.best.path,
                 QStringLiteral("/music/radiohead/ok-computer/06 Karma Police.flac"));
    }
};

QTEST_MAIN(TestPlaylistImport)

#include "test_playlist_import.moc"
