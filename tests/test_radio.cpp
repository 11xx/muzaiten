#include "core/GenreTags.h"
#include "core/MetadataBlob.h"
#include "db/Database.h"
#include "reco/RadioSession.h"
#include "reco/TrackScorer.h"
#include "scrobble/ListenHistoryStore.h"

#include <QRandomGenerator>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include <cmath>

namespace {

TrackScorer::Candidate makeCandidate(const QString &path, const QString &artistFolded,
                                     const QStringList &genresFolded, int year = 0,
                                     int rating = -1, bool hasUserRating = false,
                                     const QString &albumKey = {})
{
    TrackScorer::Candidate candidate;
    candidate.path = path;
    candidate.artistFolded = artistFolded;
    candidate.albumKey = albumKey.isEmpty() ? (artistFolded + QLatin1String("\nalbum")) : albumKey;
    candidate.genresFolded = genresFolded;
    candidate.year = year;
    candidate.effectiveRating0To100 = rating;
    candidate.hasUserRating = hasUserRating;
    return candidate;
}

double componentValue(const TrackScorer::Scored &scored, const QString &name)
{
    for (const TrackScorer::Component &component : scored.components) {
        if (component.name == name) {
            return component.value;
        }
    }
    return 0.0;
}

bool hasComponent(const TrackScorer::Scored &scored, const QString &name)
{
    for (const TrackScorer::Component &component : scored.components) {
        if (component.name == name) {
            return true;
        }
    }
    return false;
}

Track resolvePathToTrack(const QString &path)
{
    Track track;
    track.path = path;
    track.title = path;
    return track;
}

} // namespace

class RadioTest final : public QObject {
    Q_OBJECT

private slots:
    // TrackScorer
    void genreOverlapDominates();
    void genreIdfSaturatesOnRareSharedGenre();
    void genreIdfPartialOnBroadSharedGenre();
    void genreAbsentWithNoSharedGenres();
    void genreAbsentWithEmptyIdfMap();
    void eraDecaysWithYearGap();
    void skipPenaltyScalesWithSkipRate();
    void recencyPenaltyDecaysWithTime();
    void noveltyAtZeroHistoryScalesWithExploration();
    void componentsSumAndSignsMatchScore();
    void unratedAndUnknownYearYieldNoComponent();

    // RadioSession
    void artistThrottleNeverPicksSameArtistConsecutively();
    void albumCapLimitsTracksPerAlbum();
    void noRepeatsWithinSession();
    void excludePathsAreRespected();
    void rollingContextDriftsGenreWindow();
    void reasonForNonEmptyOnPick();

    // Database + ListenHistoryStore round-trips
    void radioCandidatesJoinsGenresAndFallback();
    void genreTrackCountsAggregatesAcrossLibrary();
    void trackAffinitiesAggregateAllSources();

    // GenreTags
    void nonGenrePlaceholdersAreStoplisted();
    void informativeFiltersStoplistedGenres();
};

// ---- TrackScorer -----------------------------------------------------------

void RadioTest::genreOverlapDominates()
{
    TrackScorer::SeedContext seed;
    seed.genresFolded = {QStringLiteral("dream pop"), QStringLiteral("shoegaze")};
    seed.genreIdf = {{QStringLiteral("dream pop"), 2.0}, {QStringLiteral("shoegaze"), 2.5}};

    const TrackScorer::Scored match = TrackScorer::score(
        makeCandidate(QStringLiteral("/m"), QStringLiteral("a"),
                      {QStringLiteral("dream pop"), QStringLiteral("shoegaze")}),
        {}, seed);
    const TrackScorer::Scored miss = TrackScorer::score(
        makeCandidate(QStringLiteral("/x"), QStringLiteral("b"), {QStringLiteral("techno")}),
        {}, seed);

    QVERIFY(hasComponent(match, QStringLiteral("genre")));
    QVERIFY(!hasComponent(miss, QStringLiteral("genre")));  // no overlap, no floor
    QVERIFY(componentValue(match, QStringLiteral("genre")) > 0.0);
    QVERIFY(match.score > miss.score);
}

void RadioTest::genreIdfSaturatesOnRareSharedGenre()
{
    // A single very rare shared genre (idf well past the 4.0 saturation point)
    // should saturate the genre component to 1.0 pre-weight: 3.0 (weight) *
    // 1.0 * 1.10 (exploration scale at the default exploration=30).
    TrackScorer::SeedContext seed;
    seed.genresFolded = {QStringLiteral("noise")};
    seed.genreIdf = {{QStringLiteral("noise"), 6.0}};  // far above the 4.0 saturation point

    const TrackScorer::Scored scored = TrackScorer::score(
        makeCandidate(QStringLiteral("/m"), QStringLiteral("a"), {QStringLiteral("noise")}), {}, seed);

    QVERIFY(qFuzzyCompare(componentValue(scored, QStringLiteral("genre")), 3.0 * 1.0 * 1.10));
}

void RadioTest::genreIdfPartialOnBroadSharedGenre()
{
    // A single broad shared genre (idf ~= 1.8, e.g. "rock") should land well
    // short of saturation: genreScore = 1.8 / 4.0 = 0.45, real but weak.
    TrackScorer::SeedContext seed;
    seed.genresFolded = {QStringLiteral("rock")};
    seed.genreIdf = {{QStringLiteral("rock"), 1.8}};

    const TrackScorer::Scored scored = TrackScorer::score(
        makeCandidate(QStringLiteral("/m"), QStringLiteral("a"), {QStringLiteral("rock")}), {}, seed);

    QVERIFY(qFuzzyCompare(componentValue(scored, QStringLiteral("genre")), 3.0 * 0.45 * 1.10));
}

void RadioTest::genreAbsentWithNoSharedGenres()
{
    TrackScorer::SeedContext seed;
    seed.genresFolded = {QStringLiteral("rock")};
    seed.genreIdf = {{QStringLiteral("rock"), 2.0}};

    const TrackScorer::Scored scored = TrackScorer::score(
        makeCandidate(QStringLiteral("/m"), QStringLiteral("a"), {QStringLiteral("jazz")}), {}, seed);

    QVERIFY(!hasComponent(scored, QStringLiteral("genre")));
}

void RadioTest::genreAbsentWithEmptyIdfMap()
{
    // Full genre overlap, but an empty idf map (the SeedContext default) means
    // every genre resolves to IDF 0 — the component must not appear.
    TrackScorer::SeedContext seed;
    seed.genresFolded = {QStringLiteral("rock")};
    QVERIFY(seed.genreIdf.isEmpty());

    const TrackScorer::Scored scored = TrackScorer::score(
        makeCandidate(QStringLiteral("/m"), QStringLiteral("a"), {QStringLiteral("rock")}), {}, seed);

    QVERIFY(!hasComponent(scored, QStringLiteral("genre")));
}

void RadioTest::eraDecaysWithYearGap()
{
    TrackScorer::SeedContext seed;
    seed.year = 2000;

    const double same = componentValue(
        TrackScorer::score(makeCandidate(QStringLiteral("/a"), QStringLiteral("a"), {}, 2000), {}, seed),
        QStringLiteral("era"));
    const double near = componentValue(
        TrackScorer::score(makeCandidate(QStringLiteral("/b"), QStringLiteral("b"), {}, 2015), {}, seed),
        QStringLiteral("era"));
    const TrackScorer::Scored farScored =
        TrackScorer::score(makeCandidate(QStringLiteral("/c"), QStringLiteral("c"), {}, 2031), {}, seed);

    QVERIFY(qFuzzyCompare(same, 1.0));
    QVERIFY(qFuzzyCompare(near, 0.5));
    QVERIFY(!hasComponent(farScored, QStringLiteral("era")));  // capped at 30 years → 0
}

void RadioTest::skipPenaltyScalesWithSkipRate()
{
    TrackScorer::SeedContext seed;
    TrackScorer::Affinity affinity;
    affinity.playEvents = 10;
    affinity.skipped = 5;

    // Smoothed rate: skipped / (playEvents + 2), so evidence tempers the blow.
    const TrackScorer::Scored scored =
        TrackScorer::score(makeCandidate(QStringLiteral("/a"), QStringLiteral("a"), {}), affinity, seed);
    QVERIFY(qFuzzyCompare(componentValue(scored, QStringLiteral("skips")), -2.5 * (5.0 / 12.0)));

    // A lone "not right now" skip on a barely-played track is a light touch,
    // not the maximum penalty the raw ratio would give.
    TrackScorer::Affinity lone;
    lone.playEvents = 1;
    lone.skipped = 1;
    const TrackScorer::Scored loneScored =
        TrackScorer::score(makeCandidate(QStringLiteral("/b"), QStringLiteral("b"), {}), lone, seed);
    QVERIFY(qFuzzyCompare(componentValue(loneScored, QStringLiteral("skips")), -2.5 * (1.0 / 3.0)));
}

void RadioTest::recencyPenaltyDecaysWithTime()
{
    TrackScorer::SeedContext seed;
    seed.nowSecs = 1'000'000'000;

    TrackScorer::Affinity justPlayed;
    justPlayed.lastPlayedAtSecs = seed.nowSecs;  // 0 days ago
    TrackScorer::Affinity twoWeeks;
    twoWeeks.lastPlayedAtSecs = seed.nowSecs - 14 * 86400;

    const double recent = componentValue(
        TrackScorer::score(makeCandidate(QStringLiteral("/a"), QStringLiteral("a"), {}), justPlayed, seed),
        QStringLiteral("recency"));
    const double older = componentValue(
        TrackScorer::score(makeCandidate(QStringLiteral("/b"), QStringLiteral("b"), {}), twoWeeks, seed),
        QStringLiteral("recency"));

    QVERIFY(recent < 0.0);
    QVERIFY(older < 0.0);
    QVERIFY(recent < older);  // more negative when played more recently
    QVERIFY(qFuzzyCompare(recent, -2.0));
}

void RadioTest::noveltyAtZeroHistoryScalesWithExploration()
{
    TrackScorer::SeedContext conservative;
    conservative.exploration0To100 = 0;
    TrackScorer::SeedContext exploratory;
    exploratory.exploration0To100 = 100;

    const double low = componentValue(
        TrackScorer::score(makeCandidate(QStringLiteral("/a"), QStringLiteral("a"), {}), {}, conservative),
        QStringLiteral("novelty"));
    const double high = componentValue(
        TrackScorer::score(makeCandidate(QStringLiteral("/a"), QStringLiteral("a"), {}), {}, exploratory),
        QStringLiteral("novelty"));

    QVERIFY(qFuzzyCompare(low, 0.8 * 1.0 * 0.5));   // (0.5 + 0/100)
    QVERIFY(qFuzzyCompare(high, 0.8 * 1.0 * 1.5));  // (0.5 + 100/100)
    QVERIFY(high > low);

    // Novelty fully decays once a track has been heard enough.
    TrackScorer::Affinity heard;
    heard.playEvents = 12;
    const TrackScorer::Scored scored =
        TrackScorer::score(makeCandidate(QStringLiteral("/a"), QStringLiteral("a"), {}), heard, exploratory);
    QVERIFY(!hasComponent(scored, QStringLiteral("novelty")));
}

void RadioTest::componentsSumAndSignsMatchScore()
{
    TrackScorer::SeedContext seed;
    seed.genresFolded = {QStringLiteral("rock")};
    seed.genreIdf = {{QStringLiteral("rock"), 1.8}};
    seed.recentArtistsFolded = {QStringLiteral("a")};
    TrackScorer::Affinity affinity;
    affinity.playEvents = 4;
    affinity.skipped = 2;

    const TrackScorer::Scored scored = TrackScorer::score(
        makeCandidate(QStringLiteral("/a"), QStringLiteral("a"), {QStringLiteral("rock")}), affinity, seed);

    double sum = 0.0;
    for (const TrackScorer::Component &component : scored.components) {
        sum += component.value;
        QVERIFY(component.value != 0.0);  // zero-valued components are skipped
    }
    QVERIFY(qFuzzyCompare(sum, scored.score));
    QVERIFY(componentValue(scored, QStringLiteral("genre")) > 0.0);
    QVERIFY(componentValue(scored, QStringLiteral("skips")) < 0.0);
    QVERIFY(componentValue(scored, QStringLiteral("same-artist")) < 0.0);
}

void RadioTest::unratedAndUnknownYearYieldNoComponent()
{
    TrackScorer::SeedContext seed;
    seed.year = 2000;  // known, but the candidate's year is unknown

    const TrackScorer::Scored scored = TrackScorer::score(
        makeCandidate(QStringLiteral("/a"), QStringLiteral("a"), {}, /*year=*/0, /*rating=*/-1), {}, seed);

    QVERIFY(!hasComponent(scored, QStringLiteral("rating")));
    QVERIFY(!hasComponent(scored, QStringLiteral("era")));
}

// ---- RadioSession ----------------------------------------------------------

void RadioTest::artistThrottleNeverPicksSameArtistConsecutively()
{
    // Four strong same-artist candidates (distinct albums) plus filler; the hard
    // throttle must keep any artist out of three consecutive picks.
    QVector<TrackScorer::Candidate> pool;
    for (int i = 0; i < 4; ++i) {
        pool.push_back(makeCandidate(QStringLiteral("/hot%1").arg(i), QStringLiteral("hot"),
                                     {QStringLiteral("rock")}, 2000, 100, true,
                                     QStringLiteral("hot\nalbum%1").arg(i)));
    }
    for (int i = 0; i < 6; ++i) {
        pool.push_back(makeCandidate(QStringLiteral("/f%1").arg(i), QStringLiteral("filler%1").arg(i),
                                     {QStringLiteral("rock")}, 2000, 40, false));
    }

    TrackScorer::Candidate seed = makeCandidate(QStringLiteral("/seed"), QStringLiteral("seed"),
                                                {QStringLiteral("rock")}, 2000);
    QRandomGenerator rng(12345u);
    RadioSession session(pool, {}, {}, seed, 30, 1'000'000'000, &rng);

    const QVector<Track> picks = session.nextTracks(8, {}, resolvePathToTrack);
    QVERIFY(picks.size() >= 4);
    // Reconstruct each pick's artist (all "hot" tracks share one artist; each
    // filler is its own) and assert no artist is ever picked back-to-back.
    const auto artistOf = [](const QString &path) {
        return path.startsWith(QStringLiteral("/hot")) ? QStringLiteral("hot") : path.mid(2);
    };
    for (int i = 1; i < picks.size(); ++i) {
        QVERIFY(artistOf(picks.at(i - 1).path) != artistOf(picks.at(i).path));
    }
}

void RadioTest::albumCapLimitsTracksPerAlbum()
{
    // Three tracks share one album (distinct artists so the artist throttle does
    // not interfere); at most two may be committed per session.
    QVector<TrackScorer::Candidate> pool;
    for (int i = 0; i < 3; ++i) {
        pool.push_back(makeCandidate(QStringLiteral("/alb%1").arg(i), QStringLiteral("artist%1").arg(i),
                                     {QStringLiteral("rock")}, 2000, 100, true,
                                     QStringLiteral("shared\nalbum")));
    }
    for (int i = 0; i < 6; ++i) {
        pool.push_back(makeCandidate(QStringLiteral("/o%1").arg(i), QStringLiteral("other%1").arg(i),
                                     {QStringLiteral("rock")}, 2000, 30, false,
                                     QStringLiteral("other%1\nalbum").arg(i)));
    }

    TrackScorer::Candidate seed = makeCandidate(QStringLiteral("/seed"), QStringLiteral("seed"),
                                                {QStringLiteral("rock")}, 2000);
    QRandomGenerator rng(999u);
    RadioSession session(pool, {}, {}, seed, 30, 1'000'000'000, &rng);

    const QVector<Track> picks = session.nextTracks(9, {}, resolvePathToTrack);
    int albumCount = 0;
    for (const Track &pick : picks) {
        if (pick.path.startsWith(QStringLiteral("/alb"))) {
            ++albumCount;
        }
    }
    QVERIFY2(albumCount <= 2, "more than two tracks from one album in a session");
}

void RadioTest::noRepeatsWithinSession()
{
    QVector<TrackScorer::Candidate> pool;
    for (int i = 0; i < 5; ++i) {
        pool.push_back(makeCandidate(QStringLiteral("/t%1").arg(i), QStringLiteral("artist%1").arg(i),
                                     {QStringLiteral("rock")}, 2000));
    }
    TrackScorer::Candidate seed = makeCandidate(QStringLiteral("/seed"), QStringLiteral("seed"),
                                                {QStringLiteral("rock")}, 2000);
    QRandomGenerator rng(7u);
    RadioSession session(pool, {}, {}, seed, 30, 1'000'000'000, &rng);

    const QVector<Track> picks = session.nextTracks(20, {}, resolvePathToTrack);  // more than the pool
    QSet<QString> seen;
    for (const Track &pick : picks) {
        QVERIFY2(!seen.contains(pick.path), "a path was picked twice in one session");
        seen.insert(pick.path);
    }
    QVERIFY(picks.size() <= pool.size());
}

void RadioTest::excludePathsAreRespected()
{
    QVector<TrackScorer::Candidate> pool;
    for (int i = 0; i < 5; ++i) {
        pool.push_back(makeCandidate(QStringLiteral("/t%1").arg(i), QStringLiteral("artist%1").arg(i),
                                     {QStringLiteral("rock")}, 2000));
    }
    TrackScorer::Candidate seed = makeCandidate(QStringLiteral("/seed"), QStringLiteral("seed"),
                                                {QStringLiteral("rock")}, 2000);
    QRandomGenerator rng(3u);
    RadioSession session(pool, {}, {}, seed, 30, 1'000'000'000, &rng);

    const QSet<QString> exclude{QStringLiteral("/t0"), QStringLiteral("/t3")};
    const QVector<Track> picks = session.nextTracks(20, exclude, resolvePathToTrack);
    for (const Track &pick : picks) {
        QVERIFY(!exclude.contains(pick.path));
    }
}

void RadioTest::rollingContextDriftsGenreWindow()
{
    // Seed mood is "pop". Candidates rockA/rockB are pure rock (no overlap with
    // the seed window), and r1..r3 are rock feed tracks. Before any drift, a pick
    // has no genre contribution; after playing three rock tracks the window picks
    // up "rock", so a rock pick now scores on genre.
    const auto buildPool = []() {
        QVector<TrackScorer::Candidate> pool;
        pool.push_back(makeCandidate(QStringLiteral("/rockA"), QStringLiteral("rocka"),
                                     {QStringLiteral("rock")}, 2000));
        pool.push_back(makeCandidate(QStringLiteral("/rockB"), QStringLiteral("rockb"),
                                     {QStringLiteral("rock")}, 2000));
        for (int i = 0; i < 3; ++i) {
            pool.push_back(makeCandidate(QStringLiteral("/r%1").arg(i), QStringLiteral("feed%1").arg(i),
                                         {QStringLiteral("rock")}, 2000));
        }
        return pool;
    };
    TrackScorer::Candidate seed = makeCandidate(QStringLiteral("/seed"), QStringLiteral("seed"),
                                                {QStringLiteral("pop")}, 2000);

    // A non-empty IDF map is needed so a genre match actually contributes: an
    // empty map (the default) makes every genre score as IDF 0.
    const QHash<QString, double> genreIdf{{QStringLiteral("pop"), 2.0}, {QStringLiteral("rock"), 2.0}};

    QRandomGenerator rngA(42u);
    RadioSession noDrift(buildPool(), {}, genreIdf, seed, 30, 1'000'000'000, &rngA);
    const QVector<Track> before = noDrift.nextTracks(1, {}, resolvePathToTrack);
    QCOMPARE(before.size(), 1);
    QVERIFY2(!noDrift.reasonFor(before.first().path).contains(QStringLiteral("genre")),
             "genre matched before any rock was played");

    QRandomGenerator rngB(42u);
    RadioSession drifted(buildPool(), {}, genreIdf, seed, 30, 1'000'000'000, &rngB);
    for (int i = 0; i < 3; ++i) {
        drifted.notePlayed(resolvePathToTrack(QStringLiteral("/r%1").arg(i)));
    }
    const QVector<Track> after = drifted.nextTracks(1, {}, resolvePathToTrack);
    QCOMPARE(after.size(), 1);
    QVERIFY2(after.first().path == QStringLiteral("/rockA") || after.first().path == QStringLiteral("/rockB"),
             "feed tracks should be exhausted, leaving the rock candidates");
    QVERIFY2(drifted.reasonFor(after.first().path).contains(QStringLiteral("genre")),
             "rolling window did not drift genre toward rock");
}

void RadioTest::reasonForNonEmptyOnPick()
{
    QVector<TrackScorer::Candidate> pool{makeCandidate(QStringLiteral("/t0"), QStringLiteral("a"),
                                                       {QStringLiteral("rock")}, 2000, 90, true)};
    TrackScorer::Candidate seed = makeCandidate(QStringLiteral("/seed"), QStringLiteral("seed"),
                                                {QStringLiteral("rock")}, 2000);
    QRandomGenerator rng(1u);
    RadioSession session(pool, {}, {}, seed, 30, 1'000'000'000, &rng);

    const QVector<Track> picks = session.nextTracks(1, {}, resolvePathToTrack);
    QCOMPARE(picks.size(), 1);
    QVERIFY(!session.reasonFor(picks.first().path).isEmpty());
    QVERIFY(session.reasonFor(QStringLiteral("/never")).isEmpty());
}

// ---- Database + ListenHistoryStore -----------------------------------------

namespace {

Track makeDbTrack(const QTemporaryDir &dir, const QString &filename, const QStringList &genres,
                  const QString &originalDate)
{
    Track track;
    track.path = dir.filePath(QStringLiteral("Artist/Album/%1").arg(filename));
    track.parentDir = dir.filePath(QStringLiteral("Artist/Album"));
    track.filename = filename;
    track.title = filename;
    track.artistName = QStringLiteral("Artist");
    track.albumArtistName = QStringLiteral("Album Artist");
    track.albumTitle = QStringLiteral("Album");
    track.originalDate = originalDate;
    track.fileSize = 10;
    track.fileMtime = 20;
    if (!genres.isEmpty()) {
        MetadataBlob::FullMetadata meta;
        meta.tags.insert(QStringLiteral("GENRE"), genres);
        const MetadataBlob::Encoded encoded = MetadataBlob::encode(meta);
        track.fullMetadataBlob = encoded.data;
        track.fullMetadataRawSize = encoded.rawSize;
    }
    return track;
}

} // namespace

void RadioTest::radioCandidatesJoinsGenresAndFallback()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Database db(QStringLiteral("radio-cand-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(db.open(dir.filePath(QStringLiteral("library.sqlite"))), qPrintable(db.lastError()));

    const Track rockPop = makeDbTrack(dir, QStringLiteral("01.flac"),
                                      {QStringLiteral("Rock"), QStringLiteral("Pop")},
                                      QStringLiteral("2004-05-06"));
    const Track jazz = makeDbTrack(dir, QStringLiteral("02.flac"), {QStringLiteral("Jazz")},
                                   QStringLiteral("1999"));
    const Track noGenre = makeDbTrack(dir, QStringLiteral("03.flac"), {}, QStringLiteral("2010-01-01"));
    QVERIFY2(db.upsertTrack(rockPop), qPrintable(db.lastError()));
    QVERIFY2(db.upsertTrack(jazz), qPrintable(db.lastError()));
    QVERIFY2(db.upsertTrack(noGenre), qPrintable(db.lastError()));
    QVERIFY(db.setUserTrackRating(rockPop.path, 80));

    const QVector<RadioCandidateRow> rockRows =
        db.radioCandidates({GenreTags::folded(QStringLiteral("Rock"))});
    QCOMPARE(rockRows.size(), 1);
    QCOMPARE(rockRows.first().path, rockPop.path);
    QCOMPARE(rockRows.first().year, 2004);
    QVERIFY(rockRows.first().hasUserRating);
    QCOMPARE(rockRows.first().effectiveRating0To100, 80);
    // The row carries the track's FULL folded genre set, not just the match.
    QVERIFY(rockRows.first().genresFolded.contains(GenreTags::folded(QStringLiteral("Rock"))));
    QVERIFY(rockRows.first().genresFolded.contains(GenreTags::folded(QStringLiteral("Pop"))));

    // A genre nobody has returns nothing.
    QVERIFY(db.radioCandidates({GenreTags::folded(QStringLiteral("Techno"))}).isEmpty());

    // Fallback samples every scanned track regardless of genre.
    const QVector<RadioCandidateRow> fallback = db.radioFallbackCandidates();
    QCOMPARE(fallback.size(), 3);
}

void RadioTest::genreTrackCountsAggregatesAcrossLibrary()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Database db(QStringLiteral("genre-counts-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(db.open(dir.filePath(QStringLiteral("library.sqlite"))), qPrintable(db.lastError()));

    // rock+pop, jazz, and a genre-less track: df(rock)=1, df(pop)=1, df(jazz)=1,
    // taggedTrackTotal=2 (the genre-less track does not count).
    const Track rockPop = makeDbTrack(dir, QStringLiteral("01.flac"),
                                      {QStringLiteral("Rock"), QStringLiteral("Pop")},
                                      QStringLiteral("2004-05-06"));
    const Track jazz = makeDbTrack(dir, QStringLiteral("02.flac"), {QStringLiteral("Jazz")},
                                   QStringLiteral("1999"));
    const Track noGenre = makeDbTrack(dir, QStringLiteral("03.flac"), {}, QStringLiteral("2010-01-01"));
    QVERIFY2(db.upsertTrack(rockPop), qPrintable(db.lastError()));
    QVERIFY2(db.upsertTrack(jazz), qPrintable(db.lastError()));
    QVERIFY2(db.upsertTrack(noGenre), qPrintable(db.lastError()));

    int taggedTrackTotal = 0;
    const QHash<QString, int> counts = db.genreTrackCounts(&taggedTrackTotal);
    QCOMPARE(counts.value(GenreTags::folded(QStringLiteral("Rock"))), 1);
    QCOMPARE(counts.value(GenreTags::folded(QStringLiteral("Pop"))), 1);
    QCOMPARE(counts.value(GenreTags::folded(QStringLiteral("Jazz"))), 1);
    QCOMPARE(taggedTrackTotal, 2);
}

void RadioTest::trackAffinitiesAggregateAllSources()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));
    QVERIFY(store.isOpen());

    const QString path = QStringLiteral("/music/song.flac");

    Track track;
    track.path = path;
    track.title = QStringLiteral("Song");
    track.artistName = QStringLiteral("Artist");

    // play_events: 3 finished, 1 early skip, 1 late skip. Only the early skip
    // (before the scrobble threshold, min(duration/2, 4 min)) may count as a
    // dislike; the late skip is a listen the user merely moved on from.
    for (int i = 0; i < 5; ++i) {
        ListenHistoryStore::PlayEvent event;
        event.track = track;
        event.startedAtSecs = 1000 + i;
        event.durationMs = 240000;
        if (i == 3) {
            event.outcome = QStringLiteral("skipped");
            event.playedMs = 30000;    // early: 30 s of a 4-min track
        } else if (i == 4) {
            event.outcome = QStringLiteral("skipped");
            event.playedMs = 200000;   // late: past the 120 s threshold
        } else {
            event.outcome = QStringLiteral("finished");
            event.playedMs = 240000;
        }
        event.source = QStringLiteral("queue_auto");
        event.sessionId = QStringLiteral("s1");
        QVERIFY(store.recordPlayEvent(event) > 0);
    }

    // listens: 2 local listens (distinct timestamps).
    QVERIFY(store.recordListen(track, 5000, false, false) > 0);
    QVERIFY(store.recordListen(track, 6000, false, false) > 0);

    // imported_listens: 3 matched to the path.
    QList<ListenHistoryStore::ImportedListen> imported;
    for (int i = 0; i < 3; ++i) {
        ListenHistoryStore::ImportedListen row;
        row.source = QStringLiteral("listenbrainz");
        row.listenedAtSecs = 7000 + i;
        row.title = QStringLiteral("Song");
        row.artist = QStringLiteral("Artist");
        row.matchedTrackPath = path;
        imported.push_back(row);
    }
    QCOMPARE(store.recordImportedListens(imported), 3);

    // baselines: max across services (never summed).
    ListenHistoryStore::PlaycountBaseline lastfm;
    lastfm.source = QStringLiteral("lastfm");
    lastfm.artist = QStringLiteral("Artist");
    lastfm.title = QStringLiteral("Song");
    lastfm.matchedTrackPath = path;
    lastfm.count = 42;
    lastfm.syncedAtSecs = 8000;
    QVERIFY(store.upsertPlaycountBaseline(lastfm));
    ListenHistoryStore::PlaycountBaseline lb = lastfm;
    lb.source = QStringLiteral("listenbrainz");
    lb.count = 30;
    QVERIFY(store.upsertPlaycountBaseline(lb));

    const QHash<QString, ListenHistoryStore::TrackAffinityRow> affinities = store.trackAffinities();
    QVERIFY(affinities.contains(path));
    const ListenHistoryStore::TrackAffinityRow row = affinities.value(path);
    QCOMPARE(row.playEvents, 5);
    QCOMPARE(row.finished, 3);
    QCOMPARE(row.skipped, 1);          // the late skip is not a dislike
    QCOMPARE(row.lastPlayedAtSecs, qint64(1004));
    QCOMPARE(row.listenCount, 5);      // 2 local + 3 imported
    QCOMPARE(row.baselineMax, 42);     // max(42, 30), never 72
}

// ---- GenreTags --------------------------------------------------------------

void RadioTest::nonGenrePlaceholdersAreStoplisted()
{
    const QStringList placeholders = {
        QStringLiteral("other"),      QStringLiteral("unknown"), QStringLiteral("misc"),
        QStringLiteral("none"),       QStringLiteral("undefined"), QStringLiteral("no genre"),
        QStringLiteral("unclassifiable"), QStringLiteral("various"), QStringLiteral("genre"),
    };
    for (const QString &placeholder : placeholders) {
        QVERIFY2(GenreTags::isNonGenre(placeholder), qPrintable(placeholder));
    }
    // Case/whitespace variation is handled by folding upstream (see folded());
    // isNonGenre itself compares already-folded input.
    QVERIFY(GenreTags::isNonGenre(GenreTags::folded(QStringLiteral("  Other  "))));

    QVERIFY(!GenreTags::isNonGenre(QStringLiteral("rock")));
    QVERIFY(!GenreTags::isNonGenre(QStringLiteral("dream pop")));
    QVERIFY(!GenreTags::isNonGenre(QStringLiteral("")));
}

void RadioTest::informativeFiltersStoplistedGenres()
{
    const QStringList mixed = {QStringLiteral("rock"), QStringLiteral("other"), QStringLiteral("shoegaze"),
                               QStringLiteral("unknown")};
    QCOMPARE(GenreTags::informative(mixed), QStringList({QStringLiteral("rock"), QStringLiteral("shoegaze")}));

    // Junk-only input filters down to nothing (the seed's whole genre list, in
    // the bug this fix addresses).
    QVERIFY(GenreTags::informative({QStringLiteral("other")}).isEmpty());

    QVERIFY(GenreTags::informative({}).isEmpty());
}

QTEST_GUILESS_MAIN(RadioTest)
#include "test_radio.moc"
