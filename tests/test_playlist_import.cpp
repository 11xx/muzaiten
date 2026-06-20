#include <QTest>

#include "playlist/PlaylistImport.h"
#include "playlist/PlaylistMatcher.h"
#include "playlist/import/YouTubeImport.h"
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
        // Short, specific track for the radio-edit-noise + coverage cases.
        records.append(makeRecord(QStringLiteral("Skinnyman"), QStringLiteral("Static-X"),
                                  QStringLiteral("Start A War"),
                                  QStringLiteral("/music/static-x/start-a-war/06 Skinnyman.flac"),
                                  3 * 60000 + 12000));
        // A long-title "magnet": its length makes it a fuzzy subsequence for almost
        // any short query, so the coverage guard must keep it from being claimed.
        records.append(makeRecord(
            QStringLiteral("Eugene Onegin Op 24 Act 3 Scene and Arioso Itak poidyom tebya predstavlyu ya Uzhel ta samaya Tatyana"),
            QStringLiteral("Dmitri Hvorostovsky Nuccia Focile Orchestre de Paris Semyon Bychkov"),
            QStringLiteral("Tchaikovsky Eugene Onegin"),
            QStringLiteral("/music/classical/eugene-onegin/13 act 3.flac"),
            6 * 60000));
        // Three versions of one track: a version tag in the source must pick the
        // right copy instead of collapsing to MultiMatch.
        records.append(makeRecord(QStringLiteral("My Hood (Full Mix)"), QStringLiteral("Suni Clay"),
                                  QStringLiteral("Suni Clay"),
                                  QStringLiteral("/music/suni/01 my hood full.flac"), 4 * 60000));
        records.append(makeRecord(QStringLiteral("My Hood (Instrumental)"), QStringLiteral("Suni Clay"),
                                  QStringLiteral("Suni Clay"),
                                  QStringLiteral("/music/suni/02 my hood inst.flac"), 4 * 60000));
        records.append(makeRecord(QStringLiteral("My Hood (Acapella Mix)"), QStringLiteral("Suni Clay"),
                                  QStringLiteral("Suni Clay"),
                                  QStringLiteral("/music/suni/03 my hood aca.flac"), 4 * 60000));
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

    void parse_jsonlItems()
    {
        ImportHeader header;
        const auto entries = parse(QStringLiteral(
            "{\"title\":\"So What\",\"artist\":\"Miles Davis\",\"album\":\"Kind of Blue\","
            "\"durationMs\":540000,\"externalId\":\"youtube:abc\",\"comment\":\"jazz\"}\n"
            "{\"directPath\":\"/music/x.flac\"}\n"), Format::Auto, &header);
        QVERIFY(!header.present);
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.first().title, QStringLiteral("So What"));
        QCOMPARE(entries.first().artist, QStringLiteral("Miles Davis"));
        QCOMPARE(entries.first().album, QStringLiteral("Kind of Blue"));
        QCOMPARE(entries.first().durationMs, qint64(540000));
        QCOMPARE(entries.first().externalId, QStringLiteral("youtube:abc"));
        QCOMPARE(entries.first().comment, QStringLiteral("jazz"));
        QCOMPARE(entries.at(1).directPath, QStringLiteral("/music/x.flac"));
    }

    void parse_jsonlHeaderAndTolerance()
    {
        ImportHeader header;
        const auto entries = parse(QStringLiteral(
            "{\"playlist\":{\"name\":\"Mix 2019\",\"comment\":\"from takeout\"}}\n"
            "\n"
            "# a comment line\n"
            "{\"title\":\"Karma Police\",\"artist\":\"Radiohead\",\"durationMs\":\"261000\",\"addedAt\":1604954896}\n"
            "{ this is not valid json }\n"
            "{\"comment\":\"no title or path\"}\n"
            "{\"title\":\"Hey Jude\",\"durationMs\":-5}\n"), Format::Auto, &header);
        QVERIFY(header.present);
        QCOMPARE(header.name, QStringLiteral("Mix 2019"));
        QCOMPARE(header.comment, QStringLiteral("from takeout"));
        // Blank, comment, malformed and the title/path-less line are all skipped.
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.first().title, QStringLiteral("Karma Police"));
        QCOMPARE(entries.first().durationMs, qint64(261000));  // numeric string coerced
        QCOMPARE(entries.first().addedAt, qint64(1604954896));
        QCOMPARE(entries.at(1).title, QStringLiteral("Hey Jude"));
        QCOMPARE(entries.at(1).durationMs, qint64(0));          // negative clamped
    }

    void parse_jsonlAddedAtNumericStringAndInvalidValues()
    {
        const auto entries = parse(QStringLiteral(
            "{\"title\":\"String timestamp\",\"addedAt\":\"1604954896\"}\n"
            "{\"title\":\"Zero\",\"addedAt\":0}\n"
            "{\"title\":\"Negative\",\"addedAt\":-1}\n"
            "{\"title\":\"Fraction\",\"addedAt\":1604954896.5}\n"
            "{\"title\":\"Malformed\",\"addedAt\":\"1604.954896\"}\n"
            "{\"title\":\"Out of range\",\"addedAt\":253402300800}\n"
            "{\"title\":\"Missing\"}\n"), Format::Jsonl);
        QCOMPARE(entries.size(), 7);
        QCOMPARE(entries.at(0).addedAt, qint64(1604954896));
        for (qsizetype i = 1; i < entries.size(); ++i) {
            QCOMPARE(entries.at(i).addedAt, qint64(0));
        }
    }

    void parse_jsonlHeaderOnlyWhenFirst()
    {
        // A "playlist" object that is not the first content line is treated as a
        // (skipped) item, not a header.
        ImportHeader header;
        const auto entries = parse(QStringLiteral(
            "{\"title\":\"So What\",\"artist\":\"Miles Davis\"}\n"
            "{\"playlist\":{\"name\":\"Late\"}}\n"), Format::Auto, &header);
        QVERIFY(!header.present);
        QCOMPARE(entries.size(), 1);
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

    void match_confidentHitIsMatched()
    {
        const auto outcome = PlaylistMatcher::match(m_index,
            parseLine(QStringLiteral("Miles Davis - So What")));
        QCOMPARE(outcome.decision, PlaylistMatcher::Decision::Matched);
        QVERIFY(outcome.confidence0To100 >= PlaylistMatcher::kMatchedConfidence);
    }

    void match_relaxedFallbackIsApproximate()
    {
        // No "Artist - Title" separator, and the artist words live in the artist
        // field, not the title — so the scoped query misses and only the relaxed
        // free-text fallback resolves it: a single auto-pick, flagged Approximate.
        const auto outcome = PlaylistMatcher::match(m_index,
            parseLine(QStringLiteral("Miles Davis So What")));
        QCOMPARE(outcome.decision, PlaylistMatcher::Decision::Approximate);
        QCOMPARE(outcome.best.title, QStringLiteral("So What"));
        QVERIFY(outcome.confidence0To100 > 0
                && outcome.confidence0To100 < PlaylistMatcher::kMatchedConfidence);
    }

    void match_radioEditNoiseStripped()
    {
        // "(Clean Album Version) (Clean)" tags must not become title tokens — if
        // they do, the real track is excluded (no such subsequence) and a long
        // magnet that does contain them wins. After stripping, Skinnyman matches.
        const auto outcome = PlaylistMatcher::match(m_index,
            parseLine(QStringLiteral("Static-X - Skinnyman (Clean Album Version) (Clean)")));
        QCOMPARE(outcome.decision, PlaylistMatcher::Decision::Matched);
        QCOMPARE(outcome.best.path,
                 QStringLiteral("/music/static-x/start-a-war/06 Skinnyman.flac"));
    }

    void match_longFieldMagnetNotClaimed()
    {
        // A short query that only a long title could fuzzily subsequence must never
        // be auto-matched to that magnet — better Pending than a confident-wrong
        // green. (This is the Eugene-Onegin false positive the coverage guard kills.)
        ImportEntry entry;
        entry.artist = QStringLiteral("Aria");
        entry.title = QStringLiteral("Stan");
        const auto outcome = PlaylistMatcher::match(m_index, entry);
        QVERIFY(outcome.best.path != QStringLiteral("/music/classical/eugene-onegin/13 act 3.flac"));
        QVERIFY(outcome.decision != PlaylistMatcher::Decision::Matched);
    }

    void match_versionDiscriminatorPicksRightCopy()
    {
        // The library holds three "My Hood" versions; the source carries "(Full
        // Mix)". Trying the full title before the noise-stripped one must pick the
        // Full Mix copy rather than collapse to MultiMatch.
        ImportEntry entry;
        entry.artist = QStringLiteral("Suni Clay");
        entry.title = QStringLiteral("My Hood (Full Mix)");
        const auto outcome = PlaylistMatcher::match(m_index, entry);
        QCOMPARE(outcome.decision, PlaylistMatcher::Decision::Matched);
        QCOMPARE(outcome.best.path, QStringLiteral("/music/suni/01 my hood full.flac"));
    }

    void match_ambiguousVersionsStayMulti()
    {
        // Without a discriminator, the three versions are genuinely ambiguous.
        ImportEntry entry;
        entry.artist = QStringLiteral("Suni Clay");
        entry.title = QStringLiteral("My Hood");
        const auto outcome = PlaylistMatcher::match(m_index, entry);
        QCOMPARE(outcome.decision, PlaylistMatcher::Decision::MultiMatch);
        QVERIFY(outcome.candidatePaths.size() >= 2);
    }

    void match_directPathWins()
    {
        ImportEntry entry;
        entry.directPath = QStringLiteral("/music/radiohead/ok-computer/06 Karma Police.flac");
        const auto outcome = PlaylistMatcher::match(m_index, entry);
        QCOMPARE(outcome.decision, PlaylistMatcher::Decision::Matched);
        QCOMPARE(outcome.best.title, QStringLiteral("Karma Police"));
    }

    // ---- youtube adapter (offline json parsing) ----------------------------

    void youtube_entriesFromJson()
    {
        const QByteArray json = R"({
            "title": "Jazz Mix",
            "entries": [
                {"id": "abc123", "title": "So What", "channel": "Miles Davis - Topic", "duration": 540.0},
                {"id": "def456", "title": "Radiohead - Karma Police", "channel": "someuser", "duration": 261},
                {"id": "gone", "title": "[Deleted video]", "channel": "", "duration": null}
            ]
        })";
        QString title;
        const auto entries = YouTubePlaylistFetcher::entriesFromJson(json, &title);
        QCOMPARE(title, QStringLiteral("Jazz Mix"));
        QCOMPARE(entries.size(), 2);  // deleted video dropped
        // YT Music style: bare title, artist from the "- Topic" channel.
        QCOMPARE(entries.at(0).title, QStringLiteral("So What"));
        QCOMPARE(entries.at(0).artist, QStringLiteral("Miles Davis"));
        QCOMPARE(entries.at(0).durationMs, qint64(540000));
        QCOMPARE(entries.at(0).externalId, QStringLiteral("abc123"));
        // Plain YouTube style: "Artist - Title" in the video title wins.
        QCOMPARE(entries.at(1).artist, QStringLiteral("Radiohead"));
        QCOMPARE(entries.at(1).title, QStringLiteral("Karma Police"));
    }

    void youtube_urlDetection()
    {
        QVERIFY(YouTubePlaylistFetcher::looksLikePlaylistUrl(
            QStringLiteral("https://music.youtube.com/playlist?list=PLabc")));
        QVERIFY(YouTubePlaylistFetcher::looksLikePlaylistUrl(
            QStringLiteral("https://www.youtube.com/watch?v=x&list=PLabc")));
        QVERIFY(!YouTubePlaylistFetcher::looksLikePlaylistUrl(
            QStringLiteral("https://www.youtube.com/watch?v=x")));
        QVERIFY(!YouTubePlaylistFetcher::looksLikePlaylistUrl(
            QStringLiteral("https://example.com/playlist?list=PLabc")));
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
