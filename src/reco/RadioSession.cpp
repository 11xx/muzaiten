#include "reco/RadioSession.h"

#include "core/FoldKey.h"
#include "core/GenreTags.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

// Draw from among the top-scoring candidates rather than always the single best:
// a deterministic top-1 queue quickly feels dead (same track every time the same
// context recurs). K is small so picks stay strongly on-theme.
constexpr int kTopK = 5;
// Hard sequencing throttles (enforced before scoring, not as score terms).
constexpr int kThrottleArtists = 3;   // no artist within the last 3 picks/plays
constexpr int kAlbumCap = 2;          // at most 2 tracks per album per session
// Mirrors ListenTracker::maxRequiredListenMs / the CASE in
// ListenHistoryStore::trackAffinities: the scrobble threshold is half a
// track's duration, capped at 4 minutes for very long tracks.
constexpr qint64 kMaxScrobbleThresholdMs = 4 * 60 * 1000;

// Keep only the most recent `limit` entries of a consecutive-deduped artist list.
void pushRecentArtist(QStringList &artists, const QString &folded, int limit)
{
    if (folded.isEmpty()) {
        return;
    }
    if (artists.isEmpty() || artists.last() != folded) {
        artists.push_back(folded);
    }
    while (artists.size() > limit) {
        artists.removeFirst();
    }
}

QJsonArray stringListToJson(const QStringList &strings)
{
    QJsonArray json;
    for (const QString &value : strings) {
        json.append(value);
    }
    return json;
}

QJsonArray stringSetToJson(const QSet<QString> &strings)
{
    QStringList sorted;
    sorted.reserve(strings.size());
    for (const QString &value : strings) {
        sorted.push_back(value);
    }
    sorted.sort();
    return stringListToJson(sorted);
}

QStringList stringListFromJson(const QJsonValue &value)
{
    QStringList strings;
    const QJsonArray array = value.toArray();
    strings.reserve(array.size());
    for (const QJsonValue &item : array) {
        const QString text = item.toString();
        if (!text.isEmpty()) {
            strings.push_back(text);
        }
    }
    return strings;
}

QSet<QString> stringSetFromJson(const QJsonValue &value)
{
    const QStringList strings = stringListFromJson(value);
    return QSet<QString>(strings.cbegin(), strings.cend());
}

} // namespace

RadioSession::RadioSession(QVector<TrackScorer::Candidate> pool,
                           QHash<QString, TrackScorer::Affinity> affinities,
                           QHash<QString, double> genreIdf,
                           TrackScorer::Candidate seed,
                           int exploration0To100,
                           qint64 nowSecs,
                           QRandomGenerator *rng,
                           TrackScorer::Weights weights)
    : m_pool(std::move(pool))
    , m_affinities(std::move(affinities))
    , m_genreIdf(std::move(genreIdf))
    , m_seed(std::move(seed))
    , m_weights(std::move(weights))
    , m_exploration(std::clamp(exploration0To100, 0, 100))
    , m_nowSecs(nowSecs)
    , m_rng(rng != nullptr ? rng : QRandomGenerator::global())
{
    // Stoplisted placeholder genres ("Other", "Unknown", ...) must never anchor
    // the rolling mood window — filter at the chokepoint where the seed's
    // genres enter it (notePlayed() filters the other entry point).
    m_seed.genresFolded = GenreTags::informative(m_seed.genresFolded);

    m_byPath.reserve(m_pool.size() + 1);
    for (const TrackScorer::Candidate &candidate : m_pool) {
        m_byPath.insert(candidate.path, candidate);
    }
    if (!m_seed.path.isEmpty()) {
        m_byPath.insert(m_seed.path, m_seed);
    }
    if (!m_seed.songKey.isEmpty()) {
        m_usedSongKeys.insert(m_seed.songKey);
    }
}

RadioSession::RadioSession(QVector<TrackScorer::Candidate> pool,
                           QHash<QString, TrackScorer::Affinity> affinities,
                           QHash<QString, double> genreIdf,
                           int exploration0To100,
                           qint64 nowSecs,
                           QRandomGenerator *rng,
                           TrackScorer::Weights weights)
    : RadioSession(std::move(pool), std::move(affinities), std::move(genreIdf),
                   TrackScorer::Candidate{}, exploration0To100, nowSecs, rng, std::move(weights))
{
}

QStringList RadioSession::rollingGenres() const
{
    // The seed always anchors the mood; the last few played tracks let it drift.
    QStringList genres = m_seed.genresFolded;
    QSet<QString> seen(genres.cbegin(), genres.cend());
    for (const QStringList &played : m_playedGenres) {
        for (const QString &genre : played) {
            if (!seen.contains(genre)) {
                seen.insert(genre);
                genres.push_back(genre);
            }
        }
    }
    return genres;
}

void RadioSession::recordPick(const TrackScorer::Candidate &candidate, const TrackScorer::Scored &scored)
{
    m_usedPaths.insert(candidate.path);
    if (!candidate.songKey.isEmpty()) {
        m_usedSongKeys.insert(candidate.songKey);
    }
    m_albumCounts[candidate.albumKey] += 1;
    m_pickReasons.insert(candidate.path, scored.components);
    m_pickReasonOrder.push_back(candidate.path);
}

QVector<Track> RadioSession::nextTracks(int count, const QSet<QString> &excludePaths,
                                        const std::function<Track(const QString &path)> &resolveTrack)
{
    QVector<Track> result;
    if (count <= 0) {
        return result;
    }
    // A batch-local recent-artist list so a multi-pick call throttles within
    // itself the same way successive single picks (fed back via notePlayed) do.
    QStringList batchArtists = m_recentArtists;

    for (int picked = 0; picked < count; ++picked) {
        TrackScorer::SeedContext context;
        context.genresFolded = rollingGenres();
        context.genreIdf = m_genreIdf;
        context.recentArtistsFolded = QSet<QString>(batchArtists.cbegin(), batchArtists.cend());
        context.year = m_seed.year;
        context.nowSecs = m_nowSecs;
        context.exploration0To100 = m_exploration;

        const QSet<QString> throttled = context.recentArtistsFolded;
        QSet<QString> excludedSongKeys;
        for (const QString &path : excludePaths) {
            const auto it = m_byPath.constFind(path);
            if (it != m_byPath.constEnd() && !it->songKey.isEmpty()) {
                excludedSongKeys.insert(it->songKey);
            }
        }

        QList<std::pair<TrackScorer::Scored, const TrackScorer::Candidate *>> scored;
        scored.reserve(m_pool.size());
        for (const TrackScorer::Candidate &candidate : m_pool) {
            if (candidate.path.isEmpty() || m_usedPaths.contains(candidate.path)
                || excludePaths.contains(candidate.path)) {
                continue;
            }
            if (!candidate.songKey.isEmpty()
                && (m_usedSongKeys.contains(candidate.songKey) || excludedSongKeys.contains(candidate.songKey))) {
                continue;
            }
            if (!candidate.artistFolded.isEmpty() && throttled.contains(candidate.artistFolded)) {
                continue;
            }
            if (m_albumCounts.value(candidate.albumKey) >= kAlbumCap) {
                continue;
            }
            scored.push_back({TrackScorer::score(candidate, m_affinities.value(candidate.path), context, m_weights),
                              &candidate});
        }
        if (scored.isEmpty()) {
            break;
        }

        std::sort(scored.begin(), scored.end(), [](const auto &left, const auto &right) {
            return left.first.score > right.first.score;
        });
        const int topN = std::min<int>(kTopK, static_cast<int>(scored.size()));

        // Weighted-random draw among the top N. Scores can be negative, so shift
        // by the batch minimum plus a floor to keep every weight positive while
        // preserving the ordering's relative pull.
        double minScore = scored.front().first.score;
        for (int i = 0; i < topN; ++i) {
            minScore = std::min(minScore, scored.at(i).first.score);
        }
        double total = 0.0;
        for (int i = 0; i < topN; ++i) {
            total += (scored.at(i).first.score - minScore) + 0.001;
        }
        double roll = m_rng->generateDouble() * total;
        int chosenIndex = topN - 1;
        for (int i = 0; i < topN; ++i) {
            roll -= (scored.at(i).first.score - minScore) + 0.001;
            if (roll <= 0.0) {
                chosenIndex = i;
                break;
            }
        }

        const TrackScorer::Candidate &chosen = *scored.at(chosenIndex).second;
        recordPick(chosen, scored.at(chosenIndex).first);
        pushRecentArtist(batchArtists, chosen.artistFolded, kThrottleArtists);

        const Track resolved = resolveTrack(chosen.path);
        if (!resolved.path.isEmpty()) {
            result.push_back(resolved);
        }
    }
    return result;
}

void RadioSession::setExploration(int exploration0To100)
{
    m_exploration = std::clamp(exploration0To100, 0, 100);
}

bool RadioSession::isEarlySkip(qint64 playedMs, qint64 durationMs)
{
    const qint64 threshold = durationMs > 0 ? std::min(durationMs / 2, kMaxScrobbleThresholdMs)
                                             : kMaxScrobbleThresholdMs;
    return playedMs < threshold;
}

void RadioSession::notePlayed(const Track &track)
{
    if (track.path.isEmpty()) {
        return;
    }
    pushRecentArtist(m_recentArtists, FoldKey::fold(track.artistName), kThrottleArtists);

    QStringList genres;
    QString albumKey = FoldKey::albumKey(track.albumArtistName, track.albumTitle);
    QString songKey = FoldKey::songKey(track.musicBrainz.recordingId, track.artistName, track.title);
    const auto it = m_byPath.constFind(track.path);
    if (it != m_byPath.constEnd()) {
        // Filter here too: pool candidates already carry canonical genre keys,
        // and this is the other chokepoint genres enter the rolling context
        // through.
        genres = GenreTags::informative(it->genresFolded);
        albumKey = it->albumKey;
        songKey = it->songKey;
    }
    m_playedGenres.push_back(genres);
    while (m_playedGenres.size() > kThrottleArtists) {
        m_playedGenres.removeFirst();
    }

    // Count the album only once: a radio pick already tallied it at pick time;
    // the seed and user-queued interruptions get counted here (first sighting).
    if (!m_usedPaths.contains(track.path)) {
        m_usedPaths.insert(track.path);
        m_albumCounts[albumKey] += 1;
    }
    if (!songKey.isEmpty()) {
        m_usedSongKeys.insert(songKey);
    }
}

QString RadioSession::reasonFor(const QString &path) const
{
    const auto it = m_pickReasons.constFind(path);
    if (it == m_pickReasons.constEnd() || it->isEmpty()) {
        return {};
    }
    // Terse and data-driven: strongest-contributing components first, each as
    // "name +/-value" rounded to one decimal. Stage 3 dresses this up for the UI.
    QList<TrackScorer::Component> components = *it;
    std::sort(components.begin(), components.end(), [](const auto &left, const auto &right) {
        return std::abs(left.value) > std::abs(right.value);
    });
    QStringList parts;
    parts.reserve(components.size());
    for (const TrackScorer::Component &component : components) {
        const double rounded = std::round(component.value * 10.0) / 10.0;
        parts.push_back(QStringLiteral("%1 %2%3")
                            .arg(component.name,
                                 rounded >= 0.0 ? QStringLiteral("+") : QStringLiteral("-"))
                            .arg(std::abs(rounded), 0, 'f', 1));
    }
    return parts.join(QStringLiteral("; "));
}

QList<TrackScorer::Component> RadioSession::reasonComponentsFor(const QString &path) const
{
    return m_pickReasons.value(path);
}

QVector<RadioSession::PickReason> RadioSession::pickReasons() const
{
    QVector<PickReason> reasons;
    reasons.reserve(m_pickReasonOrder.size());
    for (const QString &path : m_pickReasonOrder) {
        const auto it = m_pickReasons.constFind(path);
        if (it != m_pickReasons.constEnd()) {
            reasons.push_back(PickReason{path, *it});
        }
    }
    return reasons;
}

QJsonObject RadioSession::constraintState() const
{
    QJsonObject albumCounts;
    QStringList albumKeys = m_albumCounts.keys();
    albumKeys.sort();
    for (const QString &albumKey : albumKeys) {
        albumCounts.insert(albumKey, m_albumCounts.value(albumKey));
    }

    QJsonArray playedGenres;
    for (const QStringList &genres : m_playedGenres) {
        playedGenres.append(stringListToJson(genres));
    }

    return QJsonObject{
        {QStringLiteral("usedSongKeys"), stringSetToJson(m_usedSongKeys)},
        {QStringLiteral("usedPaths"), stringSetToJson(m_usedPaths)},
        {QStringLiteral("albumGroupCounts"), albumCounts},
        {QStringLiteral("recentArtists"), stringListToJson(m_recentArtists)},
        {QStringLiteral("playedGenres"), playedGenres},
    };
}

void RadioSession::restoreConstraintState(const QJsonObject &state)
{
    if (state.contains(QStringLiteral("usedSongKeys"))) {
        m_usedSongKeys = stringSetFromJson(state.value(QStringLiteral("usedSongKeys")));
    }
    if (state.contains(QStringLiteral("usedPaths"))) {
        m_usedPaths = stringSetFromJson(state.value(QStringLiteral("usedPaths")));
    }
    if (state.contains(QStringLiteral("albumGroupCounts"))) {
        m_albumCounts.clear();
        const QJsonObject counts = state.value(QStringLiteral("albumGroupCounts")).toObject();
        for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
            const int count = it.value().toInt(0);
            if (!it.key().isEmpty() && count > 0) {
                m_albumCounts.insert(it.key(), count);
            }
        }
    }
    if (state.contains(QStringLiteral("recentArtists"))) {
        m_recentArtists = stringListFromJson(state.value(QStringLiteral("recentArtists")));
        while (m_recentArtists.size() > kThrottleArtists) {
            m_recentArtists.removeFirst();
        }
    }
    if (state.contains(QStringLiteral("playedGenres"))) {
        m_playedGenres.clear();
        const QJsonArray groups = state.value(QStringLiteral("playedGenres")).toArray();
        for (const QJsonValue &group : groups) {
            m_playedGenres.push_back(stringListFromJson(group));
        }
        while (m_playedGenres.size() > kThrottleArtists) {
            m_playedGenres.removeFirst();
        }
    }
    m_pickReasons.clear();
}
