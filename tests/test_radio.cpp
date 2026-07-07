#include "core/FoldKey.h"
#include "core/GenreTags.h"
#include "core/MetadataBlob.h"
#include "db/Database.h"
#include "features/FeatureStore.h"
#include "features/QualityRank.h"
#include "features/SongIdentity.h"
#include "reco/AffinityPool.h"
#include "reco/ArtistRadio.h"
#include "reco/RadioFilters.h"
#include "reco/RadioMix.h"
#include "reco/RadioSession.h"
#include "reco/ReasonText.h"
#include "reco/TrackScorer.h"
#include "reco/WeightLearner.h"
#include "scrobble/ListenHistoryStore.h"

#include <QRandomGenerator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <QVariant>

#include <cmath>

namespace {

TrackScorer::Candidate makeCandidate(const QString &path, const QString &artistFolded,
                                     const QStringList &genresFolded, int year = 0,
                                     int rating = -1, bool hasUserRating = false,
                                     const QString &albumKey = {}, const QString &songKey = {},
                                     double tempoBpm = -1.0, double energy = -1.0,
                                     qint64 contentGroupId = -1)
{
    TrackScorer::Candidate candidate;
    candidate.path = path;
    candidate.contentGroupId = contentGroupId;
    candidate.songKey = songKey.isEmpty() ? (QStringLiteral("path:") + path) : songKey;
    candidate.artistFolded = artistFolded;
    candidate.albumKey = albumKey.isEmpty() ? (artistFolded + QLatin1String("\nalbum")) : albumKey;
    candidate.genresFolded = genresFolded;
    candidate.year = year;
    candidate.tempoBpm = tempoBpm;
    candidate.energy = energy;
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

double componentValue(const QList<TrackScorer::Component> &components, const QString &name)
{
    for (const TrackScorer::Component &component : components) {
        if (component.name == name) {
            return component.value;
        }
    }
    return 0.0;
}

bool hasComponent(const QList<TrackScorer::Component> &components, const QString &name)
{
    for (const TrackScorer::Component &component : components) {
        if (component.name == name) {
            return true;
        }
    }
    return false;
}

WeightLearner::Sample learnerSample(bool earlySkip, std::initializer_list<QPair<QString, double>> features)
{
    WeightLearner::Sample sample;
    sample.earlySkip = earlySkip;
    for (const QPair<QString, double> &feature : features) {
        sample.features.insert(feature.first, feature.second);
    }
    return sample;
}

double learnedMultiplier(const WeightLearner::Result &result, const QString &componentName)
{
    for (const WeightLearner::ComponentResult &row : result.components) {
        if (row.componentName == componentName) {
            return row.multiplier;
        }
    }
    return 1.0;
}

Track resolvePathToTrack(const QString &path)
{
    Track track;
    track.path = path;
    track.title = path;
    return track;
}

Track playedTrack(const QString &path, const QString &artistName)
{
    Track track = resolvePathToTrack(path);
    track.artistName = artistName;
    return track;
}

TrackScorer::Affinity makeAffinity(int playEvents, int finished, int skipped,
                                   qint64 lastPlayedAtSecs, int listenCount, int baselineMax)
{
    TrackScorer::Affinity affinity;
    affinity.playEvents = playEvents;
    affinity.finished = finished;
    affinity.skipped = skipped;
    affinity.lastPlayedAtSecs = lastPlayedAtSecs;
    affinity.listenCount = listenCount;
    affinity.baselineMax = baselineMax;
    return affinity;
}

void QCOMPARE_AFFINITY(const TrackScorer::Affinity &actual, const TrackScorer::Affinity &expected)
{
    QCOMPARE(actual.playEvents, expected.playEvents);
    QCOMPARE(actual.finished, expected.finished);
    QCOMPARE(actual.skipped, expected.skipped);
    QCOMPARE(actual.lastPlayedAtSecs, expected.lastPlayedAtSecs);
    QCOMPARE(actual.listenCount, expected.listenCount);
    QCOMPARE(actual.baselineMax, expected.baselineMax);
}

bool containsCandidatePath(const QVector<TrackScorer::Candidate> &candidates, const QString &path)
{
    for (const TrackScorer::Candidate &candidate : candidates) {
        if (candidate.path == path) {
            return true;
        }
    }
    return false;
}

} // namespace

class RadioTest final : public QObject {
    Q_OBJECT

private slots:
    // FoldKey
    void songKeyPrefersRecordingMbid();
    void songKeyFallsBackToFoldedArtistTitle();
    void albumGroupKeyPrefersReleaseGroupMbid();

    // AffinityPool
    void affinityPoolSumsDuplicateSongHistory();
    void affinityPoolLeavesUnmappedPathsUnchanged();
    void affinityPoolEmptyMapIsNoOp();

    // RadioMix
    void rediscoveryLovedOldPasses();
    void rediscoveryRecentFails();
    void rediscoveryUnlovedFails();
    void rediscoveryRelaxationKicksIn();
    void deepCutsLikedArtistRarePasses();
    void deepCutsOverplayedFails();
    void deepCutsSkipStainedFails();

    // RadioFilters
    void radioFiltersExcludeFlaggedCandidates();
    void radioFiltersDropNoLearnBeforeSongPooling();

    // TrackScorer
    void genreOverlapDominates();
    void genreIdfSaturatesOnRareSharedGenre();
    void genreIdfPartialOnBroadSharedGenre();
    void genreCrowdingDampensTagSoupMatches();
    void genreAbsentWithNoSharedGenres();
    void genreAbsentWithEmptyIdfMap();
    void scoringWeightsJsonOverridesDefaults();
    void scoringWeightsJsonRoundTripsAllFields();
    void scoringWeightSpecsRoundTripThroughJson();
    void scoringWeightsJsonRejectsUnknownAndInvalidFields();
    void weightLearnerLearnsDirectionalMultipliers();
    void weightLearnerStrengthensVindicatedPenalties();
    void weightLearnerWeakensUselessPenalties();
    void weightLearnerIsDeterministic();
    void weightLearnerClampsExtremeSuggestions();
    void weightLearnerRefusesSparseData();
    void eraDecaysWithYearGap();
    void tempoAndEnergyUseSonicProximity();
    void unknownTempoOrEnergyYieldsNoComponent();
    void audioComponentUsesEmbeddingCosine();
    void unknownAudioEmbeddingYieldsNoComponent();
    void skipPenaltyScalesWithSkipRate();
    void recencyPenaltyDecaysWithTime();
    void noveltyAtZeroHistoryScalesWithExploration();
    void componentsSumAndSignsMatchScore();
    void unratedAndUnknownYearYieldNoComponent();

    // RadioSession
    void artistThrottleNeverPicksSameArtistConsecutively();
    void albumCapLimitsTracksPerAlbum();
    void songKeyDedupUsesRecordingMbid();
    void songKeyDedupFallsBackToArtistTitle();
    void albumCapUsesReleaseGroupKey();
    void noRepeatsWithinSession();
    void excludePathsAreRespected();
    void rollingContextDriftsGenreWindow();
    void rollingSonicContextUsesPlayedMean();
    void substitutedBestCopyFeedsRollingContext();
    void rollingAudioContextUsesSeedAndPlayedEmbeddings();
    void reasonForNonEmptyOnPick();
    void reasonComponentsRoundTripFromPick();
    void resolvedPickStoresReasonUnderResolvedPath();
    void pickReasonsEnumeratesStoredComponents();
    void reasonSentencePicksTopPhrases();
    void reasonSentenceNamesAudio();
    void reasonSentenceNamesTempoAndEnergy();
    void reasonSentenceHandlesPenaltyOnly();
    void reasonSentenceEmptyOnEmptyInput();
    void reasonBreakdownFormatsSigned();
    void setExplorationTakesEffectOnSubsequentPicks();
    void batchOfFifteenRespectsThrottlesAndIsDistinct();
    void isEarlySkipUsesHalfDurationCappedAtFourMinutes();
    void anchorlessSessionDriftsFromPlayedContext();
    void syntheticArtistSeedThrottlesSeedArtistAndIsNotPickable();
    void foreignNotePlayedEntersRollingConstraints();
    void constraintStateRoundTripPreservesSequencing();
    void artistConstraintStateMetadataRoundTrips();

    // Database + ListenHistoryStore round-trips
    void radioCandidatesJoinsGenresAndFallback();
    void neighborCandidateRowsAugmentTagPoorPoolAndRespectFlags();
    void featureStoreV3ScalarsFeedTempoEnergyScoring();
    void genreAliasesExpandCandidatesAndMergeCounts();
    void radioWeightProfilesRoundTrip();
    void ignoredRadioGenresRoundTripAndSuppressCandidateJoins();
    void ignoredRadioGenresCanonicalizeAliasesAndPreserveVisibility();
    void genreTrackCountsAggregatesAcrossLibrary();
    void artistSeedGenresAggregateFrequencyAliasesIgnoresAndCap();
    void artistMedianYearIgnoresUnknowns();
    void artistRepresentativeTrackUsesRatingThenAffinity();
    void sampleArtistsForGenreReturnsDeterministicNames();
    void genrePipeBackfillResplitsStoredMetadata();
    void trackAffinitiesAggregateAllSources();
    void contentGroupAffinityPoolsDisagreeingTags();
    void contentGroupSongKeyDedupsRadioSession();
    void contentGroupResolverQueuesBestOrPinnedCopy();

    // GenreTags
    void genreTagsSplitPipeSeparators();
    void genreCanonicalIdentityAndMapping();
    void genreCanonicalStoplistAfterMapping();
    void nonGenrePlaceholdersAreStoplisted();
    void informativeFiltersStoplistedGenres();
};

// ---- FoldKey ---------------------------------------------------------------

void RadioTest::songKeyPrefersRecordingMbid()
{
    QCOMPARE(FoldKey::songKey(QStringLiteral("recording-1"), QStringLiteral("Artist"), QStringLiteral("Title")),
             QStringLiteral("mbid:recording-1"));
}

void RadioTest::songKeyFallsBackToFoldedArtistTitle()
{
    QCOMPARE(FoldKey::songKey({}, QStringLiteral("  The  Artist  "), QStringLiteral("  The  Title  ")),
             QStringLiteral("at:the artist\nthe title"));
}

void RadioTest::albumGroupKeyPrefersReleaseGroupMbid()
{
    QCOMPARE(FoldKey::albumGroupKey(QStringLiteral("release-group-1"), QStringLiteral("Artist"),
                                    QStringLiteral("Album")),
             QStringLiteral("rg:release-group-1"));
    QCOMPARE(FoldKey::albumGroupKey({}, QStringLiteral("  Album  Artist  "), QStringLiteral("  Album  ")),
             QStringLiteral("album artist\nalbum"));
}

// ---- AffinityPool ----------------------------------------------------------

void RadioTest::affinityPoolSumsDuplicateSongHistory()
{
    const QHash<QString, TrackScorer::Affinity> byPath{
        {QStringLiteral("/album.flac"), makeAffinity(2, 1, 1, 100, 3, 12)},
        {QStringLiteral("/compilation.opus"), makeAffinity(4, 3, 0, 250, 5, 9)},
    };
    const QHash<QString, QString> pathToSongKey{
        {QStringLiteral("/album.flac"), QStringLiteral("mbid:shared")},
        {QStringLiteral("/compilation.opus"), QStringLiteral("mbid:shared")},
        {QStringLiteral("/portable.mp3"), QStringLiteral("mbid:shared")},
    };

    const QHash<QString, TrackScorer::Affinity> pooled =
        AffinityPool::poolBySongKey(byPath, pathToSongKey);
    const TrackScorer::Affinity expected = makeAffinity(6, 4, 1, 250, 8, 12);

    QCOMPARE_AFFINITY(pooled.value(QStringLiteral("/album.flac")), expected);
    QCOMPARE_AFFINITY(pooled.value(QStringLiteral("/compilation.opus")), expected);
    QCOMPARE_AFFINITY(pooled.value(QStringLiteral("/portable.mp3")), expected);
}

void RadioTest::affinityPoolLeavesUnmappedPathsUnchanged()
{
    const TrackScorer::Affinity original = makeAffinity(1, 1, 0, 42, 2, 7);
    const QHash<QString, TrackScorer::Affinity> byPath{
        {QStringLiteral("/unmapped.flac"), original},
    };
    const QHash<QString, QString> pathToSongKey{
        {QStringLiteral("/other.flac"), QStringLiteral("mbid:other")},
    };

    const QHash<QString, TrackScorer::Affinity> pooled =
        AffinityPool::poolBySongKey(byPath, pathToSongKey);

    QCOMPARE(pooled.size(), 1);
    QCOMPARE_AFFINITY(pooled.value(QStringLiteral("/unmapped.flac")), original);
}

void RadioTest::affinityPoolEmptyMapIsNoOp()
{
    const TrackScorer::Affinity original = makeAffinity(3, 2, 1, 123, 5, 11);
    const QHash<QString, TrackScorer::Affinity> byPath{
        {QStringLiteral("/song.flac"), original},
    };

    const QHash<QString, TrackScorer::Affinity> pooled =
        AffinityPool::poolBySongKey(byPath, {});

    QCOMPARE(pooled.size(), 1);
    QCOMPARE_AFFINITY(pooled.value(QStringLiteral("/song.flac")), original);
}

// ---- RadioMix --------------------------------------------------------------

void RadioTest::rediscoveryLovedOldPasses()
{
    constexpr qint64 now = 1'000'000'000;
    const QVector<TrackScorer::Candidate> filtered = RadioMix::filterCandidates(
        RadioMix::Mode::Rediscovery,
        {makeCandidate(QStringLiteral("/old-loved"), QStringLiteral("artist"), {}, 0, 80, true)},
        {{QStringLiteral("/old-loved"), makeAffinity(0, 0, 0, now - 181 * 86400, 0, 0)}},
        now);

    QCOMPARE(filtered.size(), 1);
    QCOMPARE(filtered.first().path, QStringLiteral("/old-loved"));
}

void RadioTest::rediscoveryRecentFails()
{
    constexpr qint64 now = 1'000'000'000;
    const QVector<TrackScorer::Candidate> filtered = RadioMix::filterCandidates(
        RadioMix::Mode::Rediscovery,
        {makeCandidate(QStringLiteral("/recent-loved"), QStringLiteral("artist"), {}, 0, 90, true)},
        {{QStringLiteral("/recent-loved"), makeAffinity(0, 0, 0, now - 30 * 86400, 0, 0)}},
        now);

    QVERIFY(filtered.isEmpty());
}

void RadioTest::rediscoveryUnlovedFails()
{
    constexpr qint64 now = 1'000'000'000;
    const QVector<TrackScorer::Candidate> filtered = RadioMix::filterCandidates(
        RadioMix::Mode::Rediscovery,
        {makeCandidate(QStringLiteral("/old-unloved"), QStringLiteral("artist"), {}, 0, 40, true)},
        {{QStringLiteral("/old-unloved"), makeAffinity(0, 4, 0, now - 220 * 86400, 0, 0)}},
        now);

    QVERIFY(filtered.isEmpty());
}

void RadioTest::rediscoveryRelaxationKicksIn()
{
    constexpr qint64 now = 1'000'000'000;
    QVector<TrackScorer::Candidate> candidates;
    QHash<QString, TrackScorer::Affinity> affinities;
    for (int i = 0; i < 49; ++i) {
        const QString path = QStringLiteral("/strict%1").arg(i);
        candidates.push_back(makeCandidate(path, QStringLiteral("artist%1").arg(i), {}, 0, 75, true));
        affinities.insert(path, makeAffinity(0, 0, 0, now - 220 * 86400, 0, 0));
    }
    candidates.push_back(makeCandidate(QStringLiteral("/relaxed"), QStringLiteral("artist-relaxed"),
                                       {}, 0, 80, true));
    affinities.insert(QStringLiteral("/relaxed"), makeAffinity(0, 0, 0, now - 120 * 86400, 0, 0));

    const QVector<TrackScorer::Candidate> filtered =
        RadioMix::filterCandidates(RadioMix::Mode::Rediscovery, candidates, affinities, now);

    QCOMPARE(filtered.size(), 50);
    QVERIFY(containsCandidatePath(filtered, QStringLiteral("/relaxed")));
}

void RadioTest::deepCutsLikedArtistRarePasses()
{
    const QVector<TrackScorer::Candidate> candidates{
        makeCandidate(QStringLiteral("/hit"), QStringLiteral("liked-artist"), {}),
        makeCandidate(QStringLiteral("/rare"), QStringLiteral("liked-artist"), {}),
    };
    const QHash<QString, TrackScorer::Affinity> affinities{
        {QStringLiteral("/hit"), makeAffinity(0, 0, 0, 100, 20, 0)},
        {QStringLiteral("/rare"), makeAffinity(0, 0, 0, 100, 1, 0)},
    };

    const QVector<TrackScorer::Candidate> filtered =
        RadioMix::filterCandidates(RadioMix::Mode::DeepCuts, candidates, affinities, 1000);

    QVERIFY(containsCandidatePath(filtered, QStringLiteral("/rare")));
    QVERIFY(!containsCandidatePath(filtered, QStringLiteral("/hit")));
}

void RadioTest::deepCutsOverplayedFails()
{
    const QVector<TrackScorer::Candidate> candidates{
        makeCandidate(QStringLiteral("/hit"), QStringLiteral("liked-artist"), {}),
        makeCandidate(QStringLiteral("/overplayed"), QStringLiteral("liked-artist"), {}),
    };
    const QHash<QString, TrackScorer::Affinity> affinities{
        {QStringLiteral("/hit"), makeAffinity(0, 0, 0, 100, 20, 0)},
        {QStringLiteral("/overplayed"), makeAffinity(0, 0, 0, 100, 3, 0)},
    };

    const QVector<TrackScorer::Candidate> filtered =
        RadioMix::filterCandidates(RadioMix::Mode::DeepCuts, candidates, affinities, 1000);

    QVERIFY(!containsCandidatePath(filtered, QStringLiteral("/overplayed")));
}

void RadioTest::deepCutsSkipStainedFails()
{
    const QVector<TrackScorer::Candidate> candidates{
        makeCandidate(QStringLiteral("/hit"), QStringLiteral("liked-artist"), {}),
        makeCandidate(QStringLiteral("/skipped"), QStringLiteral("liked-artist"), {}),
    };
    const QHash<QString, TrackScorer::Affinity> affinities{
        {QStringLiteral("/hit"), makeAffinity(0, 0, 0, 100, 20, 0)},
        {QStringLiteral("/skipped"), makeAffinity(0, 0, 1, 100, 1, 0)},
    };

    const QVector<TrackScorer::Candidate> filtered =
        RadioMix::filterCandidates(RadioMix::Mode::DeepCuts, candidates, affinities, 1000);

    QVERIFY(!containsCandidatePath(filtered, QStringLiteral("/skipped")));
}

// ---- RadioFilters ----------------------------------------------------------

void RadioTest::radioFiltersExcludeFlaggedCandidates()
{
    const QVector<TrackScorer::Candidate> candidates = {
        makeCandidate(QStringLiteral("/music/a.flac"), QStringLiteral("artist"), {QStringLiteral("rock")}),
        makeCandidate(QStringLiteral("/music/b.flac"), QStringLiteral("artist"), {QStringLiteral("rock")}),
        makeCandidate(QStringLiteral("/music/c.flac"), QStringLiteral("artist"), {QStringLiteral("rock")}),
    };

    const QVector<TrackScorer::Candidate> filtered = RadioFilters::excludeFlaggedCandidates(
        candidates, QSet<QString>({QStringLiteral("/music/b.flac")}));

    QCOMPARE(filtered.size(), 2);
    QVERIFY(containsCandidatePath(filtered, QStringLiteral("/music/a.flac")));
    QVERIFY(!containsCandidatePath(filtered, QStringLiteral("/music/b.flac")));
    QVERIFY(containsCandidatePath(filtered, QStringLiteral("/music/c.flac")));
}

void RadioTest::radioFiltersDropNoLearnBeforeSongPooling()
{
    const QHash<QString, TrackScorer::Affinity> byPath{
        {QStringLiteral("/music/album.flac"), makeAffinity(2, 1, 0, 100, 3, 10)},
        {QStringLiteral("/music/compilation.flac"), makeAffinity(5, 4, 1, 200, 8, 20)},
        {QStringLiteral("/music/other.flac"), makeAffinity(1, 1, 0, 50, 1, 0)},
    };
    const QHash<QString, QString> pathToSongKey{
        {QStringLiteral("/music/album.flac"), QStringLiteral("mbid:shared")},
        {QStringLiteral("/music/compilation.flac"), QStringLiteral("mbid:shared")},
        {QStringLiteral("/music/other.flac"), QStringLiteral("mbid:other")},
    };
    const QSet<QString> noLearn{QStringLiteral("/music/compilation.flac")};

    const QHash<QString, TrackScorer::Affinity> filteredAffinities =
        RadioFilters::excludeFlaggedAffinities(byPath, noLearn);
    const QHash<QString, QString> filteredMappings =
        RadioFilters::excludeFlaggedPathMappings(pathToSongKey, noLearn);
    const QHash<QString, TrackScorer::Affinity> pooled =
        AffinityPool::poolBySongKey(filteredAffinities, filteredMappings);

    QVERIFY(!pooled.contains(QStringLiteral("/music/compilation.flac")));
    QCOMPARE_AFFINITY(pooled.value(QStringLiteral("/music/album.flac")),
                      makeAffinity(2, 1, 0, 100, 3, 10));
    QCOMPARE_AFFINITY(pooled.value(QStringLiteral("/music/other.flac")),
                      makeAffinity(1, 1, 0, 50, 1, 0));
}

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

void RadioTest::genreCrowdingDampensTagSoupMatches()
{
    TrackScorer::SeedContext seed;
    seed.genresFolded = {QStringLiteral("electronic"), QStringLiteral("trance"), QStringLiteral("pop"),
                         QStringLiteral("jazz"), QStringLiteral("alternative")};
    seed.genreIdf = {
        {QStringLiteral("electronic"), 1.0},
        {QStringLiteral("trance"), 1.0},
        {QStringLiteral("pop"), 1.0},
        {QStringLiteral("jazz"), 1.0},
        {QStringLiteral("alternative"), 1.0},
    };

    const TrackScorer::Candidate soup =
        makeCandidate(QStringLiteral("/soup"), QStringLiteral("a"), seed.genresFolded);
    const double damped = componentValue(TrackScorer::score(soup, {}, seed), QStringLiteral("genre"));

    TrackScorer::Weights noCrowding = TrackScorer::defaultWeights();
    noCrowding.genreCrowdingSoftLimit = 99.0;
    const double undamped = componentValue(TrackScorer::score(soup, {}, seed, noCrowding), QStringLiteral("genre"));

    QVERIFY(damped > 0.0);
    QVERIFY(damped < undamped);
    QVERIFY(qFuzzyCompare(undamped, 3.0 * 1.0 * 1.10));
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

void RadioTest::scoringWeightsJsonOverridesDefaults()
{
    QString error;
    const TrackScorer::Weights weights = TrackScorer::weightsFromJson(
        R"({"ratingWeight":0.75,"genreWeight":2.0,"tempoWeight":0.9,"energyWeight":1.1,"audioWeight":1.3,"skipPenalty":-3.5,"genreCrowdingSoftLimit":2})",
        &error);
    QVERIFY(error.isEmpty());
    QVERIFY(qFuzzyCompare(weights.ratingWeight, 0.75));
    QVERIFY(qFuzzyCompare(weights.genreWeight, 2.0));
    QVERIFY(qFuzzyCompare(weights.tempoWeight, 0.9));
    QVERIFY(qFuzzyCompare(weights.energyWeight, 1.1));
    QVERIFY(qFuzzyCompare(weights.audioWeight, 1.3));
    QVERIFY(qFuzzyCompare(weights.skipPenalty, -3.5));
    QVERIFY(qFuzzyCompare(weights.genreCrowdingSoftLimit, 2.0));

    TrackScorer::SeedContext seed;
    const TrackScorer::Scored scored = TrackScorer::score(
        makeCandidate(QStringLiteral("/rated"), QStringLiteral("a"), {}, 0, 100), {}, seed, weights);
    QVERIFY(qFuzzyCompare(componentValue(scored, QStringLiteral("rating")), 0.75));

    const TrackScorer::Weights fallback = TrackScorer::weightsFromJson("{", &error);
    QVERIFY(!error.isEmpty());
    QVERIFY(qFuzzyCompare(fallback.ratingWeight, TrackScorer::defaultWeights().ratingWeight));
}

void RadioTest::scoringWeightsJsonRoundTripsAllFields()
{
    TrackScorer::Weights weights;
    weights.genreWeight = 2.5;
    weights.genreIdfSaturation = 3.5;
    weights.genreCrowdingSoftLimit = 2.0;
    weights.eraWeight = 0.75;
    weights.eraSpanYears = 12.0;
    weights.tempoWeight = 0.55;
    weights.energyWeight = 0.65;
    weights.audioWeight = 1.3;
    weights.ratingWeight = 0.6;
    weights.userRatingBoost = 1.4;
    weights.historyWeight = 1.2;
    weights.historySaturation = 25.0;
    weights.noveltyWeight = 0.9;
    weights.noveltyZeroAt = 8.0;
    weights.recencyPenalty = -1.5;
    weights.recencyHalfLifeDays = 10.0;
    weights.skipPenalty = -3.0;
    weights.sameArtistPenalty = -0.4;

    QString error;
    const TrackScorer::Weights roundTrip = TrackScorer::weightsFromJson(
        TrackScorer::weightsToJson(weights), &error);
    QVERIFY(error.isEmpty());
    QVERIFY(qFuzzyCompare(roundTrip.genreWeight, weights.genreWeight));
    QVERIFY(qFuzzyCompare(roundTrip.genreIdfSaturation, weights.genreIdfSaturation));
    QVERIFY(qFuzzyCompare(roundTrip.genreCrowdingSoftLimit, weights.genreCrowdingSoftLimit));
    QVERIFY(qFuzzyCompare(roundTrip.eraWeight, weights.eraWeight));
    QVERIFY(qFuzzyCompare(roundTrip.eraSpanYears, weights.eraSpanYears));
    QVERIFY(qFuzzyCompare(roundTrip.tempoWeight, weights.tempoWeight));
    QVERIFY(qFuzzyCompare(roundTrip.energyWeight, weights.energyWeight));
    QVERIFY(qFuzzyCompare(roundTrip.audioWeight, weights.audioWeight));
    QVERIFY(qFuzzyCompare(roundTrip.ratingWeight, weights.ratingWeight));
    QVERIFY(qFuzzyCompare(roundTrip.userRatingBoost, weights.userRatingBoost));
    QVERIFY(qFuzzyCompare(roundTrip.historyWeight, weights.historyWeight));
    QVERIFY(qFuzzyCompare(roundTrip.historySaturation, weights.historySaturation));
    QVERIFY(qFuzzyCompare(roundTrip.noveltyWeight, weights.noveltyWeight));
    QVERIFY(qFuzzyCompare(roundTrip.noveltyZeroAt, weights.noveltyZeroAt));
    QVERIFY(qFuzzyCompare(roundTrip.recencyPenalty, weights.recencyPenalty));
    QVERIFY(qFuzzyCompare(roundTrip.recencyHalfLifeDays, weights.recencyHalfLifeDays));
    QVERIFY(qFuzzyCompare(roundTrip.skipPenalty, weights.skipPenalty));
    QVERIFY(qFuzzyCompare(roundTrip.sameArtistPenalty, weights.sameArtistPenalty));
}

void RadioTest::scoringWeightSpecsRoundTripThroughJson()
{
    const QVector<TrackScorer::WeightSpec> specs = TrackScorer::weightSpecs();
    QCOMPARE(specs.size(), 18);
    TrackScorer::Weights weights = TrackScorer::defaultWeights();
    for (const TrackScorer::WeightSpec &spec : specs) {
        QVERIFY(!spec.key.isEmpty());
        QVERIFY(!spec.label.isEmpty());
        QVERIFY(!spec.tooltip.isEmpty());
        QVERIFY(spec.minimum <= spec.defaultValue);
        QVERIFY(spec.defaultValue <= spec.maximum);
        double current = 0.0;
        QVERIFY(TrackScorer::weightValue(weights, spec.key, &current));
        QVERIFY(qFuzzyCompare(current, spec.defaultValue));
        const double adjusted = spec.maximum <= 0.0 ? spec.maximum : spec.minimum;
        QVERIFY(TrackScorer::setWeightValue(weights, spec.key, adjusted));
    }

    QString error;
    const TrackScorer::Weights roundTrip = TrackScorer::weightsFromJson(TrackScorer::weightsToJson(weights), &error);
    QVERIFY(error.isEmpty());
    for (const TrackScorer::WeightSpec &spec : specs) {
        double expected = 0.0;
        double actual = 0.0;
        QVERIFY(TrackScorer::weightValue(weights, spec.key, &expected));
        QVERIFY(TrackScorer::weightValue(roundTrip, spec.key, &actual));
        QVERIFY(qFuzzyCompare(expected, actual));
    }
}

void RadioTest::scoringWeightsJsonRejectsUnknownAndInvalidFields()
{
    QString error;
    TrackScorer::weightsFromJson(R"({"unknownWeight":1.0})", &error);
    QVERIFY(error.contains(QStringLiteral("unknownWeight")));

    TrackScorer::weightsFromJson(R"({"genreWeight":"loud"})", &error);
    QVERIFY(error.contains(QStringLiteral("genreWeight")));

    TrackScorer::weightsFromJson(R"({"recencyPenalty":1.0})", &error);
    QVERIFY(error.contains(QStringLiteral("recencyPenalty")));

    TrackScorer::weightsFromJson(R"({"energyWeight":-0.1})", &error);
    QVERIFY(error.contains(QStringLiteral("energyWeight")));

    TrackScorer::weightsFromJson(R"({"audioWeight":-0.1})", &error);
    QVERIFY(error.contains(QStringLiteral("audioWeight")));

    // Rejection is all-or-nothing: a valid key before the failing one must not
    // survive into the returned weights.
    const TrackScorer::Weights partial =
        TrackScorer::weightsFromJson(R"({"genreWeight":9.0,"skipPenalty":1.0})", &error);
    QVERIFY(error.contains(QStringLiteral("skipPenalty")));
    QVERIFY(qFuzzyCompare(partial.genreWeight, TrackScorer::defaultWeights().genreWeight));
}

void RadioTest::weightLearnerLearnsDirectionalMultipliers()
{
    QVector<WeightLearner::Sample> samples;
    for (int i = 0; i < 40; ++i) {
        samples.push_back(learnerSample(true, {
            {QStringLiteral("genre"), 1.0},
            {QStringLiteral("rating"), 0.0},
            {QStringLiteral("tempo"), 0.5},
        }));
    }
    for (int i = 0; i < 200; ++i) {
        samples.push_back(learnerSample(false, {
            {QStringLiteral("genre"), 0.0},
            {QStringLiteral("rating"), 1.0},
            {QStringLiteral("tempo"), 0.5},
        }));
    }

    const WeightLearner::Result result = WeightLearner::learn(samples);
    QVERIFY2(result.ok, qPrintable(result.error));
    QCOMPARE(result.sampleCount, 240);
    QCOMPARE(result.positiveLabels, 40);
    QVERIFY(learnedMultiplier(result, QStringLiteral("genre")) < 1.0);
    QVERIFY(learnedMultiplier(result, QStringLiteral("rating")) > 1.0);
    QVERIFY(std::abs(learnedMultiplier(result, QStringLiteral("tempo")) - 1.0) < 0.10);
}

void RadioTest::weightLearnerStrengthensVindicatedPenalties()
{
    // Skipped picks carried a strong (negative) recency contribution: the
    // penalty warned and the pick happened anyway. The suggestion must make
    // that penalty STRONGER (larger magnitude, still negative), not weaker.
    QVector<WeightLearner::Sample> samples;
    for (int i = 0; i < 40; ++i) {
        samples.push_back(learnerSample(true, {{QStringLiteral("recency"), -1.5}}));
    }
    for (int i = 0; i < 200; ++i) {
        samples.push_back(learnerSample(false, {{QStringLiteral("recency"), 0.0}}));
    }

    const WeightLearner::Result result = WeightLearner::learn(samples);
    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY(learnedMultiplier(result, QStringLiteral("recency")) > 1.0);
    QVERIFY(result.suggestedWeights.recencyPenalty
            < TrackScorer::defaultWeights().recencyPenalty);
    QVERIFY(result.suggestedWeights.recencyPenalty <= 0.0);
}

void RadioTest::weightLearnerWeakensUselessPenalties()
{
    // The penalized tracks were the ones the user LISTENED to; skips came
    // from unpenalized picks. The penalty is suppressing the wrong tracks
    // and must shrink toward zero while staying non-positive.
    QVector<WeightLearner::Sample> samples;
    for (int i = 0; i < 40; ++i) {
        samples.push_back(learnerSample(true, {{QStringLiteral("skips"), 0.0}}));
    }
    for (int i = 0; i < 200; ++i) {
        samples.push_back(learnerSample(false, {{QStringLiteral("skips"), -1.0}}));
    }

    const WeightLearner::Result result = WeightLearner::learn(samples);
    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY(learnedMultiplier(result, QStringLiteral("skips")) < 1.0);
    QVERIFY(result.suggestedWeights.skipPenalty
            > TrackScorer::defaultWeights().skipPenalty);
    QVERIFY(result.suggestedWeights.skipPenalty <= 0.0);
}

void RadioTest::weightLearnerIsDeterministic()
{
    QVector<WeightLearner::Sample> samples;
    for (int i = 0; i < 30; ++i) {
        samples.push_back(learnerSample(true, {{QStringLiteral("audio"), 1.0}}));
    }
    for (int i = 0; i < 200; ++i) {
        samples.push_back(learnerSample(false, {{QStringLiteral("audio"), 0.0},
                                                {QStringLiteral("novelty"), 1.0}}));
    }

    const WeightLearner::Result first = WeightLearner::learn(samples);
    const WeightLearner::Result second = WeightLearner::learn(samples);
    QVERIFY(first.ok);
    QVERIFY(second.ok);
    QCOMPARE(first.suggestedWeightsJson, second.suggestedWeightsJson);
    QCOMPARE(first.components.size(), second.components.size());
    for (qsizetype i = 0; i < first.components.size(); ++i) {
        QCOMPARE(first.components.at(i).componentName, second.components.at(i).componentName);
        QVERIFY(qFuzzyCompare(first.components.at(i).multiplier, second.components.at(i).multiplier));
    }
}

void RadioTest::weightLearnerClampsExtremeSuggestions()
{
    QVector<WeightLearner::Sample> samples;
    for (int i = 0; i < 60; ++i) {
        samples.push_back(learnerSample(true, {{QStringLiteral("genre"), 1.0},
                                               {QStringLiteral("rating"), 0.0}}));
    }
    for (int i = 0; i < 200; ++i) {
        samples.push_back(learnerSample(false, {{QStringLiteral("genre"), 0.0},
                                                {QStringLiteral("rating"), 1.0}}));
    }

    WeightLearner::Options options;
    options.iterations = 5000;
    options.l2Lambda = 0.0;
    const WeightLearner::Result result = WeightLearner::learn(samples, options);
    QVERIFY2(result.ok, qPrintable(result.error));
    QVERIFY(qFuzzyCompare(learnedMultiplier(result, QStringLiteral("genre")), 0.25));
    QVERIFY(qFuzzyCompare(learnedMultiplier(result, QStringLiteral("rating")), 4.0));
    QVERIFY(qFuzzyCompare(result.suggestedWeights.genreWeight,
                          TrackScorer::defaultWeights().genreWeight * 0.25));
    QVERIFY(qFuzzyCompare(result.suggestedWeights.ratingWeight,
                          TrackScorer::defaultWeights().ratingWeight * 4.0));
}

void RadioTest::weightLearnerRefusesSparseData()
{
    QVector<WeightLearner::Sample> samples;
    samples.push_back(learnerSample(true, {{QStringLiteral("genre"), 1.0}}));
    const WeightLearner::Result tooFew = WeightLearner::learn(samples);
    QVERIFY(!tooFew.ok);
    QVERIFY(tooFew.error.contains(QStringLiteral("have 1, need 200")));

    samples.clear();
    for (int i = 0; i < 200; ++i) {
        samples.push_back(learnerSample(false, {{QStringLiteral("genre"), 0.5}}));
    }
    const WeightLearner::Result noSkips = WeightLearner::learn(samples);
    QVERIFY(!noSkips.ok);
    QVERIFY(noSkips.error.contains(QStringLiteral("have 0, need 20")));
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

void RadioTest::tempoAndEnergyUseSonicProximity()
{
    TrackScorer::SeedContext seed;
    seed.contextTempoBpm = 120.0;
    seed.contextEnergy = 0.75;

    TrackScorer::Weights weights = TrackScorer::defaultWeights();
    weights.tempoWeight = 0.4;
    weights.energyWeight = 0.6;

    const TrackScorer::Scored adjacent = TrackScorer::score(
        makeCandidate(QStringLiteral("/near"), QStringLiteral("a"), {}, 0, -1, false,
                      QStringLiteral("album"), QStringLiteral("song"), 150.0, 0.25),
        {}, seed, weights);
    QVERIFY(qFuzzyCompare(componentValue(adjacent, QStringLiteral("tempo")), 0.4 * 0.5));
    QVERIFY(qFuzzyCompare(componentValue(adjacent, QStringLiteral("energy")), 0.6 * 0.5));

    const TrackScorer::Scored exact = TrackScorer::score(
        makeCandidate(QStringLiteral("/exact"), QStringLiteral("a"), {}, 0, -1, false,
                      QStringLiteral("album"), QStringLiteral("song"), 120.0, 0.75),
        {}, seed, weights);
    QVERIFY(qFuzzyCompare(componentValue(exact, QStringLiteral("tempo")), 0.4));
    QVERIFY(qFuzzyCompare(componentValue(exact, QStringLiteral("energy")), 0.6));

    const TrackScorer::Scored distant = TrackScorer::score(
        makeCandidate(QStringLiteral("/far"), QStringLiteral("a"), {}, 0, -1, false,
                      QStringLiteral("album"), QStringLiteral("song"), 181.0, 1.9),
        {}, seed, weights);
    QVERIFY(!hasComponent(distant, QStringLiteral("tempo")));
    QVERIFY(!hasComponent(distant, QStringLiteral("energy")));
}

void RadioTest::unknownTempoOrEnergyYieldsNoComponent()
{
    TrackScorer::SeedContext knownContext;
    knownContext.contextTempoBpm = 120.0;
    knownContext.contextEnergy = 0.5;

    const TrackScorer::Scored unknownCandidate = TrackScorer::score(
        makeCandidate(QStringLiteral("/unknown"), QStringLiteral("a"), {}), {}, knownContext);
    QVERIFY(!hasComponent(unknownCandidate, QStringLiteral("tempo")));
    QVERIFY(!hasComponent(unknownCandidate, QStringLiteral("energy")));

    TrackScorer::SeedContext unknownContext;
    const TrackScorer::Scored knownCandidate = TrackScorer::score(
        makeCandidate(QStringLiteral("/known"), QStringLiteral("a"), {}, 0, -1, false,
                      QStringLiteral("album"), QStringLiteral("song"), 120.0, 0.5),
        {}, unknownContext);
    QVERIFY(!hasComponent(knownCandidate, QStringLiteral("tempo")));
    QVERIFY(!hasComponent(knownCandidate, QStringLiteral("energy")));
}

void RadioTest::audioComponentUsesEmbeddingCosine()
{
    const QHash<qint64, QVector<float>> embeddings{
        {10, QVector<float>{1.0F, 0.0F}},
        {11, QVector<float>{0.6F, 0.8F}},
        {12, QVector<float>{-1.0F, 0.0F}},
    };
    TrackScorer::SeedContext seed;
    seed.audioCentroid = {1.0F, 0.0F};
    seed.embeddingsByGroup = &embeddings;

    TrackScorer::Weights weights = TrackScorer::defaultWeights();
    weights.audioWeight = 2.0;

    TrackScorer::Candidate same = makeCandidate(QStringLiteral("/same"), QStringLiteral("a"), {});
    same.contentGroupId = 10;
    TrackScorer::Candidate near = makeCandidate(QStringLiteral("/near"), QStringLiteral("b"), {});
    near.contentGroupId = 11;
    TrackScorer::Candidate inverse = makeCandidate(QStringLiteral("/inverse"), QStringLiteral("c"), {});
    inverse.contentGroupId = 12;

    const TrackScorer::Scored sameScored = TrackScorer::score(same, {}, seed, weights);
    QVERIFY(qFuzzyCompare(componentValue(sameScored, QStringLiteral("audio")), 2.0));

    const TrackScorer::Scored nearScored = TrackScorer::score(near, {}, seed, weights);
    QVERIFY(std::abs(componentValue(nearScored, QStringLiteral("audio")) - 1.2) < 0.000001);

    const TrackScorer::Scored inverseScored = TrackScorer::score(inverse, {}, seed, weights);
    QVERIFY(!hasComponent(inverseScored, QStringLiteral("audio")));
}

void RadioTest::unknownAudioEmbeddingYieldsNoComponent()
{
    const QHash<qint64, QVector<float>> embeddings{
        {10, QVector<float>{1.0F, 0.0F}},
        {11, QVector<float>{1.0F}},
    };
    TrackScorer::Candidate candidate = makeCandidate(QStringLiteral("/known"), QStringLiteral("a"), {});
    candidate.contentGroupId = 10;

    TrackScorer::SeedContext noCentroid;
    noCentroid.embeddingsByGroup = &embeddings;
    QVERIFY(!hasComponent(TrackScorer::score(candidate, {}, noCentroid), QStringLiteral("audio")));

    TrackScorer::SeedContext noLookup;
    noLookup.audioCentroid = {1.0F, 0.0F};
    QVERIFY(!hasComponent(TrackScorer::score(candidate, {}, noLookup), QStringLiteral("audio")));

    TrackScorer::SeedContext knownContext;
    knownContext.audioCentroid = {1.0F, 0.0F};
    knownContext.embeddingsByGroup = &embeddings;
    TrackScorer::Candidate missing = makeCandidate(QStringLiteral("/missing"), QStringLiteral("b"), {});
    missing.contentGroupId = 999;
    QVERIFY(!hasComponent(TrackScorer::score(missing, {}, knownContext), QStringLiteral("audio")));

    TrackScorer::Candidate mismatched = makeCandidate(QStringLiteral("/mismatch"), QStringLiteral("c"), {});
    mismatched.contentGroupId = 11;
    QVERIFY(!hasComponent(TrackScorer::score(mismatched, {}, knownContext), QStringLiteral("audio")));
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

void RadioTest::songKeyDedupUsesRecordingMbid()
{
    const QString sharedSong = FoldKey::songKey(QStringLiteral("recording-1"), {}, {});
    QVector<TrackScorer::Candidate> pool{
        makeCandidate(QStringLiteral("/copy-a"), QStringLiteral("artist-a"), {QStringLiteral("rock")}, 2000,
                      100, true, QStringLiteral("album-a"), sharedSong),
        makeCandidate(QStringLiteral("/copy-b"), QStringLiteral("artist-b"), {QStringLiteral("rock")}, 2000,
                      100, true, QStringLiteral("album-b"), sharedSong),
    };
    TrackScorer::Candidate seed = makeCandidate(QStringLiteral("/seed"), QStringLiteral("seed"),
                                                {QStringLiteral("rock")}, 2000);
    QRandomGenerator rng(11u);
    RadioSession session(pool, {}, {}, seed, 30, 1'000'000'000, &rng);

    const QVector<Track> picks = session.nextTracks(2, {}, resolvePathToTrack);
    QCOMPARE(picks.size(), 1);
    QVERIFY(picks.first().path == QStringLiteral("/copy-a") || picks.first().path == QStringLiteral("/copy-b"));
}

void RadioTest::songKeyDedupFallsBackToArtistTitle()
{
    const QString sharedSong = FoldKey::songKey({}, QStringLiteral("  The Artist  "),
                                                QStringLiteral("  The Song  "));
    QVector<TrackScorer::Candidate> pool{
        makeCandidate(QStringLiteral("/lossless.flac"), QStringLiteral("artist-a"), {QStringLiteral("rock")},
                      2000, 100, true, QStringLiteral("album-a"), sharedSong),
        makeCandidate(QStringLiteral("/portable.opus"), QStringLiteral("artist-b"), {QStringLiteral("rock")},
                      2000, 100, true, QStringLiteral("album-b"), sharedSong),
    };
    TrackScorer::Candidate seed = makeCandidate(QStringLiteral("/seed"), QStringLiteral("seed"),
                                                {QStringLiteral("rock")}, 2000);
    QRandomGenerator rng(12u);
    RadioSession session(pool, {}, {}, seed, 30, 1'000'000'000, &rng);

    const QVector<Track> picks = session.nextTracks(2, {}, resolvePathToTrack);
    QCOMPARE(picks.size(), 1);
    QVERIFY(picks.first().path == QStringLiteral("/lossless.flac")
            || picks.first().path == QStringLiteral("/portable.opus"));
}

void RadioTest::albumCapUsesReleaseGroupKey()
{
    const QString sharedAlbum = FoldKey::albumGroupKey(QStringLiteral("release-group-1"),
                                                       QStringLiteral("Album Artist"),
                                                       QStringLiteral("Original Edition"));
    QVector<TrackScorer::Candidate> pool;
    for (int i = 0; i < 3; ++i) {
        pool.push_back(makeCandidate(QStringLiteral("/rg%1").arg(i), QStringLiteral("rg-artist%1").arg(i),
                                     {QStringLiteral("rock")}, 2000, 100, true, sharedAlbum));
    }
    for (int i = 0; i < 6; ++i) {
        pool.push_back(makeCandidate(QStringLiteral("/other%1").arg(i), QStringLiteral("other%1").arg(i),
                                     {QStringLiteral("rock")}, 2000, 30, false,
                                     QStringLiteral("other%1\nalbum").arg(i)));
    }

    TrackScorer::Candidate seed = makeCandidate(QStringLiteral("/seed"), QStringLiteral("seed"),
                                                {QStringLiteral("rock")}, 2000);
    QRandomGenerator rng(13u);
    RadioSession session(pool, {}, {}, seed, 30, 1'000'000'000, &rng);

    const QVector<Track> picks = session.nextTracks(9, {}, resolvePathToTrack);
    int albumCount = 0;
    for (const Track &pick : picks) {
        if (pick.path.startsWith(QStringLiteral("/rg"))) {
            ++albumCount;
        }
    }
    QVERIFY2(albumCount <= 2, "more than two tracks from one release group in a session");
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

void RadioTest::rollingSonicContextUsesPlayedMean()
{
    QVector<TrackScorer::Candidate> pool{
        makeCandidate(QStringLiteral("/target"), QStringLiteral("target"), {}, 0, -1, false,
                      QStringLiteral("album-target"), QStringLiteral("song-target"), 110.0, 0.5),
        makeCandidate(QStringLiteral("/feed-a"), QStringLiteral("feed-a"), {}, 0, -1, false,
                      QStringLiteral("album-feed-a"), QStringLiteral("song-feed-a"), 90.0, 0.2),
        makeCandidate(QStringLiteral("/feed-b"), QStringLiteral("feed-b"), {}, 0, -1, false,
                      QStringLiteral("album-feed-b"), QStringLiteral("song-feed-b"), 130.0, 0.8),
        makeCandidate(QStringLiteral("/feed-unknown"), QStringLiteral("feed-unknown"), {}, 0, -1, false,
                      QStringLiteral("album-feed-u"), QStringLiteral("song-feed-u")),
    };
    TrackScorer::Candidate seed = makeCandidate(QStringLiteral("/seed"), QStringLiteral("seed"), {}, 0, -1, false,
                                                QStringLiteral("album-seed"), QStringLiteral("song-seed"), 60.0, 0.0);
    QRandomGenerator rng(7u);
    RadioSession session(pool, {}, {}, seed, 30, 1'000'000'000, &rng);

    session.notePlayed(resolvePathToTrack(QStringLiteral("/feed-a")));
    session.notePlayed(resolvePathToTrack(QStringLiteral("/feed-b")));
    session.notePlayed(resolvePathToTrack(QStringLiteral("/feed-unknown")));

    const QVector<Track> picks = session.nextTracks(1, {}, resolvePathToTrack);
    QCOMPARE(picks.size(), 1);
    QCOMPARE(picks.first().path, QStringLiteral("/target"));

    const QList<TrackScorer::Component> components = session.reasonComponentsFor(QStringLiteral("/target"));
    QVERIFY(qFuzzyCompare(componentValue(components, QStringLiteral("tempo")), 0.4));
    QVERIFY(qFuzzyCompare(componentValue(components, QStringLiteral("energy")), 0.6));
}

void RadioTest::substitutedBestCopyFeedsRollingContext()
{
    QVector<TrackScorer::Candidate> pool{
        makeCandidate(QStringLiteral("/dup-a"), QStringLiteral("dup"), {}, 0, -1, false,
                      QStringLiteral("album-dup"), QStringLiteral("song-dup"), 100.0, 0.4),
        makeCandidate(QStringLiteral("/target"), QStringLiteral("target"), {}, 0, -1, false,
                      QStringLiteral("album-target"), QStringLiteral("song-target"), 100.0, 0.4),
    };
    TrackScorer::Candidate seed = makeCandidate(QStringLiteral("/seed"), QStringLiteral("seed"), {});
    QRandomGenerator rng(9u);
    RadioSession session(pool, {}, {}, seed, 30, 1'000'000'000, &rng);

    // Force the dup pick, substituted by the resolver to a non-pool best copy.
    const QVector<Track> firstPicks =
        session.nextTracks(1, {QStringLiteral("/target")}, [](const QString &path) {
            Q_UNUSED(path);
            return playedTrack(QStringLiteral("/dup-best"), QStringLiteral("dup"));
        });
    QCOMPARE(firstPicks.size(), 1);
    QCOMPARE(firstPicks.first().path, QStringLiteral("/dup-best"));

    session.notePlayed(firstPicks.first());

    const QVector<Track> picks = session.nextTracks(1, {}, resolvePathToTrack);
    QCOMPARE(picks.size(), 1);
    QCOMPARE(picks.first().path, QStringLiteral("/target"));

    // The played best copy resolves through the aliased candidate row, so the
    // rolling sonic context knows its scalars and the target scores on them.
    const QList<TrackScorer::Component> components = session.reasonComponentsFor(QStringLiteral("/target"));
    QVERIFY(qFuzzyCompare(componentValue(components, QStringLiteral("tempo")), 0.4));
    QVERIFY(qFuzzyCompare(componentValue(components, QStringLiteral("energy")), 0.6));
}

void RadioTest::rollingAudioContextUsesSeedAndPlayedEmbeddings()
{
    const QHash<qint64, QVector<float>> embeddings{
        {1, QVector<float>{1.0F, 0.0F}},
        {2, QVector<float>{0.0F, 1.0F}},
        {3, QVector<float>{0.0F, 1.0F}},
        {4, QVector<float>{0.4472136F, 0.8944272F}},
    };
    QVector<TrackScorer::Candidate> pool{
        makeCandidate(QStringLiteral("/target"), QStringLiteral("target"), {}, 0, -1, false,
                      QStringLiteral("album-target"), QStringLiteral("song-target"), -1.0, -1.0, 4),
        makeCandidate(QStringLiteral("/feed-a"), QStringLiteral("feed-a"), {}, 0, -1, false,
                      QStringLiteral("album-feed-a"), QStringLiteral("song-feed-a"), -1.0, -1.0, 2),
        makeCandidate(QStringLiteral("/feed-b"), QStringLiteral("feed-b"), {}, 0, -1, false,
                      QStringLiteral("album-feed-b"), QStringLiteral("song-feed-b"), -1.0, -1.0, 3),
        makeCandidate(QStringLiteral("/feed-unknown"), QStringLiteral("feed-unknown"), {}, 0, -1, false,
                      QStringLiteral("album-feed-u"), QStringLiteral("song-feed-u")),
    };
    TrackScorer::Candidate seed = makeCandidate(QStringLiteral("/seed"), QStringLiteral("seed"), {}, 0, -1, false,
                                                QStringLiteral("album-seed"), QStringLiteral("song-seed"),
                                                -1.0, -1.0, 1);
    QRandomGenerator rng(8u);
    RadioSession session(pool, {}, {}, seed, 30, 1'000'000'000, &rng,
                         TrackScorer::defaultWeights(), embeddings);

    session.notePlayed(resolvePathToTrack(QStringLiteral("/feed-a")));
    session.notePlayed(resolvePathToTrack(QStringLiteral("/feed-b")));
    session.notePlayed(resolvePathToTrack(QStringLiteral("/feed-unknown")));

    const QVector<Track> picks = session.nextTracks(1, {}, resolvePathToTrack);
    QCOMPARE(picks.size(), 1);
    QCOMPARE(picks.first().path, QStringLiteral("/target"));

    const QList<TrackScorer::Component> components = session.reasonComponentsFor(QStringLiteral("/target"));
    QVERIFY(std::abs(componentValue(components, QStringLiteral("audio")) - 1.2) < 0.000001);
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

void RadioTest::reasonComponentsRoundTripFromPick()
{
    QVector<TrackScorer::Candidate> pool{makeCandidate(QStringLiteral("/t0"), QStringLiteral("a"),
                                                       {QStringLiteral("rock")}, 2000, 90, true)};
    TrackScorer::Candidate seed = makeCandidate(QStringLiteral("/seed"), QStringLiteral("seed"),
                                                {QStringLiteral("rock")}, 2000);
    QRandomGenerator rng(1u);
    RadioSession session(pool, {}, {}, seed, 30, 1'000'000'000, &rng);

    const QVector<Track> picks = session.nextTracks(1, {}, resolvePathToTrack);
    QCOMPARE(picks.size(), 1);
    QVERIFY(!session.reasonComponentsFor(picks.first().path).isEmpty());
    QVERIFY(session.reasonComponentsFor(QStringLiteral("/never")).isEmpty());
}

void RadioTest::resolvedPickStoresReasonUnderResolvedPath()
{
    QVector<TrackScorer::Candidate> pool{
        makeCandidate(QStringLiteral("/low-quality.flac"), QStringLiteral("a"), {QStringLiteral("rock")}, 2000, 90, true,
                      QStringLiteral("album-low"), QStringLiteral("song:shared")),
        makeCandidate(QStringLiteral("/best-copy.flac"), QStringLiteral("b"), {QStringLiteral("rock")}, 2000, 50, true,
                      QStringLiteral("album-best"), QStringLiteral("song:other")),
    };
    TrackScorer::Candidate seed = makeCandidate(QStringLiteral("/seed"), QStringLiteral("seed"),
                                                {QStringLiteral("rock")}, 2000);
    QRandomGenerator rng(1u);
    RadioSession session(pool, {}, {}, seed, 30, 1'000'000'000, &rng);

    const QSet<QString> excludeBest{QStringLiteral("/best-copy.flac")};
    const auto resolveBestCopy = [](const QString &path) {
        return resolvePathToTrack(path == QStringLiteral("/low-quality.flac")
                                      ? QStringLiteral("/best-copy.flac")
                                      : path);
    };

    const QVector<Track> picks = session.nextTracks(1, excludeBest, resolveBestCopy);
    QCOMPARE(picks.size(), 1);
    QCOMPARE(picks.first().path, QStringLiteral("/best-copy.flac"));
    QVERIFY(!session.reasonComponentsFor(QStringLiteral("/low-quality.flac")).isEmpty());
    QVERIFY(!session.reasonComponentsFor(QStringLiteral("/best-copy.flac")).isEmpty());

    const QVector<RadioSession::PickReason> reasons = session.pickReasons();
    QCOMPARE(reasons.size(), 1);
    QCOMPARE(reasons.first().path, QStringLiteral("/best-copy.flac"));

    const QJsonArray usedPaths = session.constraintState().value(QStringLiteral("usedPaths")).toArray();
    QStringList used;
    used.reserve(usedPaths.size());
    for (const QJsonValue &value : usedPaths) {
        used.push_back(value.toString());
    }
    QVERIFY(used.contains(QStringLiteral("/low-quality.flac")));
    QVERIFY(used.contains(QStringLiteral("/best-copy.flac")));

    const QVector<Track> later = session.nextTracks(2, {}, resolvePathToTrack);
    QVERIFY(later.isEmpty());
}

void RadioTest::pickReasonsEnumeratesStoredComponents()
{
    QVector<TrackScorer::Candidate> pool{
        makeCandidate(QStringLiteral("/t0"), QStringLiteral("a"), {QStringLiteral("rock")}, 2000, 90, true),
        makeCandidate(QStringLiteral("/t1"), QStringLiteral("b"), {QStringLiteral("rock")}, 2001, 70, true),
    };
    TrackScorer::Candidate seed = makeCandidate(QStringLiteral("/seed"), QStringLiteral("seed"),
                                                {QStringLiteral("rock")}, 2000);
    QRandomGenerator rng(1u);
    RadioSession session(pool, {}, {}, seed, 30, 1'000'000'000, &rng);

    const QVector<Track> picks = session.nextTracks(2, {}, resolvePathToTrack);
    QCOMPARE(picks.size(), 2);

    const QVector<RadioSession::PickReason> reasons = session.pickReasons();
    QCOMPARE(reasons.size(), picks.size());
    for (qsizetype i = 0; i < reasons.size(); ++i) {
        QCOMPARE(reasons.at(i).path, picks.at(i).path);
        const QList<TrackScorer::Component> byPath = session.reasonComponentsFor(reasons.at(i).path);
        QCOMPARE(reasons.at(i).components.size(), byPath.size());
        for (qsizetype j = 0; j < reasons.at(i).components.size(); ++j) {
            QCOMPARE(reasons.at(i).components.at(j).name, byPath.at(j).name);
            QCOMPARE(reasons.at(i).components.at(j).value, byPath.at(j).value);
        }
    }
}

void RadioTest::reasonSentencePicksTopPhrases()
{
    const QList<TrackScorer::Component> components{
        {QStringLiteral("genre"), 2.1},
        {QStringLiteral("rating"), 1.4},
        {QStringLiteral("novelty"), 0.1},
        {QStringLiteral("recency"), -0.6},
    };

    QCOMPARE(ReasonText::sentence(components),
             QStringLiteral("Radio pick — matches the session's mood · you rate it highly "
	                            "(held back: heard recently)"));
}

void RadioTest::reasonSentenceNamesAudio()
{
    const QList<TrackScorer::Component> components{
        {QStringLiteral("audio"), 1.2},
        {QStringLiteral("energy"), 0.6},
    };

    QCOMPARE(ReasonText::sentence(components),
             QStringLiteral("Radio pick — sounds similar · similar energy"));
}

void RadioTest::reasonSentenceNamesTempoAndEnergy()
{
    const QList<TrackScorer::Component> components{
        {QStringLiteral("tempo"), 0.4},
        {QStringLiteral("energy"), 0.6},
    };

    QCOMPARE(ReasonText::sentence(components),
             QStringLiteral("Radio pick — similar energy · matches the pace"));
}

void RadioTest::reasonSentenceHandlesPenaltyOnly()
{
    const QList<TrackScorer::Component> components{
        {QStringLiteral("skips"), -0.8},
        {QStringLiteral("recency"), -0.6},
    };

    QCOMPARE(ReasonText::sentence(components),
             QStringLiteral("Radio pick — held back: often skipped early · heard recently"));
}

void RadioTest::reasonSentenceEmptyOnEmptyInput()
{
    QVERIFY(ReasonText::sentence({}).isEmpty());
}

void RadioTest::reasonBreakdownFormatsSigned()
{
    const QList<TrackScorer::Component> components{
        {QStringLiteral("recency"), -0.6},
        {QStringLiteral("genre"), 2.1},
    };

    QCOMPARE(ReasonText::breakdown(components), QStringLiteral("genre +2.1 · recency -0.6"));
}

void RadioTest::setExplorationTakesEffectOnSubsequentPicks()
{
    // Two candidates, no genre/year/rating overlap with the seed and no
    // affinity history: the only component in play is novelty, which scales
    // directly with the exploration knob (see
    // RadioTest::noveltyAtZeroHistoryScalesWithExploration for the same math
    // against TrackScorer directly).
    QVector<TrackScorer::Candidate> pool{
        makeCandidate(QStringLiteral("/a"), QStringLiteral("artist-a"), {}),
        makeCandidate(QStringLiteral("/b"), QStringLiteral("artist-b"), {}),
    };
    TrackScorer::Candidate seed = makeCandidate(QStringLiteral("/seed"), QStringLiteral("seed"), {});
    QRandomGenerator rng(1u);
    RadioSession session(pool, {}, {}, seed, /*exploration=*/0, 1'000'000'000, &rng);

    const QVector<Track> first = session.nextTracks(1, {}, resolvePathToTrack);
    QCOMPARE(first.size(), 1);
    // kNoveltyWeight(0.8) * noveltyRatio(1.0) * explorationScale(0.5 + 0/100).
    QVERIFY2(session.reasonFor(first.first().path).contains(QStringLiteral("novelty +0.4")),
             qPrintable(session.reasonFor(first.first().path)));

    session.setExploration(100);
    const QVector<Track> second = session.nextTracks(1, {}, resolvePathToTrack);
    QCOMPARE(second.size(), 1);
    // kNoveltyWeight(0.8) * noveltyRatio(1.0) * explorationScale(0.5 + 100/100).
    QVERIFY2(session.reasonFor(second.first().path).contains(QStringLiteral("novelty +1.2")),
             qPrintable(session.reasonFor(second.first().path)));
}

void RadioTest::batchOfFifteenRespectsThrottlesAndIsDistinct()
{
    // A single-pick call already can't repeat a path or an artist within the
    // throttle window (see artistThrottleNeverPicksSameArtistConsecutively /
    // noRepeatsWithinSession); this pins the same guarantees for the actual
    // YT-Music-style batch size AppCore now requests at once (15).
    QVector<TrackScorer::Candidate> pool;
    for (int i = 0; i < 5; ++i) {
        pool.push_back(makeCandidate(QStringLiteral("/hot%1").arg(i), QStringLiteral("hot"),
                                     {QStringLiteral("rock")}, 2000, 100, true,
                                     QStringLiteral("hot\nalbum%1").arg(i)));
    }
    for (int i = 0; i < 15; ++i) {
        pool.push_back(makeCandidate(QStringLiteral("/f%1").arg(i), QStringLiteral("filler%1").arg(i),
                                     {QStringLiteral("rock")}, 2000, 40, false));
    }
    TrackScorer::Candidate seed = makeCandidate(QStringLiteral("/seed"), QStringLiteral("seed"),
                                                {QStringLiteral("rock")}, 2000);
    QRandomGenerator rng(2024u);
    RadioSession session(pool, {}, {}, seed, 30, 1'000'000'000, &rng);

    const QVector<Track> picks = session.nextTracks(15, {}, resolvePathToTrack);
    QCOMPARE(picks.size(), 15);

    const auto artistOf = [](const QString &path) {
        return path.startsWith(QStringLiteral("/hot")) ? QStringLiteral("hot") : path.mid(2);
    };
    QSet<QString> seen;
    for (int i = 0; i < picks.size(); ++i) {
        QVERIFY2(!seen.contains(picks.at(i).path), "a path repeated within a 15-pick batch");
        seen.insert(picks.at(i).path);
        if (i > 0) {
            QVERIFY2(artistOf(picks.at(i - 1).path) != artistOf(picks.at(i).path),
                     "same artist picked back-to-back in a 15-pick batch");
        }
    }
}

void RadioTest::isEarlySkipUsesHalfDurationCappedAtFourMinutes()
{
    // Half-duration rule for a short track (mirrors the ListenHistoryStore /
    // ListenTracker examples: a 4-minute track's threshold is 120 s).
    QVERIFY(RadioSession::isEarlySkip(30000, 240000));
    QVERIFY(!RadioSession::isEarlySkip(200000, 240000));

    // The 4-minute cap kicks in for a long track (here ~16.7 min): half the
    // duration would be 500 s, but the threshold caps at 240 s.
    QVERIFY(RadioSession::isEarlySkip(200000, 1000000));
    QVERIFY(!RadioSession::isEarlySkip(250000, 1000000));

    // Unknown duration (<= 0) falls back to the 4-minute cap outright.
    QVERIFY(RadioSession::isEarlySkip(200000, 0));
    QVERIFY(!RadioSession::isEarlySkip(250000, 0));
}

void RadioTest::anchorlessSessionDriftsFromPlayedContext()
{
    const auto buildPool = []() {
        return QVector<TrackScorer::Candidate>{
            makeCandidate(QStringLiteral("/feed"), QStringLiteral("feed"), {QStringLiteral("rock")}, 2000),
            makeCandidate(QStringLiteral("/rock"), QStringLiteral("rock"), {QStringLiteral("rock")}, 2000),
        };
    };
    const QHash<QString, double> genreIdf{{QStringLiteral("rock"), 2.0}};

    QRandomGenerator rngA(44u);
    RadioSession noContext(buildPool(), {}, genreIdf, 30, 1'000'000'000, &rngA);
    const QVector<Track> before = noContext.nextTracks(1, {}, resolvePathToTrack);
    QCOMPARE(before.size(), 1);
    QVERIFY2(!noContext.reasonFor(before.first().path).contains(QStringLiteral("genre")),
             "anchorless session had genre context before notePlayed");

    QRandomGenerator rngB(44u);
    RadioSession drifted(buildPool(), {}, genreIdf, 30, 1'000'000'000, &rngB);
    drifted.notePlayed(resolvePathToTrack(QStringLiteral("/feed")));
    const QVector<Track> after = drifted.nextTracks(1, {}, resolvePathToTrack);
    QCOMPARE(after.size(), 1);
    QCOMPARE(after.first().path, QStringLiteral("/rock"));
    QVERIFY2(drifted.reasonFor(after.first().path).contains(QStringLiteral("genre")),
             "anchorless session did not use notePlayed genre context");
}

void RadioTest::syntheticArtistSeedThrottlesSeedArtistAndIsNotPickable()
{
    QVector<TrackScorer::Candidate> pool{
        makeCandidate(QStringLiteral("/same"), QStringLiteral("seed artist"), {QStringLiteral("rock")}, 2000, 100, true),
        makeCandidate(QStringLiteral("/other"), QStringLiteral("other artist"), {QStringLiteral("rock")}, 2000, 60, true),
    };
    TrackScorer::Candidate seed = ArtistRadio::syntheticSeedCandidate(
        QStringLiteral("Seed Artist"), {QStringLiteral("rock")}, 2000);
    QRandomGenerator rng(7u);
    RadioSession session(pool, {}, {{QStringLiteral("rock"), 2.0}}, seed, 30, 1'000'000'000, &rng);

    const QVector<Track> picks = session.nextTracks(2, {}, resolvePathToTrack);
    QCOMPARE(picks.size(), 1);
    QCOMPARE(picks.first().path, QStringLiteral("/other"));
    for (const Track &pick : picks) {
        QVERIFY(!pick.path.isEmpty());
    }
}

void RadioTest::foreignNotePlayedEntersRollingConstraints()
{
    const QString foreignSongKey = FoldKey::songKey(QStringLiteral("foreign-rec"), QStringLiteral("Foreign"),
                                                    QStringLiteral("Manual Song"));
    const QString foreignAlbum = FoldKey::albumKey(QStringLiteral("Foreign"), QStringLiteral("Manual Album"));
    const QVector<TrackScorer::Candidate> pool{
        makeCandidate(QStringLiteral("/manual-foreign"), QStringLiteral("foreign"),
                      {QStringLiteral("rock")}, 2000, 100, true,
                      foreignAlbum, foreignSongKey),
        makeCandidate(QStringLiteral("/same-artist"), QStringLiteral("foreign"),
                      {QStringLiteral("rock")}, 2000, 100, true,
                      QStringLiteral("same-artist\nalbum"), QStringLiteral("song:same-artist")),
        makeCandidate(QStringLiteral("/same-song"), QStringLiteral("other"),
                      {QStringLiteral("rock")}, 2000, 100, true,
                      QStringLiteral("same-song\nalbum"), foreignSongKey),
        makeCandidate(QStringLiteral("/same-album"), QStringLiteral("album-a"),
                      {QStringLiteral("rock")}, 2000, 100, true,
                      foreignAlbum, QStringLiteral("song:album-a")),
        makeCandidate(QStringLiteral("/same-album-2"), QStringLiteral("album-b"),
                      {QStringLiteral("rock")}, 2000, 100, true,
                      foreignAlbum, QStringLiteral("song:album-b")),
        makeCandidate(QStringLiteral("/same-album-3"), QStringLiteral("album-c"),
                      {QStringLiteral("rock")}, 2000, 100, true,
                      foreignAlbum, QStringLiteral("song:album-c")),
        makeCandidate(QStringLiteral("/rock-target"), QStringLiteral("target"),
                      {QStringLiteral("rock")}, 2000, 100, true,
                      QStringLiteral("target\nalbum"), QStringLiteral("song:target")),
    };
    const QHash<QString, double> genreIdf{{QStringLiteral("rock"), 2.0}};
    RadioSession session(pool, {}, genreIdf, 30, 1'000'000'000);

    Track foreign;
    foreign.path = QStringLiteral("/manual-foreign");
    foreign.title = QStringLiteral("Manual Song");
    foreign.artistName = QStringLiteral("Foreign");
    foreign.albumArtistName = QStringLiteral("Foreign");
    foreign.albumTitle = QStringLiteral("Manual Album");
    foreign.musicBrainz.recordingId = QStringLiteral("foreign-rec");
    foreign.musicBrainz.releaseGroupId = QStringLiteral("foreign-rg");
    session.notePlayed(foreign);

    const QVector<Track> picks = session.nextTracks(10, {}, resolvePathToTrack);
    QSet<QString> pickedPaths;
    for (const Track &pick : picks) {
        pickedPaths.insert(pick.path);
    }

    QVERIFY(!pickedPaths.contains(QStringLiteral("/same-artist")));
    QVERIFY(!pickedPaths.contains(QStringLiteral("/same-song")));
    int sameAlbumPicks = 0;
    for (const QString &path : {QStringLiteral("/same-album"), QStringLiteral("/same-album-2"),
                               QStringLiteral("/same-album-3")}) {
        if (pickedPaths.contains(path)) {
            ++sameAlbumPicks;
        }
    }
    QCOMPARE(sameAlbumPicks, 1);
    QVERIFY(pickedPaths.contains(QStringLiteral("/rock-target")));
    QVERIFY2(session.reasonFor(QStringLiteral("/rock-target")).contains(QStringLiteral("genre")),
             "foreign notePlayed track did not enter the rolling genre context");
}

void RadioTest::constraintStateRoundTripPreservesSequencing()
{
    const QString usedSongKey = QStringLiteral("song:already-used");
    const QString cappedAlbum = QStringLiteral("album:already-capped");
    QVector<TrackScorer::Candidate> pool{
        makeCandidate(QStringLiteral("/used-a"), QStringLiteral("used-a"), {}, 0, -1, false,
                      QStringLiteral("album:used-a"), usedSongKey),
        makeCandidate(QStringLiteral("/used-b"), QStringLiteral("used-b"), {}, 0, -1, false,
                      QStringLiteral("album:used-b"), usedSongKey),
        makeCandidate(QStringLiteral("/album-a"), QStringLiteral("album-a"), {}, 0, -1, false,
                      cappedAlbum, QStringLiteral("song:album-a")),
        makeCandidate(QStringLiteral("/album-b"), QStringLiteral("album-b"), {}, 0, -1, false,
                      cappedAlbum, QStringLiteral("song:album-b")),
        makeCandidate(QStringLiteral("/album-c"), QStringLiteral("album-c"), {}, 0, -1, false,
                      cappedAlbum, QStringLiteral("song:album-c")),
        makeCandidate(QStringLiteral("/recent-feed"), QStringLiteral("recent"), {}, 0, -1, false,
                      QStringLiteral("album:recent-feed"), QStringLiteral("song:recent-feed")),
        makeCandidate(QStringLiteral("/recent-c"), QStringLiteral("recent"), {}, 0, -1, false,
                      QStringLiteral("album:recent-c"), QStringLiteral("song:recent-c")),
        makeCandidate(QStringLiteral("/allowed"), QStringLiteral("allowed"), {}, 0, -1, false,
                      QStringLiteral("album:allowed"), QStringLiteral("song:allowed")),
    };

    RadioSession original(pool, {}, {}, 30, 1'000'000'000);
    original.notePlayed(playedTrack(QStringLiteral("/used-a"), QStringLiteral("Used A")));
    original.notePlayed(playedTrack(QStringLiteral("/album-a"), QStringLiteral("Album A")));
    original.notePlayed(playedTrack(QStringLiteral("/album-b"), QStringLiteral("Album B")));
    original.notePlayed(playedTrack(QStringLiteral("/recent-feed"), QStringLiteral("Recent")));

    RadioSession restored(pool, {}, {}, 30, 1'000'000'000);
    restored.restoreConstraintState(original.constraintState());
    const QVector<Track> picks = restored.nextTracks(10, {}, resolvePathToTrack);

    QSet<QString> pickedPaths;
    for (const Track &track : picks) {
        pickedPaths.insert(track.path);
    }
    QVERIFY(pickedPaths.contains(QStringLiteral("/allowed")));
    QVERIFY(!pickedPaths.contains(QStringLiteral("/used-b")));
    QVERIFY(!pickedPaths.contains(QStringLiteral("/album-c")));
    QVERIFY(!pickedPaths.contains(QStringLiteral("/recent-c")));
}

void RadioTest::artistConstraintStateMetadataRoundTrips()
{
    RadioSession session({}, {}, {}, ArtistRadio::syntheticSeedCandidate(
                             QStringLiteral("The Artist"), {QStringLiteral("rock")}, 1999),
                         30, 1'000'000'000);

    QJsonObject root = session.constraintState();
    root.insert(QStringLiteral("active"), true);
    root.insert(QStringLiteral("kind"), QStringLiteral("artist"));
    root.insert(QStringLiteral("artistName"), QStringLiteral("The Artist"));
    root.insert(QStringLiteral("exploration"), 30);

    const QJsonObject restored = QJsonDocument(root).object();
    QCOMPARE(restored.value(QStringLiteral("kind")).toString(), QStringLiteral("artist"));
    QCOMPARE(restored.value(QStringLiteral("artistName")).toString(), QStringLiteral("The Artist"));
    QVERIFY(restored.value(QStringLiteral("seedPath")).toString().isEmpty());
}

// ---- Database + ListenHistoryStore -----------------------------------------

namespace {

Track makeDbTrack(const QTemporaryDir &dir, const QString &filename, const QStringList &genres,
                  const QString &originalDate, const QString &recordingId = {},
                  const QString &releaseGroupId = {},
                  const QStringList &mediaTags = QStringList())
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
    track.musicBrainz.recordingId = recordingId;
    track.musicBrainz.releaseGroupId = releaseGroupId;
    track.fileSize = 10;
    track.fileMtime = 20;
    if (!genres.isEmpty() || !mediaTags.isEmpty()) {
        MetadataBlob::FullMetadata meta;
        if (!genres.isEmpty()) {
            meta.tags.insert(QStringLiteral("GENRE"), genres);
        }
        if (!mediaTags.isEmpty()) {
            meta.tags.insert(QStringLiteral("MEDIA"), mediaTags);
        }
        const MetadataBlob::Encoded encoded = MetadataBlob::encode(meta);
        track.fullMetadataBlob = encoded.data;
        track.fullMetadataRawSize = encoded.rawSize;
    }
    return track;
}

struct FeatureNeighborRow {
    qint64 contentGroupId = -1;
    qint64 neighborGroupId = -1;
    int rank = 0;
    double cosine = 0.0;
};

struct FeatureScalarRow {
    double tempoBpm = -1.0;
    double energy = -1.0;
};

bool execFeatureFixtureSql(QSqlQuery &query, const QString &sql, QString *error)
{
    if (query.exec(sql)) {
        return true;
    }
    if (error != nullptr) {
        *error = query.lastError().text() + QStringLiteral(": ") + sql;
    }
    return false;
}

QString createRadioFeatureFixture(const QTemporaryDir &dir,
                                  const QHash<QString, qint64> &contentGroups,
                                  QString *error,
                                  const QList<FeatureNeighborRow> &neighbors = {},
                                  QHash<qint64, FeatureScalarRow> scalars = {})
{
    const QString path = dir.filePath(QStringLiteral("features.sqlite"));
    const QString connectionName =
        QStringLiteral("radio-feature-fixture-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool ok = true;

    {
        QSqlDatabase fixture = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        fixture.setDatabaseName(path);
        if (!fixture.open()) {
            if (error != nullptr) {
                *error = fixture.lastError().text();
            }
            ok = false;
        }

        if (ok) {
            QSqlQuery query(fixture);
            ok = ok && execFeatureFixtureSql(query, QStringLiteral("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT)"), error);
            ok = ok && execFeatureFixtureSql(query, QStringLiteral(
                                                   "CREATE TABLE files("
                                                   " path TEXT PRIMARY KEY,"
                                                   " mtime INTEGER NOT NULL,"
                                                   " size INTEGER NOT NULL,"
                                                   " duration_ms INTEGER,"
                                                   " decode_hash TEXT,"
                                                   " chromaprint_fp BLOB,"
                                                   " content_group_id INTEGER,"
                                                   " analyzed_at INTEGER NOT NULL,"
                                                   " status TEXT NOT NULL DEFAULT 'ok')"),
                                               error);
            ok = ok && execFeatureFixtureSql(query, QStringLiteral(
                                                   "CREATE TABLE content_groups(id INTEGER PRIMARY KEY AUTOINCREMENT)"),
                                               error);
            ok = ok && execFeatureFixtureSql(query, QStringLiteral(
                                                   "CREATE TABLE features("
                                                   " content_group_id INTEGER PRIMARY KEY,"
                                                   " tempo_bpm REAL,"
                                                   " loudness_lufs REAL,"
                                                   " loudness_std_db REAL,"
                                                   " spectral_centroid_mean_hz REAL,"
                                                   " spectral_centroid_std_hz REAL,"
                                                   " spectral_flatness_mean REAL,"
                                                   " zero_crossing_rate REAL,"
                                                   " onset_rate_hz REAL,"
                                                   " energy REAL,"
                                                   " extractor TEXT NOT NULL,"
                                                   " version TEXT NOT NULL)"),
                                               error);
            if (!neighbors.isEmpty()) {
                ok = ok && execFeatureFixtureSql(query, QStringLiteral(
                                                       "CREATE TABLE embeddings("
                                                       " content_group_id INTEGER PRIMARY KEY,"
                                                       " model TEXT NOT NULL,"
                                                       " version TEXT NOT NULL,"
                                                       " dim INTEGER NOT NULL,"
                                                       " vector BLOB NOT NULL)"),
                                                   error);
                ok = ok && execFeatureFixtureSql(query, QStringLiteral(
                                                       "CREATE TABLE track_neighbors("
                                                       " content_group_id INTEGER NOT NULL,"
                                                       " neighbor_group_id INTEGER NOT NULL,"
                                                       " rank INTEGER NOT NULL,"
                                                       " cosine REAL NOT NULL,"
                                                       " PRIMARY KEY(content_group_id, rank))"),
                                                   error);
            }
        }

        if (ok) {
            QSqlQuery meta(fixture);
            meta.prepare(QStringLiteral("INSERT INTO meta(key, value) VALUES('schema_version', ?)"));
            meta.addBindValue(QStringLiteral("3"));
            if (!meta.exec()) {
                if (error != nullptr) {
                    *error = meta.lastError().text();
                }
                ok = false;
            }
        }

        QSet<qint64> insertedGroups;
        if (ok) {
            for (auto it = contentGroups.cbegin(); it != contentGroups.cend(); ++it) {
                if (it.value() < 0 || insertedGroups.contains(it.value())) {
                    continue;
                }
                QSqlQuery group(fixture);
                group.prepare(QStringLiteral("INSERT INTO content_groups(id) VALUES(?)"));
                group.addBindValue(it.value());
                if (!group.exec()) {
                    if (error != nullptr) {
                        *error = group.lastError().text();
                    }
                    ok = false;
                    break;
                }
                insertedGroups.insert(it.value());
            }
        }

        if (ok) {
            for (auto it = contentGroups.cbegin(); it != contentGroups.cend(); ++it) {
                QSqlQuery insert(fixture);
                insert.prepare(QStringLiteral(
                    "INSERT INTO files(path, mtime, size, duration_ms, decode_hash, chromaprint_fp, "
                    "content_group_id, analyzed_at, status) VALUES(?, 1, 1, 1000, ?, ?, ?, 1000, 'ok')"));
                insert.addBindValue(it.key());
                insert.addBindValue(QStringLiteral("hash-%1").arg(it.value()));
                insert.addBindValue(QByteArray::fromHex("01020304"));
                insert.addBindValue(it.value() >= 0 ? QVariant(it.value()) : QVariant());
                if (!insert.exec()) {
                    if (error != nullptr) {
                        *error = insert.lastError().text();
                    }
                    ok = false;
                    break;
                }
            }
        }

        if (ok) {
            for (auto it = scalars.cbegin(); it != scalars.cend(); ++it) {
                QSqlQuery insert(fixture);
                insert.prepare(QStringLiteral(
                    "INSERT INTO features(content_group_id, tempo_bpm, loudness_lufs, loudness_std_db, "
                    "spectral_centroid_mean_hz, spectral_centroid_std_hz, spectral_flatness_mean, "
                    "zero_crossing_rate, onset_rate_hz, energy, extractor, version) "
                    "VALUES(?, ?, NULL, NULL, NULL, NULL, NULL, NULL, NULL, ?, 'fixture', 'dsp')"));
                insert.addBindValue(it.key());
                insert.addBindValue(it.value().tempoBpm > 0.0 ? QVariant(it.value().tempoBpm) : QVariant());
                insert.addBindValue(it.value().energy >= 0.0 ? QVariant(it.value().energy) : QVariant());
                if (!insert.exec()) {
                    if (error != nullptr) {
                        *error = insert.lastError().text();
                    }
                    ok = false;
                    break;
                }
            }
        }

        if (ok) {
            for (const FeatureNeighborRow &neighbor : neighbors) {
                QSqlQuery insert(fixture);
                insert.prepare(QStringLiteral(
                    "INSERT INTO track_neighbors(content_group_id, neighbor_group_id, rank, cosine) "
                    "VALUES(?, ?, ?, ?)"));
                insert.addBindValue(neighbor.contentGroupId);
                insert.addBindValue(neighbor.neighborGroupId);
                insert.addBindValue(neighbor.rank);
                insert.addBindValue(neighbor.cosine);
                if (!insert.exec()) {
                    if (error != nullptr) {
                        *error = insert.lastError().text();
                    }
                    ok = false;
                    break;
                }
            }
        }

        fixture.close();
    }

    QSqlDatabase::removeDatabase(connectionName);
    return ok ? path : QString();
}

QHash<QString, QString> resolvedSongKeysFromFixture(Database &db, FeatureStore &features)
{
    const auto rows = db.trackMatchRows();
    QStringList paths;
    paths.reserve(rows.size());
    for (const auto &[path, artist, title, recordingMbid] : rows) {
        Q_UNUSED(artist);
        Q_UNUSED(title);
        Q_UNUSED(recordingMbid);
        paths.push_back(path);
    }

    const QHash<QString, qint64> groups = features.contentGroupsForPaths(paths);
    QList<SongIdentity::TrackIdentity> identities;
    identities.reserve(rows.size());
    for (const auto &[path, artist, title, recordingMbid] : rows) {
        identities.push_back({path, artist, title, recordingMbid, groups.value(path, -1)});
    }
    return SongIdentity::resolvedSongKeys(identities);
}

TrackScorer::Candidate candidateFromFixtureRow(const RadioCandidateRow &row,
                                               const QHash<QString, QString> &resolvedSongKeys)
{
    TrackScorer::Candidate candidate;
    candidate.path = row.path;
    candidate.songKey = resolvedSongKeys.value(row.path, FoldKey::songKey(row.mbRecordingId, row.artistName, row.title));
    candidate.artistFolded = FoldKey::fold(row.artistName);
    candidate.albumKey = FoldKey::albumGroupKey(row.releaseGroupId, row.albumArtistName, row.albumTitle);
    candidate.genresFolded = row.genresFolded;
    candidate.year = row.year;
    candidate.effectiveRating0To100 = row.effectiveRating0To100;
    candidate.hasUserRating = row.hasUserRating;
    return candidate;
}

QVector<TrackScorer::Candidate> fixtureCandidates(Database &db,
                                                  const QHash<QString, QString> &resolvedSongKeys,
                                                  int limit = 500)
{
    QVector<TrackScorer::Candidate> pool;
    const QVector<RadioCandidateRow> rows = db.radioFallbackCandidates(limit);
    pool.reserve(rows.size());
    for (const RadioCandidateRow &row : rows) {
        pool.push_back(candidateFromFixtureRow(row, resolvedSongKeys));
    }
    return pool;
}

TrackScorer::Affinity fixtureAffinityFromRow(const ListenHistoryStore::TrackAffinityRow &row)
{
    TrackScorer::Affinity affinity;
    affinity.playEvents = row.playEvents;
    affinity.finished = row.finished;
    affinity.skipped = row.skipped;
    affinity.lastPlayedAtSecs = row.lastPlayedAtSecs;
    affinity.listenCount = row.listenCount;
    affinity.baselineMax = row.baselineMax;
    return affinity;
}

QStringList mediaTagsFromFixtureDb(Database &db, const QString &path)
{
    QStringList media;
    const MetadataBlob::FullMetadata metadata = db.fullMetadata(path);
    for (auto it = metadata.tags.cbegin(); it != metadata.tags.cend(); ++it) {
        if (it.key().compare(QStringLiteral("MEDIA"), Qt::CaseInsensitive) == 0) {
            media.append(it.value());
        }
    }
    return media;
}

Track resolveBestFixtureCopy(Database &db, FeatureStore &features, const QString &path,
                             const QSet<QString> &blockedPaths = {})
{
    Track fallback = db.trackForPath(path);
    if (fallback.path.isEmpty()) {
        return {};
    }
    db.enrichTrackForStatus(fallback);

    const qint64 groupId = features.contentGroupForPath(path);
    if (groupId < 0) {
        return fallback;
    }
    const QStringList groupPaths = features.pathsInGroup(groupId);
    if (groupPaths.size() < 2) {
        return fallback;
    }

    QVector<QualityRank::Copy> copies;
    QHash<QString, Track> tracksByPath;
    copies.reserve(groupPaths.size());
    tracksByPath.reserve(groupPaths.size());
    for (const QString &groupPath : groupPaths) {
        if (blockedPaths.contains(groupPath)) {
            continue;
        }
        Track copy = db.trackForPath(groupPath);
        if (copy.path.isEmpty()) {
            continue;
        }
        db.enrichTrackForStatus(copy);
        copies.push_back(QualityRank::Copy{
            copy.path,
            copy.codec,
            copy.bitDepth,
            copy.sampleRateHz,
            copy.bitrateKbps,
            mediaTagsFromFixtureDb(db, copy.path),
        });
        tracksByPath.insert(copy.path, copy);
    }

    const QString bestPath = QualityRank::bestPath(copies, db.contentGroupPin(groupId));
    return tracksByPath.value(bestPath, fallback);
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
                                      QStringLiteral("2004-05-06"), QStringLiteral("recording-1"),
                                      QStringLiteral("release-group-1"));
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
    QCOMPARE(rockRows.first().title, rockPop.title);
    QCOMPARE(rockRows.first().mbRecordingId, QStringLiteral("recording-1"));
    QCOMPARE(rockRows.first().releaseGroupId, QStringLiteral("release-group-1"));
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

void RadioTest::neighborCandidateRowsAugmentTagPoorPoolAndRespectFlags()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Database db(QStringLiteral("radio-neighbor-pool-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(db.open(dir.filePath(QStringLiteral("library.sqlite"))), qPrintable(db.lastError()));

    const Track seed = makeDbTrack(dir, QStringLiteral("seed.flac"), {QStringLiteral("Rock")},
                                   QStringLiteral("2004"));
    const Track sameGenre = makeDbTrack(dir, QStringLiteral("same.flac"), {QStringLiteral("Rock")},
                                        QStringLiteral("2005"));
    const Track tagPoorNeighbor = makeDbTrack(dir, QStringLiteral("neighbor.flac"), {},
                                              QStringLiteral("2006"));
    QVERIFY2(db.upsertTrack(seed), qPrintable(db.lastError()));
    QVERIFY2(db.upsertTrack(sameGenre), qPrintable(db.lastError()));
    QVERIFY2(db.upsertTrack(tagPoorNeighbor), qPrintable(db.lastError()));

    QString featureError;
    const QString featuresPath = createRadioFeatureFixture(dir, {
                                                               {seed.path, 100},
                                                               {sameGenre.path, 101},
                                                               {tagPoorNeighbor.path, 200},
                                                           },
                                                           &featureError,
                                                           {{100, 200, 1, 0.95}});
    QVERIFY2(!featuresPath.isEmpty(), qPrintable(featureError));
    FeatureStore features(featuresPath);
    QVERIFY(features.isOpen());

    const QHash<QString, QString> resolvedSongKeys = resolvedSongKeysFromFixture(db, features);
    QVector<TrackScorer::Candidate> pool;
    for (const RadioCandidateRow &row : db.radioCandidates({GenreTags::folded(QStringLiteral("Rock"))})) {
        pool.push_back(candidateFromFixtureRow(row, resolvedSongKeys));
    }
    QVERIFY(containsCandidatePath(pool, seed.path));
    QVERIFY(containsCandidatePath(pool, sameGenre.path));
    QVERIFY(!containsCandidatePath(pool, tagPoorNeighbor.path));

    QStringList neighborPaths;
    const qint64 seedGroup = features.contentGroupForPath(seed.path);
    const QList<QPair<qint64, double>> neighbors = features.neighborsOfGroup(seedGroup, 200);
    QCOMPARE(neighbors.size(), 1);
    const QStringList groupPaths = features.pathsInGroup(neighbors.first().first);
    QCOMPARE(groupPaths.size(), 1);
    neighborPaths.push_back(groupPaths.first());

    for (const RadioCandidateRow &row : db.radioCandidatesForPaths(neighborPaths)) {
        pool.push_back(candidateFromFixtureRow(row, resolvedSongKeys));
    }
    QVERIFY(containsCandidatePath(pool, tagPoorNeighbor.path));

    QVERIFY2(db.setTrackFlag(tagPoorNeighbor.path, Database::TrackFlag::NeverRadio, true),
             qPrintable(db.lastError()));
    const QVector<TrackScorer::Candidate> filtered = RadioFilters::excludeFlaggedCandidates(
        pool, db.flaggedPaths(Database::TrackFlag::NeverRadio));
    QVERIFY(!containsCandidatePath(filtered, tagPoorNeighbor.path));
}

void RadioTest::featureStoreV3ScalarsFeedTempoEnergyScoring()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Database db(QStringLiteral("radio-v3-scalars-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(db.open(dir.filePath(QStringLiteral("library.sqlite"))), qPrintable(db.lastError()));

    const Track seed = makeDbTrack(dir, QStringLiteral("seed.flac"), {QStringLiteral("Rock")},
                                   QStringLiteral("2004"));
    const Track candidateTrack = makeDbTrack(dir, QStringLiteral("candidate.flac"), {QStringLiteral("Rock")},
                                             QStringLiteral("2004"));
    QVERIFY2(db.upsertTrack(seed), qPrintable(db.lastError()));
    QVERIFY2(db.upsertTrack(candidateTrack), qPrintable(db.lastError()));

    QString featureError;
    const QString featuresPath = createRadioFeatureFixture(dir, {
                                                               {seed.path, 10},
                                                               {candidateTrack.path, 11},
                                                           },
                                                           &featureError,
                                                           {},
                                                           {
                                                               {10, {120.0, 0.6}},
                                                               {11, {120.0, 0.6}},
                                                           });
    QVERIFY2(!featuresPath.isEmpty(), qPrintable(featureError));
    FeatureStore features(featuresPath);
    QVERIFY(features.isOpen());
    QCOMPARE(features.schemaVersion(), 3);

    const QHash<QString, qint64> groups = features.contentGroupsForPaths({seed.path, candidateTrack.path});
    const QHash<qint64, FeatureStore::Scalars> scalars = features.scalarsForGroups({10, 11});

    TrackScorer::Candidate candidate = makeCandidate(candidateTrack.path,
                                                     FoldKey::fold(candidateTrack.artistName),
                                                     {GenreTags::folded(QStringLiteral("Rock"))},
                                                     2004);
    candidate.contentGroupId = groups.value(candidateTrack.path, -1);
    candidate.tempoBpm = scalars.value(candidate.contentGroupId).tempoBpm;
    candidate.energy = scalars.value(candidate.contentGroupId).energy;

    TrackScorer::SeedContext context;
    context.genresFolded = {GenreTags::folded(QStringLiteral("Rock"))};
    context.genreIdf.insert(GenreTags::folded(QStringLiteral("Rock")), 1.0);
    context.year = 2004;
    context.contextTempoBpm = scalars.value(groups.value(seed.path)).tempoBpm;
    context.contextEnergy = scalars.value(groups.value(seed.path)).energy;

    const TrackScorer::Scored scored = TrackScorer::score(candidate, {}, context);
    QVERIFY(qFuzzyCompare(componentValue(scored, QStringLiteral("tempo")), 0.4));
    QVERIFY(qFuzzyCompare(componentValue(scored, QStringLiteral("energy")), 0.6));
}

void RadioTest::genreAliasesExpandCandidatesAndMergeCounts()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Database db(QStringLiteral("genre-alias-radio-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(db.open(dir.filePath(QStringLiteral("library.sqlite"))), qPrintable(db.lastError()));

    const Track canonical = makeDbTrack(dir, QStringLiteral("01.flac"), {QStringLiteral("Classical")},
                                        QStringLiteral("2004-05-06"));
    const Track portuguese = makeDbTrack(dir, QStringLiteral("02.flac"), {QString::fromUtf8("Clássica")},
                                         QStringLiteral("1999"));
    QVERIFY2(db.upsertTrack(canonical), qPrintable(db.lastError()));
    QVERIFY2(db.upsertTrack(portuguese), qPrintable(db.lastError()));

    const QVector<RadioCandidateRow> rows = db.radioCandidates({QStringLiteral("classical")});
    QCOMPARE(rows.size(), 2);
    QVERIFY(containsCandidatePath(QVector<TrackScorer::Candidate>{
                                      makeCandidate(rows.at(0).path, QStringLiteral("artist"), {}),
                                      makeCandidate(rows.at(1).path, QStringLiteral("artist"), {}),
                                  },
                                  portuguese.path));

    int taggedTrackTotal = 0;
    const QHash<QString, int> rawCounts = db.genreTrackCounts(&taggedTrackTotal);
    const QHash<QString, QString> aliases = db.genreAliases();
    QHash<QString, int> canonicalCounts;
    for (auto it = rawCounts.cbegin(); it != rawCounts.cend(); ++it) {
        canonicalCounts[GenreTags::canonical(it.key(), aliases)] += it.value();
    }
    QCOMPARE(canonicalCounts.value(QStringLiteral("classical")), 2);
    QCOMPARE(taggedTrackTotal, 2);
}

void RadioTest::radioWeightProfilesRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Database db(QStringLiteral("radio-weight-profiles-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(db.open(dir.filePath(QStringLiteral("library.sqlite"))), qPrintable(db.lastError()));

    TrackScorer::Weights weights = TrackScorer::defaultWeights();
    weights.genreWeight = 2.25;
    weights.ratingWeight = 0.75;
    const QString balanced = QString::fromUtf8(TrackScorer::weightsToJson(weights));
    QVERIFY2(db.saveRadioWeightProfile(QStringLiteral("balanced"), balanced), qPrintable(db.lastError()));
    QCOMPARE(db.radioWeightProfile(QStringLiteral("balanced")), balanced);

    weights.genreWeight = 1.0;
    weights.noveltyWeight = 1.4;
    const QString exploratory = QString::fromUtf8(TrackScorer::weightsToJson(weights));
    QVERIFY2(db.saveRadioWeightProfile(QStringLiteral("explore"), exploratory), qPrintable(db.lastError()));

    QVector<Database::RadioWeightProfile> profiles = db.radioWeightProfiles();
    QCOMPARE(profiles.size(), 2);
    QCOMPARE(profiles.at(0).name, QStringLiteral("balanced"));
    QCOMPARE(profiles.at(0).weightsJson, balanced);
    QVERIFY(!profiles.at(0).updatedAt.isEmpty());
    QCOMPARE(profiles.at(1).name, QStringLiteral("explore"));

    weights.ratingWeight = 1.1;
    const QString updated = QString::fromUtf8(TrackScorer::weightsToJson(weights));
    QVERIFY2(db.saveRadioWeightProfile(QStringLiteral("balanced"), updated), qPrintable(db.lastError()));
    QCOMPARE(db.radioWeightProfile(QStringLiteral("balanced")), updated);
    QCOMPARE(db.radioWeightProfiles().size(), 2);

    QVERIFY2(db.removeRadioWeightProfile(QStringLiteral("explore")), qPrintable(db.lastError()));
    QVERIFY(db.radioWeightProfile(QStringLiteral("explore")).isEmpty());
    profiles = db.radioWeightProfiles();
    QCOMPARE(profiles.size(), 1);
    QCOMPARE(profiles.first().name, QStringLiteral("balanced"));
}

void RadioTest::ignoredRadioGenresRoundTripAndSuppressCandidateJoins()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Database db(QStringLiteral("radio-ignored-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(db.open(dir.filePath(QStringLiteral("library.sqlite"))), qPrintable(db.lastError()));

    const Track rock = makeDbTrack(dir, QStringLiteral("01.flac"), {QStringLiteral("Rock")},
                                   QStringLiteral("2004-05-06"));
    const Track metal = makeDbTrack(dir, QStringLiteral("02.flac"), {QStringLiteral("Metal")},
                                    QStringLiteral("2005"));
    QVERIFY2(db.upsertTrack(rock), qPrintable(db.lastError()));
    QVERIFY2(db.upsertTrack(metal), qPrintable(db.lastError()));

    const QString rockFolded = GenreTags::folded(QStringLiteral("Rock"));
    QCOMPARE(db.radioCandidates({rockFolded}).size(), 1);

    QVERIFY2(db.setRadioGenreIgnored(rockFolded, true), qPrintable(db.lastError()));
    QVERIFY(db.ignoredRadioGenres().contains(rockFolded));
    QVERIFY(db.radioCandidates({rockFolded}).isEmpty());
    QCOMPARE(db.radioCandidates({GenreTags::folded(QStringLiteral("Metal"))}).size(), 1);

    QVERIFY2(db.setRadioGenreIgnored(rockFolded, false), qPrintable(db.lastError()));
    QVERIFY(!db.ignoredRadioGenres().contains(rockFolded));
    QCOMPARE(db.radioCandidates({rockFolded}).size(), 1);
}

void RadioTest::ignoredRadioGenresCanonicalizeAliasesAndPreserveVisibility()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Database db(QStringLiteral("radio-ignored-alias-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(db.open(dir.filePath(QStringLiteral("library.sqlite"))), qPrintable(db.lastError()));

    const Track aliasOnly = makeDbTrack(dir, QStringLiteral("01.flac"), {QStringLiteral("Alt Rock")},
                                        QStringLiteral("2004-05-06"));
    const Track mixed = makeDbTrack(dir, QStringLiteral("02.flac"),
                                    {QStringLiteral("Alt Rock"), QStringLiteral("Metal")},
                                    QStringLiteral("2005"));
    QVERIFY2(db.upsertTrack(aliasOnly), qPrintable(db.lastError()));
    QVERIFY2(db.upsertTrack(mixed), qPrintable(db.lastError()));
    QVERIFY2(db.setGenreAlias(QStringLiteral("Alt Rock"), QStringLiteral("Rock")), qPrintable(db.lastError()));

    const QHash<QString, QString> aliases = db.genreAliases();
    const QString canonical = GenreTags::canonical(GenreTags::folded(QStringLiteral("Alt Rock")), aliases);
    QCOMPARE(canonical, QStringLiteral("rock"));
    QVERIFY2(db.setRadioGenreIgnored(canonical, true), qPrintable(db.lastError()));

    QVERIFY(db.radioCandidates({GenreTags::folded(QStringLiteral("Alt Rock"))}).isEmpty());
    const QVector<RadioCandidateRow> metalRows = db.radioCandidates({GenreTags::folded(QStringLiteral("Metal"))});
    QCOMPARE(metalRows.size(), 1);
    QCOMPARE(metalRows.first().path, mixed.path);
    QVERIFY(metalRows.first().genresFolded.contains(GenreTags::folded(QStringLiteral("Alt Rock"))));
    QVERIFY(metalRows.first().genresFolded.contains(GenreTags::folded(QStringLiteral("Metal"))));

    QCOMPARE(db.genresForTrack(aliasOnly.path), QStringList{QStringLiteral("Alt Rock")});
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

void RadioTest::artistSeedGenresAggregateFrequencyAliasesIgnoresAndCap()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Database db(QStringLiteral("artist-seed-genres-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(db.open(dir.filePath(QStringLiteral("library.sqlite"))), qPrintable(db.lastError()));

    QVector<Track> tracks{
        makeDbTrack(dir, QStringLiteral("01.flac"), {QStringLiteral("Alt Rock"), QStringLiteral("Metal")},
                    QStringLiteral("2000")),
        makeDbTrack(dir, QStringLiteral("02.flac"), {QStringLiteral("Rock"), QStringLiteral("Metal")},
                    QStringLiteral("2001")),
        makeDbTrack(dir, QStringLiteral("03.flac"), {QStringLiteral("Alt Rock"), QStringLiteral("Jazz")},
                    QStringLiteral("2002")),
        makeDbTrack(dir, QStringLiteral("04.flac"), {QStringLiteral("Pop")}, QStringLiteral("2003")),
        makeDbTrack(dir, QStringLiteral("05.flac"), {QStringLiteral("Classical")}, QStringLiteral("2004")),
        makeDbTrack(dir, QStringLiteral("06.flac"), {QStringLiteral("Electronic")}, QStringLiteral("2005")),
        makeDbTrack(dir, QStringLiteral("07.flac"), {QStringLiteral("Other")}, QStringLiteral("2006")),
    };
    for (Track &track : tracks) {
        track.albumArtistName = QStringLiteral("Artist Radio");
        QVERIFY2(db.upsertTrack(track), qPrintable(db.lastError()));
    }
    QVERIFY2(db.setGenreAlias(QStringLiteral("Alt Rock"), QStringLiteral("Rock")), qPrintable(db.lastError()));
    QVERIFY2(db.setRadioGenreIgnored(GenreTags::folded(QStringLiteral("Pop")), true), qPrintable(db.lastError()));

    const QStringList seedGenres = ArtistRadio::aggregateSeedGenres(
        db.genreCountsForArtist(QStringLiteral("Artist Radio")), db.genreAliases(), db.ignoredRadioGenres());

    QCOMPARE(seedGenres, QStringList({QStringLiteral("rock"), QStringLiteral("metal"),
                                      QStringLiteral("classical"), QStringLiteral("electronic")}));
}

void RadioTest::artistMedianYearIgnoresUnknowns()
{
    QVector<Track> odd(4);
    odd[0].originalDate = QStringLiteral("2001-01-01");
    odd[1].date = QStringLiteral("1999");
    odd[2].originalDate = QStringLiteral("2010");
    odd[3].date = QStringLiteral("unknown");
    QCOMPARE(ArtistRadio::medianTrackYear(odd), 2001);

    QVector<Track> even(3);
    even[0].date = QStringLiteral("1990");
    even[1].originalDate = QStringLiteral("2000-05-06");
    even[2].date = QStringLiteral("not-a-year");
    QCOMPARE(ArtistRadio::medianTrackYear(even), 1995);

    QVector<Track> unknownOnly(1);
    unknownOnly[0].date = QStringLiteral("xxxx");
    QCOMPARE(ArtistRadio::medianTrackYear(unknownOnly), 0);
}

void RadioTest::artistRepresentativeTrackUsesRatingThenAffinity()
{
    Track lowerRated;
    lowerRated.path = QStringLiteral("/a");
    lowerRated.effectiveRating0To100 = 80;

    Track higherAffinity;
    higherAffinity.path = QStringLiteral("/b");
    higherAffinity.effectiveRating0To100 = 90;

    Track lowerAffinity;
    lowerAffinity.path = QStringLiteral("/c");
    lowerAffinity.effectiveRating0To100 = 90;

    QHash<QString, TrackScorer::Affinity> affinities;
    affinities.insert(QStringLiteral("/b"), makeAffinity(3, 2, 0, 0, 3, 0));
    affinities.insert(QStringLiteral("/c"), makeAffinity(1, 0, 1, 0, 1, 0));

    QCOMPARE(ArtistRadio::representativeTrack({lowerRated, lowerAffinity, higherAffinity}, affinities).path,
             QStringLiteral("/b"));
}

void RadioTest::sampleArtistsForGenreReturnsDeterministicNames()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Database db(QStringLiteral("genre-sample-artists-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(db.open(dir.filePath(QStringLiteral("library.sqlite"))), qPrintable(db.lastError()));

    Track beta = makeDbTrack(dir, QStringLiteral("01.flac"), {QStringLiteral("Rock")},
                             QStringLiteral("2004-05-06"));
    beta.artistName = QStringLiteral("Beta Artist");
    Track alpha = makeDbTrack(dir, QStringLiteral("02.flac"), {QStringLiteral("Rock")},
                              QStringLiteral("2004-05-06"));
    alpha.artistName = QStringLiteral("Alpha Artist");
    Track jazz = makeDbTrack(dir, QStringLiteral("03.flac"), {QStringLiteral("Jazz")},
                             QStringLiteral("2004-05-06"));
    jazz.artistName = QStringLiteral("Jazz Artist");

    QVERIFY2(db.upsertTrack(beta), qPrintable(db.lastError()));
    QVERIFY2(db.upsertTrack(alpha), qPrintable(db.lastError()));
    QVERIFY2(db.upsertTrack(jazz), qPrintable(db.lastError()));

    QCOMPARE(db.sampleArtistsForGenre(GenreTags::folded(QStringLiteral("Rock")), 3),
             QStringList({QStringLiteral("Alpha Artist"), QStringLiteral("Beta Artist")}));
    QCOMPARE(db.sampleArtistsForGenre(GenreTags::folded(QStringLiteral("Rock")), 1),
             QStringList({QStringLiteral("Alpha Artist")}));
}

void RadioTest::genrePipeBackfillResplitsStoredMetadata()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString dbPath = dir.filePath(QStringLiteral("library.sqlite"));
    const QString connectionName = QStringLiteral("genre-pipe-backfill-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));

    const Track track = makeDbTrack(dir, QStringLiteral("01.flac"),
                                    {QStringLiteral("Alternative | Other")},
                                    QStringLiteral("2004-05-06"));
    {
        Database db(connectionName);
        QVERIFY2(db.open(dbPath), qPrintable(db.lastError()));
        QVERIFY2(db.upsertTrack(track), qPrintable(db.lastError()));
        QCOMPARE(db.genresForTrack(track.path),
                 QStringList({QStringLiteral("Alternative"), QStringLiteral("Other")}));
    }

    {
        const QString rawConnection = QStringLiteral("genre-pipe-backfill-raw-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        {
            QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), rawConnection);
            raw.setDatabaseName(dbPath);
            QVERIFY(raw.open());
            QSqlQuery q(raw);
            QVERIFY(q.exec(QStringLiteral("DELETE FROM track_genres")));
            QVERIFY(q.exec(QStringLiteral(
                "INSERT INTO track_genres(track_id, genre, genre_folded) "
                "SELECT id, 'Alternative | Other', 'alternative | other' FROM tracks WHERE path LIKE '%01.flac'")));
            QVERIFY(q.exec(QStringLiteral("DELETE FROM schema_migrations WHERE version = 13")));
            raw.close();
        }
        QSqlDatabase::removeDatabase(rawConnection);
    }

    {
        Database db(connectionName);
        QVERIFY2(db.open(dbPath), qPrintable(db.lastError()));
        QCOMPARE(db.genresForTrack(track.path),
                 QStringList({QStringLiteral("Alternative"), QStringLiteral("Other")}));
    }
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

void RadioTest::contentGroupAffinityPoolsDisagreeingTags()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Database db(QStringLiteral("content-group-affinity-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(db.open(dir.filePath(QStringLiteral("library.sqlite"))), qPrintable(db.lastError()));

    Track albumCopy = makeDbTrack(dir, QStringLiteral("album-copy.flac"), {QStringLiteral("Rock")},
                                  QStringLiteral("2001"), QStringLiteral("mbid-album"));
    Track compilationCopy = makeDbTrack(dir, QStringLiteral("compilation-copy.flac"), {QStringLiteral("Jazz")},
                                        QStringLiteral("2001"), QStringLiteral("mbid-compilation"));
    albumCopy.artistName = QStringLiteral("Album Artist");
    albumCopy.title = QStringLiteral("Album Title");
    compilationCopy.artistName = QStringLiteral("Compilation Artist");
    compilationCopy.title = QStringLiteral("Compilation Title");
    QVERIFY2(db.upsertTrack(albumCopy), qPrintable(db.lastError()));
    QVERIFY2(db.upsertTrack(compilationCopy), qPrintable(db.lastError()));

    QString featureError;
    const QString featuresPath = createRadioFeatureFixture(dir, {
                                                               {albumCopy.path, 77},
                                                               {compilationCopy.path, 77},
                                                           },
                                                           &featureError);
    QVERIFY2(!featuresPath.isEmpty(), qPrintable(featureError));
    FeatureStore features(featuresPath);
    QVERIFY(features.isOpen());

    ListenHistoryStore history(dir.filePath(QStringLiteral("history.sqlite")));
    QVERIFY(history.isOpen());
    QVERIFY(history.recordListen(albumCopy, 1000, false, false) > 0);
    QVERIFY(history.recordListen(compilationCopy, 2000, false, false) > 0);

    QHash<QString, TrackScorer::Affinity> affinities;
    const QHash<QString, ListenHistoryStore::TrackAffinityRow> rows = history.trackAffinities();
    for (auto it = rows.cbegin(); it != rows.cend(); ++it) {
        affinities.insert(it.key(), fixtureAffinityFromRow(it.value()));
    }

    const QHash<QString, QString> resolvedSongKeys = resolvedSongKeysFromFixture(db, features);
    QCOMPARE(resolvedSongKeys.value(albumCopy.path), resolvedSongKeys.value(compilationCopy.path));

    const QHash<QString, TrackScorer::Affinity> pooled =
        AffinityPool::poolBySongKey(affinities, resolvedSongKeys);
    QCOMPARE(pooled.value(albumCopy.path).listenCount, 2);
    QCOMPARE(pooled.value(compilationCopy.path).listenCount, 2);
}

void RadioTest::contentGroupSongKeyDedupsRadioSession()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Database db(QStringLiteral("content-group-radio-dedup-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(db.open(dir.filePath(QStringLiteral("library.sqlite"))), qPrintable(db.lastError()));

    Track first = makeDbTrack(dir, QStringLiteral("copy-a.flac"), {QStringLiteral("Rock")},
                              QStringLiteral("2001"), QStringLiteral("mbid-a"));
    Track second = makeDbTrack(dir, QStringLiteral("copy-b.flac"), {QStringLiteral("Jazz")},
                               QStringLiteral("2001"), QStringLiteral("mbid-b"));
    first.artistName = QStringLiteral("Artist A");
    first.title = QStringLiteral("Title A");
    second.artistName = QStringLiteral("Artist B");
    second.title = QStringLiteral("Title B");
    QVERIFY2(db.upsertTrack(first), qPrintable(db.lastError()));
    QVERIFY2(db.upsertTrack(second), qPrintable(db.lastError()));

    QString featureError;
    const QString featuresPath = createRadioFeatureFixture(dir, {
                                                               {first.path, 78},
                                                               {second.path, 78},
                                                           },
                                                           &featureError);
    QVERIFY2(!featuresPath.isEmpty(), qPrintable(featureError));
    FeatureStore features(featuresPath);
    QVERIFY(features.isOpen());

    const QHash<QString, QString> resolvedSongKeys = resolvedSongKeysFromFixture(db, features);
    QCOMPARE(resolvedSongKeys.value(first.path), resolvedSongKeys.value(second.path));

    const QVector<TrackScorer::Candidate> pool = fixtureCandidates(db, resolvedSongKeys);
    QCOMPARE(pool.size(), 2);
    RadioSession session(pool, {}, {}, 30, 1'000'000'000);

    const QVector<Track> picks = session.nextTracks(10, {}, [&db](const QString &path) {
        return db.trackForPath(path);
    });
    QCOMPARE(picks.size(), 1);
}

void RadioTest::contentGroupResolverQueuesBestOrPinnedCopy()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    Database db(QStringLiteral("content-group-best-copy-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY2(db.open(dir.filePath(QStringLiteral("library.sqlite"))), qPrintable(db.lastError()));

    Track low = makeDbTrack(dir, QStringLiteral("portable.mp3"), {QStringLiteral("Rock")},
                            QStringLiteral("2001"), QStringLiteral("mbid-low"));
    low.codec = QStringLiteral("mp3");
    low.bitrateKbps = 320;
    low.sampleRateHz = 44100;
    Track high = makeDbTrack(dir, QStringLiteral("archive.flac"), {QStringLiteral("Rock")},
                             QStringLiteral("2001"), QStringLiteral("mbid-high"));
    high.codec = QStringLiteral("flac");
    high.bitDepth = 24;
    high.sampleRateHz = 96000;
    high.bitrateKbps = 3000;
    QVERIFY2(db.upsertTrack(low), qPrintable(db.lastError()));
    QVERIFY2(db.upsertTrack(high), qPrintable(db.lastError()));

    QString featureError;
    const QString featuresPath = createRadioFeatureFixture(dir, {
                                                               {low.path, 79},
                                                               {high.path, 79},
                                                           },
                                                           &featureError);
    QVERIFY2(!featuresPath.isEmpty(), qPrintable(featureError));
    FeatureStore features(featuresPath);
    QVERIFY(features.isOpen());

    const QHash<QString, QString> resolvedSongKeys = resolvedSongKeysFromFixture(db, features);
    TrackScorer::Candidate lowCandidate;
    for (const RadioCandidateRow &row : db.radioFallbackCandidates(10)) {
        if (row.path == low.path) {
            lowCandidate = candidateFromFixtureRow(row, resolvedSongKeys);
            break;
        }
    }
    QVERIFY(!lowCandidate.path.isEmpty());
    const QVector<TrackScorer::Candidate> pool{lowCandidate};

    RadioSession qualitySession(pool, {}, {}, 30, 1'000'000'000);
    const QVector<Track> qualityPicks = qualitySession.nextTracks(1, {}, [&db, &features](const QString &path) {
        return resolveBestFixtureCopy(db, features, path);
    });
    QCOMPARE(qualityPicks.size(), 1);
    QCOMPARE(qualityPicks.first().path, high.path);

    RadioSession blockedSession(pool, {}, {}, 30, 1'000'000'000);
    const QSet<QString> blockedBest{high.path};
    const QVector<Track> blockedPicks = blockedSession.nextTracks(1, {}, [&db, &features, blockedBest](const QString &path) {
        return resolveBestFixtureCopy(db, features, path, blockedBest);
    });
    QCOMPARE(blockedPicks.size(), 1);
    QCOMPARE(blockedPicks.first().path, low.path);

    QVERIFY2(db.setContentGroupPin(79, low.path), qPrintable(db.lastError()));
    RadioSession pinnedSession(pool, {}, {}, 30, 1'000'000'000);
    const QVector<Track> pinnedPicks = pinnedSession.nextTracks(1, {}, [&db, &features](const QString &path) {
        return resolveBestFixtureCopy(db, features, path);
    });
    QCOMPARE(pinnedPicks.size(), 1);
    QCOMPARE(pinnedPicks.first().path, low.path);
}

// ---- GenreTags --------------------------------------------------------------

void RadioTest::genreTagsSplitPipeSeparators()
{
    MetadataBlob::FullMetadata metadata;
    metadata.tags.insert(QStringLiteral("GENRE"), {
        QStringLiteral("Alternative|Other"),
        QStringLiteral("Dream Pop | Shoegaze"),
        QStringLiteral("Jazz/Fusion;Soul,Neo Soul"),
    });

    QCOMPARE(GenreTags::fromMetadata(metadata),
             QStringList({QStringLiteral("Alternative"), QStringLiteral("Other"),
                          QStringLiteral("Dream Pop"), QStringLiteral("Shoegaze"),
                          QStringLiteral("Jazz"), QStringLiteral("Fusion"),
                          QStringLiteral("Soul"), QStringLiteral("Neo Soul")}));
}

void RadioTest::genreCanonicalIdentityAndMapping()
{
    const QHash<QString, QString> aliases{
        {QString::fromUtf8("clássica"), QStringLiteral("classical")},
    };

    QCOMPARE(GenreTags::canonical(QStringLiteral("rock"), aliases), QStringLiteral("rock"));
    QCOMPARE(GenreTags::canonical(QString::fromUtf8("clássica"), aliases), QStringLiteral("classical"));
}

void RadioTest::genreCanonicalStoplistAfterMapping()
{
    const QHash<QString, QString> aliases{
        {QStringLiteral("miscellaneous"), QStringLiteral("misc")},
    };

    const QString canonical = GenreTags::canonical(QStringLiteral("miscellaneous"), aliases);
    QCOMPARE(canonical, QStringLiteral("misc"));
    QVERIFY(GenreTags::isNonGenre(canonical));
}

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
