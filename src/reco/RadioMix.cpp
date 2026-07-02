#include "reco/RadioMix.h"

#include <QSet>

#include <algorithm>
#include <functional>
#include <limits>

namespace {

// Rediscovery starts strict so the mode feels meaningfully dormant; relaxing to
// 90 days only prevents small libraries from ending up empty.
constexpr int kRediscoveryLovedRating = 70;
constexpr int kRediscoveryAffinitySignal = 5;
constexpr int kRediscoveryStrictDays = 180;
constexpr int kRediscoveryRelaxedDays = 90;
constexpr int kRediscoveryRelaxThreshold = 50;

// Deep cuts are rare tracks from artists with real listening evidence. Artist
// affinity intentionally ignores local finished events and uses listen-count
// style totals so imported history can establish artist preference.
constexpr int kDeepCutsLikedArtistRating = 80;
constexpr int kDeepCutsMaxOwnPlays = 2;
constexpr int kDeepCutsMaxSkips = 0;
constexpr int kDeepCutsRejectRatingBelow = 40;

int rediscoverySignal(const TrackScorer::Affinity &affinity)
{
    return affinity.listenCount + affinity.baselineMax + affinity.finished;
}

int artistAffinitySignal(const TrackScorer::Affinity &affinity)
{
    return affinity.listenCount + affinity.baselineMax;
}

int ownPlaySignal(const TrackScorer::Affinity &affinity)
{
    return affinity.listenCount + affinity.baselineMax + affinity.finished;
}

qint64 secondsForDays(int days)
{
    return static_cast<qint64>(days) * 24 * 60 * 60;
}

QVector<TrackScorer::Candidate> filterRediscovery(
    const QVector<TrackScorer::Candidate> &candidates,
    const QHash<QString, TrackScorer::Affinity> &affinities,
    qint64 nowSecs,
    int minimumAgeDays)
{
    QVector<TrackScorer::Candidate> filtered;
    filtered.reserve(candidates.size());
    for (const TrackScorer::Candidate &candidate : candidates) {
        const TrackScorer::Affinity affinity = affinities.value(candidate.path);
        if (RadioMix::isRediscoveryCandidate(candidate, affinity, nowSecs, minimumAgeDays)) {
            filtered.push_back(candidate);
        }
    }
    return filtered;
}

QHash<QString, bool> likedArtistsForDeepCuts(
    const QVector<TrackScorer::Candidate> &candidates,
    const QHash<QString, TrackScorer::Affinity> &affinities)
{
    QHash<QString, int> artistScores;
    QSet<QString> ratedLovedArtists;
    for (const TrackScorer::Candidate &candidate : candidates) {
        if (candidate.artistFolded.isEmpty()) {
            continue;
        }
        artistScores[candidate.artistFolded] += artistAffinitySignal(affinities.value(candidate.path));
        if (candidate.hasUserRating && candidate.effectiveRating0To100 >= kDeepCutsLikedArtistRating) {
            ratedLovedArtists.insert(candidate.artistFolded);
        }
    }

    QVector<int> positiveScores;
    positiveScores.reserve(artistScores.size());
    for (auto it = artistScores.cbegin(); it != artistScores.cend(); ++it) {
        if (it.value() > 0) {
            positiveScores.push_back(it.value());
        }
    }
    std::sort(positiveScores.begin(), positiveScores.end(), std::greater<>());

    int topDecileThreshold = std::numeric_limits<int>::max();
    if (!positiveScores.isEmpty()) {
        const int topCount = std::max(1, (static_cast<int>(positiveScores.size()) + 9) / 10);
        topDecileThreshold = positiveScores.at(topCount - 1);
    }

    QHash<QString, bool> likedArtists;
    likedArtists.reserve(artistScores.size() + ratedLovedArtists.size());
    for (auto it = artistScores.cbegin(); it != artistScores.cend(); ++it) {
        if (it.value() > 0 && it.value() >= topDecileThreshold) {
            likedArtists.insert(it.key(), true);
        }
    }
    for (const QString &artist : ratedLovedArtists) {
        likedArtists.insert(artist, true);
    }
    return likedArtists;
}

QVector<TrackScorer::Candidate> filterDeepCuts(
    const QVector<TrackScorer::Candidate> &candidates,
    const QHash<QString, TrackScorer::Affinity> &affinities)
{
    const QHash<QString, bool> likedArtists = likedArtistsForDeepCuts(candidates, affinities);

    QVector<TrackScorer::Candidate> filtered;
    filtered.reserve(candidates.size());
    for (const TrackScorer::Candidate &candidate : candidates) {
        if (RadioMix::isDeepCutCandidate(candidate, likedArtists, affinities)) {
            filtered.push_back(candidate);
        }
    }
    return filtered;
}

} // namespace

namespace RadioMix {

std::optional<Mode> modeFromString(const QString &mode)
{
    const QString normalized = mode.trimmed().toLower();
    if (normalized == QLatin1String("rediscovery")) {
        return Mode::Rediscovery;
    }
    if (normalized == QLatin1String("deepcuts")) {
        return Mode::DeepCuts;
    }
    return std::nullopt;
}

QVector<TrackScorer::Candidate> filterCandidates(
    Mode mode,
    const QVector<TrackScorer::Candidate> &candidates,
    const QHash<QString, TrackScorer::Affinity> &affinities,
    qint64 nowSecs)
{
    switch (mode) {
    case Mode::Rediscovery: {
        const QVector<TrackScorer::Candidate> strict =
            filterRediscovery(candidates, affinities, nowSecs, kRediscoveryStrictDays);
        if (strict.size() >= kRediscoveryRelaxThreshold) {
            return strict;
        }
        return filterRediscovery(candidates, affinities, nowSecs, kRediscoveryRelaxedDays);
    }
    case Mode::DeepCuts:
        return filterDeepCuts(candidates, affinities);
    }
    return {};
}

bool isRediscoveryCandidate(const TrackScorer::Candidate &candidate,
                            const TrackScorer::Affinity &affinity,
                            qint64 nowSecs,
                            int minimumAgeDays)
{
    const bool lovedByRating =
        candidate.hasUserRating && candidate.effectiveRating0To100 >= kRediscoveryLovedRating;
    const bool lovedByAffinity = rediscoverySignal(affinity) >= kRediscoveryAffinitySignal;
    if (!lovedByRating && !lovedByAffinity) {
        return false;
    }
    if (affinity.lastPlayedAtSecs <= 0) {
        return false;
    }
    return nowSecs - affinity.lastPlayedAtSecs >= secondsForDays(minimumAgeDays);
}

bool isDeepCutCandidate(const TrackScorer::Candidate &candidate,
                        const QHash<QString, bool> &likedArtists,
                        const QHash<QString, TrackScorer::Affinity> &affinities)
{
    if (candidate.artistFolded.isEmpty() || !likedArtists.value(candidate.artistFolded)) {
        return false;
    }
    const TrackScorer::Affinity affinity = affinities.value(candidate.path);
    if (ownPlaySignal(affinity) > kDeepCutsMaxOwnPlays) {
        return false;
    }
    if (affinity.skipped > kDeepCutsMaxSkips) {
        return false;
    }
    return candidate.effectiveRating0To100 < 0
        || candidate.effectiveRating0To100 >= kDeepCutsRejectRatingBelow;
}

} // namespace RadioMix
