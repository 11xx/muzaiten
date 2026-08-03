// Radio context/fill benchmark (dev tool; built by default, deliberately NOT
// registered with CTest). Timings are a product-decision artifact, not a gate.
//
// The benchmark is intentionally stateful: pass --allow-isolated-state together
// with MUZAITEN_STATE_ROOT pointing at a copied library, state, cache, and
// config tree. It never opens the default XDG paths or starts playback.

#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSet>
#include <QStringList>

#define private public
#include "app/AppCore.h"
#undef private
#include "core/Track.h"
#include "db/Database.h"
#include "features/FeatureStore.h"
#include "player/PlayerCore.h"
#include "reco/RadioSession.h"
#include "ui/QueueStore.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

constexpr int kWarmupReps = 1;
constexpr int kMaxPicks = 100;
constexpr int kMaxSeeds = 1'000;
constexpr int kMaxReps = 100;
constexpr int kMaxPoolLimit = 1'000'000;

struct Options {
    int picks = 15;
    int seeds = 64;
    int reps = 7;
    int poolLimit = 5'000;
    bool allowIsolatedState = false;
};

struct SeedInput {
    QString path;
    TrackScorer::Candidate anchor;
};

struct Fixture {
    QVector<TrackScorer::Candidate> pool;
    QHash<QString, TrackScorer::Affinity> affinities;
    QHash<QString, double> genreIdf;
    TrackScorer::Weights weights;
    TrackScorer::RadioSessionDecay sessionDecay;
    QVector<SeedInput> seeds;
    QVector<TrackScorer::Candidate> anchors;
    QString poolPathDigest;
    qint64 nowSecs = 0;
    int exploration = 30;
};

struct SessionSet {
    std::vector<std::unique_ptr<QRandomGenerator>> generators;
    std::vector<std::unique_ptr<RadioSession>> sessions;
};

struct Selections {
    QVector<QStringList> pathsBySeed;
};

struct ResolvedSelections {
    QVector<QVector<Track>> tracksBySeed;
};

using Milliseconds = std::vector<double>;
using Clock = std::chrono::steady_clock;

QString usage(const char *program)
{
    return QStringLiteral("usage: %1 --allow-isolated-state [--picks N] [--seeds N] [--reps N] [--pool-limit N]")
        .arg(QString::fromLocal8Bit(program));
}

bool parseBoundedInt(const QString &option, const QString &text, int minimum, int maximum,
                     int *value, QString *error)
{
    bool ok = false;
    const qlonglong parsed = text.toLongLong(&ok);
    if (!ok || parsed < minimum || parsed > maximum) {
        if (error != nullptr) {
            *error = QStringLiteral("%1 requires an integer in [%2, %3]")
                         .arg(option)
                         .arg(minimum)
                         .arg(maximum);
        }
        return false;
    }
    *value = static_cast<int>(parsed);
    return true;
}

bool parseOptions(int argc, char **argv, Options *options, QString *error)
{
    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        if (argument == QLatin1String("--allow-isolated-state")) {
            if (options->allowIsolatedState) {
                *error = QStringLiteral("--allow-isolated-state may be supplied only once");
                return false;
            }
            options->allowIsolatedState = true;
            continue;
        }

        int *destination = nullptr;
        int minimum = 0;
        int maximum = 0;
        if (argument == QLatin1String("--picks")) {
            destination = &options->picks;
            minimum = 1;
            maximum = kMaxPicks;
        } else if (argument == QLatin1String("--seeds")) {
            destination = &options->seeds;
            minimum = 1;
            maximum = kMaxSeeds;
        } else if (argument == QLatin1String("--reps")) {
            destination = &options->reps;
            minimum = 1;
            maximum = kMaxReps;
        } else if (argument == QLatin1String("--pool-limit")) {
            destination = &options->poolLimit;
            minimum = 1;
            maximum = kMaxPoolLimit;
        } else {
            *error = QStringLiteral("unknown option: %1").arg(argument);
            return false;
        }

        if (index + 1 >= argc) {
            *error = QStringLiteral("%1 requires a value").arg(argument);
            return false;
        }
        const QString value = QString::fromLocal8Bit(argv[++index]);
        if (!parseBoundedInt(argument, value, minimum, maximum, destination, error)) {
            return false;
        }
    }
    return true;
}

bool requireIsolatedEnvironment(const Options &options, QString *error)
{
    if (!options.allowIsolatedState) {
        *error = QStringLiteral("--allow-isolated-state is required");
        return false;
    }
    if (qEnvironmentVariable("MUZAITEN_STATE_ROOT").trimmed().isEmpty()) {
        *error = QStringLiteral("MUZAITEN_STATE_ROOT must be explicitly set to a nonempty path");
        return false;
    }

    static constexpr const char *kPerCategoryOverrides[] = {
        "MUZAITEN_DATA_DIR",
        "MUZAITEN_STATE_DIR",
        "MUZAITEN_CACHE_DIR",
        "MUZAITEN_CONFIG_DIR",
    };
    for (const char *name : kPerCategoryOverrides) {
        if (!qEnvironmentVariable(name).trimmed().isEmpty()) {
            *error = QStringLiteral("%1 must be unset so MUZAITEN_STATE_ROOT controls every store")
                         .arg(QString::fromLatin1(name));
            return false;
        }
    }
    return true;
}

QStringList sortedUnique(QStringList paths)
{
    paths.removeAll(QString());
    paths.removeDuplicates();
    paths.sort();
    return paths;
}

QString pathDigest(const QStringList &paths)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QString &path : sortedUnique(paths)) {
        hash.addData(path.toUtf8());
        hash.addData(QByteArrayLiteral("\n"));
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool validEmbedding(const QVector<float> &embedding)
{
    if (embedding.isEmpty()) {
        return false;
    }
    double normSquared = 0.0;
    for (const float value : embedding) {
        if (!std::isfinite(value)) {
            return false;
        }
        normSquared += static_cast<double>(value) * static_cast<double>(value);
    }
    return std::isfinite(normSquared) && normSquared > 0.0;
}

std::optional<double> cosineDistance(const QVector<float> &left, const QVector<float> &right)
{
    if (left.isEmpty() || left.size() != right.size()) {
        return std::nullopt;
    }

    double dot = 0.0;
    double leftNormSquared = 0.0;
    double rightNormSquared = 0.0;
    for (qsizetype index = 0; index < left.size(); ++index) {
        const double leftValue = static_cast<double>(left.at(index));
        const double rightValue = static_cast<double>(right.at(index));
        if (!std::isfinite(leftValue) || !std::isfinite(rightValue)) {
            return std::nullopt;
        }
        dot += leftValue * rightValue;
        leftNormSquared += leftValue * leftValue;
        rightNormSquared += rightValue * rightValue;
    }
    if (!std::isfinite(dot) || !std::isfinite(leftNormSquared) || !std::isfinite(rightNormSquared)
        || leftNormSquared <= 0.0 || rightNormSquared <= 0.0) {
        return std::nullopt;
    }

    const double cosine = dot / std::sqrt(leftNormSquared * rightNormSquared);
    if (!std::isfinite(cosine)) {
        return std::nullopt;
    }
    return 1.0 - std::clamp(cosine, -1.0, 1.0);
}

std::optional<QVector<float>> weightedPlanningCentroid(const QVector<float> &seed,
                                                       const QStringList &pendingPaths,
                                                       const QHash<QString, qint64> &groupByPath,
                                                       const QHash<qint64, QVector<float>> &embeddings)
{
    if (!validEmbedding(seed)) {
        return std::nullopt;
    }

    QVector<double> sum(seed.size(), 0.0);
    const double seedWeight = std::exp2(-static_cast<double>(pendingPaths.size()) / 8.0);
    for (qsizetype index = 0; index < seed.size(); ++index) {
        sum[index] = seedWeight * static_cast<double>(seed.at(index));
    }

    for (const QString &path : pendingPaths) {
        const qint64 groupId = groupByPath.value(path, -1);
        const QVector<float> embedding = embeddings.value(groupId);
        if (!validEmbedding(embedding) || embedding.size() != seed.size()) {
            return std::nullopt;
        }
        for (qsizetype index = 0; index < embedding.size(); ++index) {
            sum[index] += 0.5 * static_cast<double>(embedding.at(index));
        }
    }

    double normSquared = 0.0;
    for (const double value : sum) {
        normSquared += value * value;
    }
    if (!std::isfinite(normSquared) || normSquared <= 0.0) {
        return std::nullopt;
    }

    const double norm = std::sqrt(normSquared);
    QVector<float> centroid;
    centroid.reserve(sum.size());
    for (const double value : sum) {
        const double normalized = value / norm;
        if (!std::isfinite(normalized)) {
            return std::nullopt;
        }
        centroid.push_back(static_cast<float>(normalized));
    }
    return centroid;
}

bool collectEligibleSeeds(AppCore &core, int requestedSeeds, QStringList *seedPaths,
                          qint64 *libraryTrackCount, FeatureStore::Status *featureStatus,
                          int *eligibleSeedCount, QString *error)
{
    Database *database = core.database();
    FeatureStore *features = core.features();
    if (database == nullptr || features == nullptr || !features->isOpen()) {
        *error = QStringLiteral("AppCore did not open the isolated library and feature stores");
        return false;
    }

    const QList<Database::TrackMatchRow> rows = database->trackMatchRows();
    QStringList libraryPaths;
    libraryPaths.reserve(rows.size());
    for (const auto &[path, artist, title, recordingMbid] : rows) {
        Q_UNUSED(artist);
        Q_UNUSED(title);
        Q_UNUSED(recordingMbid);
        libraryPaths.push_back(path);
    }
    libraryPaths = sortedUnique(std::move(libraryPaths));
    *libraryTrackCount = libraryPaths.size();

    *featureStatus = features->status();
    const QHash<QString, qint64> groupsByPath = features->contentGroupsForPaths(libraryPaths);
    QSet<qint64> groupSet;
    for (const QString &path : libraryPaths) {
        const qint64 groupId = groupsByPath.value(path, -1);
        if (groupId >= 0) {
            groupSet.insert(groupId);
        }
    }
    QVector<qint64> sortedGroups;
    const QList<qint64> groupValues = groupSet.values();
    sortedGroups.reserve(groupValues.size());
    for (const qint64 groupId : groupValues) {
        sortedGroups.push_back(groupId);
    }
    std::sort(sortedGroups.begin(), sortedGroups.end());
    QList<qint64> groupIds;
    groupIds.reserve(sortedGroups.size());
    for (const qint64 groupId : sortedGroups) {
        groupIds.push_back(groupId);
    }
    const QHash<qint64, QVector<float>> embeddings = features->embeddingsForGroups(groupIds);

    QStringList eligible;
    for (const QString &path : libraryPaths) {
        const qint64 groupId = groupsByPath.value(path, -1);
        if (groupId >= 0 && validEmbedding(embeddings.value(groupId))) {
            eligible.push_back(path);
        }
    }
    *eligibleSeedCount = eligible.size();
    if (eligible.size() < requestedSeeds) {
        *error = QStringLiteral("requested %1 embedded seed paths, but the isolated library has only %2; lower --seeds")
                     .arg(requestedSeeds)
                     .arg(eligible.size());
        return false;
    }

    seedPaths->clear();
    seedPaths->reserve(requestedSeeds);
    if (requestedSeeds == 1) {
        seedPaths->push_back(eligible.at((eligible.size() - 1) / 2));
        return true;
    }
    for (int index = 0; index < requestedSeeds; ++index) {
        const qsizetype eligibleIndex = (static_cast<qsizetype>(index) * (eligible.size() - 1))
            / (requestedSeeds - 1);
        seedPaths->push_back(eligible.at(eligibleIndex));
    }
    return true;
}

std::optional<Fixture> buildFixture(AppCore &core, const QStringList &seedPaths, int poolLimit,
                                    QString *error)
{
    Database *database = core.database();
    if (database == nullptr) {
        *error = QStringLiteral("AppCore has no database");
        return std::nullopt;
    }

    const QHash<QString, QString> genreAliases = database->genreAliases();
    const QSet<QString> ignoredRadioGenres = database->ignoredRadioGenres();
    const QHash<QString, QString> resolvedSongKeys = core.buildResolvedSongKeyMap();

    Fixture fixture;
    fixture.nowSecs = QDateTime::currentSecsSinceEpoch();
    fixture.exploration = core.radioExploration();
    fixture.weights = core.radioScoringWeights();
    fixture.sessionDecay = core.radioSessionDecay();
    fixture.affinities = core.buildRadioAffinities(resolvedSongKeys);
    fixture.genreIdf = core.buildRadioGenreIdf(genreAliases, ignoredRadioGenres);
    fixture.pool = core.buildRadioFallbackPool(poolLimit, genreAliases, ignoredRadioGenres, resolvedSongKeys);
    if (fixture.pool.isEmpty()) {
        *error = QStringLiteral("isolated library produced an empty radio candidate pool");
        return std::nullopt;
    }

    QStringList poolPaths;
    poolPaths.reserve(fixture.pool.size());
    QSet<QString> seenPoolPaths;
    for (const TrackScorer::Candidate &candidate : fixture.pool) {
        if (candidate.path.isEmpty()) {
            *error = QStringLiteral("radio candidate pool contained an empty path");
            return std::nullopt;
        }
        if (seenPoolPaths.contains(candidate.path)) {
            *error = QStringLiteral("radio candidate pool contained duplicate paths");
            return std::nullopt;
        }
        seenPoolPaths.insert(candidate.path);
        poolPaths.push_back(candidate.path);
    }
    fixture.poolPathDigest = pathDigest(poolPaths);

    fixture.seeds.reserve(seedPaths.size());
    fixture.anchors.reserve(seedPaths.size());
    for (const QString &path : seedPaths) {
        const Track seed = database->trackForPath(path);
        if (seed.path.isEmpty()) {
            *error = QStringLiteral("embedded seed path disappeared from the isolated library: %1").arg(path);
            return std::nullopt;
        }
        const QStringList genres = core.radioFoldedGenresForTrack(path, genreAliases, ignoredRadioGenres);
        const TrackScorer::Candidate anchor = core.buildRadioSeedCandidate(seed, genres, resolvedSongKeys);
        if (anchor.contentGroupId < 0) {
            *error = QStringLiteral("embedded seed has no content group: %1").arg(path);
            return std::nullopt;
        }
        fixture.seeds.push_back({path, anchor});
        fixture.anchors.push_back(anchor);
    }
    return fixture;
}

QHash<qint64, QVector<float>> hydrateEmbeddings(AppCore &core, const Fixture &fixture)
{
    return core.radioEmbeddingsForSession(fixture.pool, fixture.anchors);
}

bool sameEmbeddingKeys(const QHash<qint64, QVector<float>> &expected,
                       const QHash<qint64, QVector<float>> &actual)
{
    if (expected.size() != actual.size()) {
        return false;
    }
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        const auto actualIt = actual.constFind(it.key());
        if (actualIt == actual.constEnd() || actualIt->size() != it->size()) {
            return false;
        }
    }
    return true;
}

bool validateHydration(const Fixture &fixture, const QHash<qint64, QVector<float>> &embeddings,
                       QString *error)
{
    if (embeddings.isEmpty()) {
        *error = QStringLiteral("embedding hydration returned zero rows");
        return false;
    }
    for (const SeedInput &seed : fixture.seeds) {
        if (!validEmbedding(embeddings.value(seed.anchor.contentGroupId))) {
            *error = QStringLiteral("embedding hydration omitted seed group for %1").arg(seed.path);
            return false;
        }
    }
    return true;
}

SessionSet makeSessions(const Fixture &fixture, const QHash<qint64, QVector<float>> &embeddings)
{
    SessionSet result;
    result.generators.reserve(fixture.seeds.size());
    result.sessions.reserve(fixture.seeds.size());
    for (qsizetype index = 0; index < fixture.seeds.size(); ++index) {
        const quint32 seed = 0x9e3779b9U ^ static_cast<quint32>(index * 0x45d9f3bU);
        result.generators.push_back(std::make_unique<QRandomGenerator>(seed));
        const SeedInput &input = fixture.seeds.at(index);
        result.sessions.push_back(std::make_unique<RadioSession>(
            fixture.pool,
            fixture.affinities,
            fixture.genreIdf,
            QVector<TrackScorer::Candidate>{input.anchor},
            RadioSession::ContextMode::MovingContext,
            fixture.exploration,
            fixture.nowSecs,
            result.generators.back().get(),
            fixture.weights,
            embeddings,
            fixture.sessionDecay));
    }
    return result;
}

Track placeholderTrack(const QString &path)
{
    Track placeholder;
    placeholder.path = path;
    return placeholder;
}

bool scoreBatch(SessionSet &sessions, int picks, Selections *selections, QString *error)
{
    selections->pathsBySeed.clear();
    selections->pathsBySeed.reserve(sessions.sessions.size());
    for (qsizetype index = 0; index < static_cast<qsizetype>(sessions.sessions.size()); ++index) {
        const QVector<Track> tracks = sessions.sessions.at(index)->nextTracks(
            picks, {}, [](const QString &path) { return placeholderTrack(path); });
        if (tracks.size() != picks) {
            *error = QStringLiteral("batch scoring produced %1 picks for seed %2, expected %3")
                         .arg(tracks.size())
                         .arg(index)
                         .arg(picks);
            return false;
        }
        QStringList paths;
        paths.reserve(tracks.size());
        for (const Track &track : tracks) {
            if (track.path.isEmpty()) {
                *error = QStringLiteral("batch scoring produced an empty placeholder path");
                return false;
            }
            paths.push_back(track.path);
        }
        selections->pathsBySeed.push_back(std::move(paths));
    }
    return true;
}

bool scoreSingles(SessionSet &sessions, int picks, Selections *selections, QString *error)
{
    selections->pathsBySeed.clear();
    selections->pathsBySeed.reserve(sessions.sessions.size());
    for (qsizetype index = 0; index < static_cast<qsizetype>(sessions.sessions.size()); ++index) {
        QStringList paths;
        paths.reserve(picks);
        for (int pick = 0; pick < picks; ++pick) {
            const QVector<Track> tracks = sessions.sessions.at(index)->nextTracks(
                1, {}, [](const QString &path) { return placeholderTrack(path); });
            if (tracks.size() != 1 || tracks.first().path.isEmpty()) {
                *error = QStringLiteral("single scoring produced an invalid pick at seed %1, pick %2")
                             .arg(index)
                             .arg(pick);
                return false;
            }
            paths.push_back(tracks.first().path);
        }
        selections->pathsBySeed.push_back(std::move(paths));
    }
    return true;
}

bool sameSelections(const Selections &batch, const Selections &singles,
                    const SessionSet &batchSessions, const SessionSet &singleSessions,
                    QString *error)
{
    if (batch.pathsBySeed != singles.pathsBySeed) {
        *error = QStringLiteral("batch and single scoring diverged by path");
        return false;
    }
    for (qsizetype index = 0; index < static_cast<qsizetype>(batchSessions.sessions.size()); ++index) {
        const QJsonObject batchState = batchSessions.sessions.at(index)->constraintState();
        const QJsonObject singleState = singleSessions.sessions.at(index)->constraintState();
        if (!(QJsonValue(batchState) == QJsonValue(singleState))) {
            *error = QStringLiteral("batch and single scoring diverged in constraint state for seed %1")
                         .arg(index);
            return false;
        }
    }
    return true;
}

bool verifyBatchSingleEquivalence(const Fixture &fixture,
                                  const QHash<qint64, QVector<float>> &embeddings,
                                  int picks, Selections *selections, QString *error)
{
    SessionSet batchSessions = makeSessions(fixture, embeddings);
    SessionSet singleSessions = makeSessions(fixture, embeddings);
    Selections batch;
    Selections singles;
    if (!scoreBatch(batchSessions, picks, &batch, error)
        || !scoreSingles(singleSessions, picks, &singles, error)) {
        return false;
    }
    if (!sameSelections(batch, singles, batchSessions, singleSessions, error)) {
        return false;
    }
    *selections = std::move(batch);
    return true;
}

bool resolveSelections(AppCore &core, const Selections &selections, ResolvedSelections *resolved,
                       QString *error)
{
    Database *database = core.database();
    if (database == nullptr) {
        *error = QStringLiteral("AppCore has no database for GUI-thread pick resolution");
        return false;
    }
    const QSet<QString> neverRadioPaths = database->flaggedPaths(Database::TrackFlag::NeverRadio);
    resolved->tracksBySeed.clear();
    resolved->tracksBySeed.reserve(selections.pathsBySeed.size());
    for (qsizetype seedIndex = 0; seedIndex < selections.pathsBySeed.size(); ++seedIndex) {
        QSet<QString> blockedPaths = neverRadioPaths;
        QVector<Track> tracks;
        tracks.reserve(selections.pathsBySeed.at(seedIndex).size());
        for (const QString &path : selections.pathsBySeed.at(seedIndex)) {
            const Track resolvedTrack = core.resolveRadioPick(path, blockedPaths);
            if (resolvedTrack.path.isEmpty()) {
                *error = QStringLiteral("GUI-thread resolveRadioPick returned no track for seed %1 path %2")
                             .arg(seedIndex)
                             .arg(path);
                return false;
            }
            blockedPaths.insert(resolvedTrack.path);
            tracks.push_back(resolvedTrack);
        }
        if (tracks.size() != selections.pathsBySeed.at(seedIndex).size()) {
            *error = QStringLiteral("GUI-thread pick resolution returned zero rows for seed %1").arg(seedIndex);
            return false;
        }
        resolved->tracksBySeed.push_back(std::move(tracks));
    }
    return true;
}

void resetQueueState(PlayerCore &player, QueueStore &model)
{
    player.resetQueue({}, -1, -1);
    model.setSnapshot({}, -1, -1, -1);
}

bool batchQueueUpdate(PlayerCore &player, QueueStore &model, const ResolvedSelections &selections,
                      int picks, QString *error)
{
    resetQueueState(player, model);
    for (qsizetype seedIndex = 0; seedIndex < selections.tracksBySeed.size(); ++seedIndex) {
        const QVector<Track> &tracks = selections.tracksBySeed.at(seedIndex);
        if (tracks.size() != picks) {
            *error = QStringLiteral("batch queue input has %1 tracks for seed %2, expected %3")
                         .arg(tracks.size())
                         .arg(seedIndex)
                         .arg(picks);
            return false;
        }
        player.injectTracks(tracks);
        model.setSnapshot(player.queue(), player.queueIndex(), player.queueIndex() + 1,
                          player.playNextInsertIndex());
    }
    const int expected = static_cast<int>(selections.tracksBySeed.size()) * picks;
    if (player.queue().size() != expected || model.tracks().size() != expected) {
        *error = QStringLiteral("batch queue update processed %1/%2 tracks")
                     .arg(model.tracks().size())
                     .arg(expected);
        return false;
    }
    return true;
}

bool singleQueueUpdate(PlayerCore &player, QueueStore &model, const ResolvedSelections &selections,
                       int picks, QString *error)
{
    resetQueueState(player, model);
    for (qsizetype seedIndex = 0; seedIndex < selections.tracksBySeed.size(); ++seedIndex) {
        const QVector<Track> &tracks = selections.tracksBySeed.at(seedIndex);
        if (tracks.size() != picks) {
            *error = QStringLiteral("single queue input has %1 tracks for seed %2, expected %3")
                         .arg(tracks.size())
                         .arg(seedIndex)
                         .arg(picks);
            return false;
        }
        for (const Track &track : tracks) {
            player.injectTracks(QVector<Track>{track});
            model.setSnapshot(player.queue(), player.queueIndex(), player.queueIndex() + 1,
                              player.playNextInsertIndex());
        }
    }
    const int expected = static_cast<int>(selections.tracksBySeed.size()) * picks;
    if (player.queue().size() != expected || model.tracks().size() != expected) {
        *error = QStringLiteral("single queue update processed %1/%2 tracks")
                     .arg(model.tracks().size())
                     .arg(expected);
        return false;
    }
    return true;
}

bool collectDisplacement(const Fixture &fixture, const QHash<qint64, QVector<float>> &embeddings,
                         const Selections &selections, Milliseconds *planning,
                         Milliseconds *consecutive, Milliseconds *ratios, QString *error)
{
    QHash<QString, qint64> groupByPath;
    groupByPath.reserve(fixture.pool.size() + fixture.anchors.size());
    for (const TrackScorer::Candidate &candidate : fixture.pool) {
        groupByPath.insert(candidate.path, candidate.contentGroupId);
    }
    for (const TrackScorer::Candidate &anchor : fixture.anchors) {
        groupByPath.insert(anchor.path, anchor.contentGroupId);
    }

    planning->clear();
    consecutive->clear();
    ratios->clear();
    for (qsizetype seedIndex = 0; seedIndex < fixture.seeds.size(); ++seedIndex) {
        const SeedInput &seed = fixture.seeds.at(seedIndex);
        const QStringList &paths = selections.pathsBySeed.at(seedIndex);
        const QVector<float> seedEmbedding = embeddings.value(seed.anchor.contentGroupId);
        QVector<QVector<float>> pickEmbeddings;
        pickEmbeddings.reserve(paths.size());
        for (const QString &path : paths) {
            const QVector<float> embedding = embeddings.value(groupByPath.value(path, -1));
            if (!validEmbedding(embedding)) {
                *error = QStringLiteral("selected pick has no valid embedding for seed %1: %2")
                             .arg(seedIndex)
                             .arg(path);
                return false;
            }
            pickEmbeddings.push_back(embedding);
        }

        const std::optional<QVector<float>> centroid = weightedPlanningCentroid(
            seedEmbedding, paths, groupByPath, embeddings);
        if (!centroid) {
            *error = QStringLiteral("could not compute planning centroid for seed %1").arg(seedIndex);
            return false;
        }
        const std::optional<double> planningDistance = cosineDistance(seedEmbedding, *centroid);
        if (!planningDistance || !std::isfinite(*planningDistance)) {
            *error = QStringLiteral("planning displacement was not finite for seed %1").arg(seedIndex);
            return false;
        }
        planning->push_back(*planningDistance);

        double consecutiveSum = 0.0;
        int consecutiveCount = 0;
        for (qsizetype pickIndex = 1; pickIndex < pickEmbeddings.size(); ++pickIndex) {
            const std::optional<double> distance = cosineDistance(
                pickEmbeddings.at(pickIndex - 1), pickEmbeddings.at(pickIndex));
            if (!distance || !std::isfinite(*distance)) {
                *error = QStringLiteral("consecutive displacement was not finite for seed %1")
                             .arg(seedIndex);
                return false;
            }
            consecutive->push_back(*distance);
            consecutiveSum += *distance;
            ++consecutiveCount;
        }
        if (consecutiveCount > 0 && std::isfinite(consecutiveSum)) {
            const double meanConsecutive = consecutiveSum / static_cast<double>(consecutiveCount);
            if (meanConsecutive > 0.0 && std::isfinite(meanConsecutive)) {
                const double ratio = *planningDistance / meanConsecutive;
                if (std::isfinite(ratio) && ratio >= 0.0) {
                    ratios->push_back(ratio);
                }
            }
        }
    }
    if (planning->empty() || consecutive->empty()) {
        *error = QStringLiteral("displacement stages produced no samples");
        return false;
    }
    return true;
}

double percentile(const Milliseconds &samples, double quantile)
{
    if (samples.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    Milliseconds sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const double position = quantile * static_cast<double>(sorted.size());
    const std::size_t index = position <= 1.0
        ? 0
        : std::min(sorted.size() - 1, static_cast<std::size_t>(std::ceil(position)) - 1);
    return sorted.at(index);
}

double median(const Milliseconds &samples)
{
    return percentile(samples, 0.5);
}

bool positiveTimings(const char *name, const Milliseconds &samples, QString *error)
{
    if (samples.empty()) {
        *error = QStringLiteral("stage %1 has no timing samples").arg(QString::fromLatin1(name));
        return false;
    }
    for (const double sample : samples) {
        if (!std::isfinite(sample) || sample <= 0.0) {
            *error = QStringLiteral("stage %1 produced a non-positive or non-finite timing")
                         .arg(QString::fromLatin1(name));
            return false;
        }
    }
    return true;
}

template <typename Callable>
double measureMs(Callable &&callable)
{
    const auto start = Clock::now();
    callable();
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

void printTiming(const char *name, const Milliseconds &samples)
{
    std::printf("stage.%s.median_ms=%.6f\n", name, median(samples));
}

void printDistribution(const char *name, const Milliseconds &samples)
{
    std::printf("displacement.%s.sample_count=%zu\n", name, samples.size());
    std::printf("displacement.%s.p50=%.9g\n", name, percentile(samples, 0.5));
    std::printf("displacement.%s.p95=%.9g\n", name, percentile(samples, 0.95));
    std::printf("displacement.%s.maximum=%.9g\n", name, percentile(samples, 1.0));
}

void printRatio(const char *name, double numerator, double denominator)
{
    const double ratio = denominator > 0.0 ? numerator / denominator
                                          : std::numeric_limits<double>::quiet_NaN();
    if (std::isfinite(ratio)) {
        std::printf("ratio.%s=%.9g\n", name, ratio);
    } else {
        std::printf("ratio.%s=none\n", name);
    }
}

int fail(const QString &error)
{
    std::fprintf(stderr, "error=%s\n", qPrintable(error));
    return 1;
}

} // namespace

int main(int argc, char **argv)
{
    Options options;
    QString error;
    if (!parseOptions(argc, argv, &options, &error)) {
        std::fprintf(stderr, "%s\n%s\n", qPrintable(error), qPrintable(usage(argv[0])));
        return 2;
    }
    if (!requireIsolatedEnvironment(options, &error)) {
        std::fprintf(stderr, "error=%s\n", qPrintable(error));
        return 2;
    }

    QApplication application(argc, argv);
    AppCore core;
    Database *database = core.database();
    FeatureStore *features = core.features();
    if (database == nullptr || features == nullptr || !features->isOpen()) {
        return fail(QStringLiteral("isolated AppCore stores are not readable"));
    }

    qint64 libraryTrackCount = 0;
    FeatureStore::Status featureStatus;
    int eligibleSeedCount = 0;
    QStringList seedPaths;
    if (!collectEligibleSeeds(core, options.seeds, &seedPaths, &libraryTrackCount,
                              &featureStatus, &eligibleSeedCount, &error)) {
        return fail(error);
    }

    std::optional<Fixture> fixtureResult = buildFixture(core, seedPaths, options.poolLimit, &error);
    if (!fixtureResult) {
        return fail(error);
    }
    Fixture fixture = std::move(*fixtureResult);
    const QString seedPathDigest = pathDigest(seedPaths);

    // One unmeasured fixture build establishes the pool membership used by every
    // downstream stage. Rebuilds are timed below and must retain this digest.
    const QHash<qint64, QVector<float>> hydratedEmbeddings = hydrateEmbeddings(core, fixture);
    if (!validateHydration(fixture, hydratedEmbeddings, &error)) {
        return fail(error);
    }

    const QHash<qint64, QVector<float>> hydrationWarmup = hydrateEmbeddings(core, fixture);
    if (!sameEmbeddingKeys(hydratedEmbeddings, hydrationWarmup)) {
        return fail(QStringLiteral("embedding hydration changed its row set between warm-up calls"));
    }

    Selections selections;
    if (!verifyBatchSingleEquivalence(fixture, hydratedEmbeddings, options.picks, &selections, &error)) {
        return fail(error);
    }
    ResolvedSelections resolvedSelections;
    if (!resolveSelections(core, selections, &resolvedSelections, &error)) {
        return fail(error);
    }

    PlayerCore *player = core.player();
    if (player == nullptr) {
        return fail(QStringLiteral("AppCore has no PlayerCore for queue measurements"));
    }
    QueueStore queueModel;
    if (!batchQueueUpdate(*player, queueModel, resolvedSelections, options.picks, &error)) {
        return fail(error);
    }
    if (!singleQueueUpdate(*player, queueModel, resolvedSelections, options.picks, &error)) {
        return fail(error);
    }
    resetQueueState(*player, queueModel);

    Milliseconds prerequisitePoolTimings;
    Milliseconds hydrationTimings;
    Milliseconds constructionTimings;
    Milliseconds batchScoringTimings;
    Milliseconds singleScoringTimings;
    Milliseconds resolutionTimings;
    Milliseconds batchQueueTimings;
    Milliseconds singleQueueTimings;
    prerequisitePoolTimings.reserve(options.reps);
    hydrationTimings.reserve(options.reps);
    constructionTimings.reserve(options.reps);
    batchScoringTimings.reserve(options.reps);
    singleScoringTimings.reserve(options.reps);
    resolutionTimings.reserve(options.reps);
    batchQueueTimings.reserve(options.reps);
    singleQueueTimings.reserve(options.reps);

    std::optional<Fixture> poolWarmup = buildFixture(core, seedPaths, options.poolLimit, &error);
    if (!poolWarmup) {
        return fail(error);
    }
    if (poolWarmup->poolPathDigest != fixture.poolPathDigest
        || poolWarmup->pool.size() != fixture.pool.size()) {
        return fail(QStringLiteral("candidate pool membership changed during warm-up; raise --pool-limit above the library size"));
    }

    // The first pass through every measured operation is deliberately excluded
    // from the reported medians so database, allocator, and backend caches warm
    // without becoming part of the timing sample.
    {
        const QHash<qint64, QVector<float>> warmEmbeddings = hydrateEmbeddings(core, fixture);
        if (!sameEmbeddingKeys(hydratedEmbeddings, warmEmbeddings)) {
            return fail(QStringLiteral("embedding hydration warm-up changed its row set"));
        }
        SessionSet warmConstruction = makeSessions(fixture, warmEmbeddings);
        SessionSet warmBatch = makeSessions(fixture, warmEmbeddings);
        SessionSet warmSingles = makeSessions(fixture, warmEmbeddings);
        Selections warmSelections;
        if (!scoreBatch(warmBatch, options.picks, &warmSelections, &error)) {
            return fail(error);
        }
        if (!scoreSingles(warmSingles, options.picks, &warmSelections, &error)) {
            return fail(error);
        }
        ResolvedSelections warmResolved;
        if (!resolveSelections(core, selections, &warmResolved, &error)) {
            return fail(error);
        }
        if (!batchQueueUpdate(*player, queueModel, resolvedSelections, options.picks, &error)) {
            return fail(error);
        }
        if (!singleQueueUpdate(*player, queueModel, resolvedSelections, options.picks, &error)) {
            return fail(error);
        }
        resetQueueState(*player, queueModel);
        Q_UNUSED(warmConstruction);
        Q_UNUSED(kWarmupReps);
    }

    for (int rep = 0; rep < options.reps; ++rep) {
        const double elapsed = measureMs([&] {
            std::optional<Fixture> measured = buildFixture(core, seedPaths, options.poolLimit, &error);
            if (!measured) {
                return;
            }
            if (measured->poolPathDigest != fixture.poolPathDigest
                || measured->pool.size() != fixture.pool.size()) {
                error = QStringLiteral("candidate pool membership changed between repetitions; raise --pool-limit above the library size");
            }
        });
        prerequisitePoolTimings.push_back(elapsed);
        if (!error.isEmpty()) {
            return fail(error);
        }
    }

    for (int rep = 0; rep < options.reps; ++rep) {
        QHash<qint64, QVector<float>> measuredEmbeddings;
        const double elapsed = measureMs([&] { measuredEmbeddings = hydrateEmbeddings(core, fixture); });
        hydrationTimings.push_back(elapsed);
        if (!validateHydration(fixture, measuredEmbeddings, &error)
            || !sameEmbeddingKeys(hydratedEmbeddings, measuredEmbeddings)) {
            if (error.isEmpty()) {
                error = QStringLiteral("embedding hydration row set changed between repetitions");
            }
            return fail(error);
        }
    }

    for (int rep = 0; rep < options.reps; ++rep) {
        SessionSet measured;
        const double elapsed = measureMs([&] { measured = makeSessions(fixture, hydratedEmbeddings); });
        constructionTimings.push_back(elapsed);
        if (measured.sessions.size() != fixture.seeds.size()) {
            return fail(QStringLiteral("RadioSession construction produced zero sessions"));
        }
    }

    for (int rep = 0; rep < options.reps; ++rep) {
        SessionSet measured = makeSessions(fixture, hydratedEmbeddings);
        Selections measuredSelections;
        const double elapsed = measureMs([&] {
            if (!scoreBatch(measured, options.picks, &measuredSelections, &error)) {
                return;
            }
        });
        batchScoringTimings.push_back(elapsed);
        if (!error.isEmpty()) {
            return fail(error);
        }
    }

    for (int rep = 0; rep < options.reps; ++rep) {
        SessionSet measured = makeSessions(fixture, hydratedEmbeddings);
        Selections measuredSelections;
        const double elapsed = measureMs([&] {
            if (!scoreSingles(measured, options.picks, &measuredSelections, &error)) {
                return;
            }
        });
        singleScoringTimings.push_back(elapsed);
        if (!error.isEmpty()) {
            return fail(error);
        }
    }

    for (int rep = 0; rep < options.reps; ++rep) {
        ResolvedSelections measuredResolved;
        const double elapsed = measureMs([&] {
            if (!resolveSelections(core, selections, &measuredResolved, &error)) {
                return;
            }
        });
        resolutionTimings.push_back(elapsed);
        if (!error.isEmpty()) {
            return fail(error);
        }
    }

    for (int rep = 0; rep < options.reps; ++rep) {
        const double elapsed = measureMs([&] {
            if (!batchQueueUpdate(*player, queueModel, resolvedSelections, options.picks, &error)) {
                return;
            }
        });
        batchQueueTimings.push_back(elapsed);
        resetQueueState(*player, queueModel);
        if (!error.isEmpty()) {
            return fail(error);
        }
    }

    for (int rep = 0; rep < options.reps; ++rep) {
        const double elapsed = measureMs([&] {
            if (!singleQueueUpdate(*player, queueModel, resolvedSelections, options.picks, &error)) {
                return;
            }
        });
        singleQueueTimings.push_back(elapsed);
        resetQueueState(*player, queueModel);
        if (!error.isEmpty()) {
            return fail(error);
        }
    }

    if (!positiveTimings("prerequisites_pool", prerequisitePoolTimings, &error)
        || !positiveTimings("embedding_hydration", hydrationTimings, &error)
        || !positiveTimings("radio_session_construction", constructionTimings, &error)
        || !positiveTimings("batch_scoring", batchScoringTimings, &error)
        || !positiveTimings("single_scoring", singleScoringTimings, &error)
        || !positiveTimings("gui_pick_resolution", resolutionTimings, &error)
        || !positiveTimings("batch_queue_update", batchQueueTimings, &error)
        || !positiveTimings("single_queue_update", singleQueueTimings, &error)) {
        return fail(error);
    }

    Milliseconds planningDisplacement;
    Milliseconds consecutiveDisplacement;
    Milliseconds displacementRatios;
    if (!collectDisplacement(fixture, hydratedEmbeddings, selections,
                             &planningDisplacement, &consecutiveDisplacement,
                             &displacementRatios, &error)) {
        return fail(error);
    }

    const int queueBatchCalls = static_cast<int>(resolvedSelections.tracksBySeed.size());
    const int queueSingleCalls = queueBatchCalls * options.picks;
    int selectedPickCount = 0;
    for (const QStringList &paths : selections.pathsBySeed) {
        selectedPickCount += paths.size();
    }

    std::printf("timing=median_excluding_warmup\n");
    std::printf("warmup_reps=%d\n", kWarmupReps);
    std::printf("timed_reps=%d\n", options.reps);
    std::printf("picks=%d\n", options.picks);
    std::printf("seeds_requested=%d\n", options.seeds);
    std::printf("seeds_used=%d\n", fixture.seeds.size());
    std::printf("pool_limit=%d\n", options.poolLimit);
    std::printf("library_track_count=%lld\n", static_cast<long long>(libraryTrackCount));
    std::printf("feature_file_count=%lld\n", static_cast<long long>(featureStatus.files));
    std::printf("feature_group_count=%lld\n", static_cast<long long>(featureStatus.groups));
    std::printf("embedded_group_count=%lld\n", static_cast<long long>(featureStatus.embeddedGroups));
    std::printf("eligible_seed_count=%d\n", eligibleSeedCount);
    std::printf("seed_path_digest=%s\n", qPrintable(seedPathDigest));
    std::printf("pool_count=%d\n", fixture.pool.size());
    std::printf("pool_path_digest=%s\n", qPrintable(fixture.poolPathDigest));
    std::printf("hydrated_embedding_count=%d\n", hydratedEmbeddings.size());
    std::printf("selected_placeholder_count=%d\n", selectedPickCount);
    std::printf("resolved_pick_count=%d\n", selectedPickCount);
    std::printf("queue_batch_calls=%d\n", queueBatchCalls);
    std::printf("queue_single_calls=%d\n", queueSingleCalls);

    printTiming("prerequisites_pool", prerequisitePoolTimings);
    printTiming("embedding_hydration", hydrationTimings);
    printTiming("radio_session_construction", constructionTimings);
    printTiming("batch_scoring", batchScoringTimings);
    printTiming("single_scoring", singleScoringTimings);
    printTiming("gui_pick_resolution", resolutionTimings);
    printTiming("batch_queue_update", batchQueueTimings);
    printTiming("single_queue_update", singleQueueTimings);
    printRatio("scoring_single_over_batch", median(singleScoringTimings), median(batchScoringTimings));
    printRatio("queue_single_over_batch", median(singleQueueTimings), median(batchQueueTimings));

    printDistribution("planning", planningDisplacement);
    printDistribution("consecutive", consecutiveDisplacement);
    printDistribution("per_seed_ratio", displacementRatios);
    return 0;
}
