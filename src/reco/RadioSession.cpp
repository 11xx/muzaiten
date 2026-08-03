#include "reco/RadioSession.h"

#include "core/FoldKey.h"
#include "core/GenreTags.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRandomGenerator>

#include <algorithm>
#include <cmath>
#include <limits>
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
constexpr double kAnchorContextHalfLifePicks = 8.0;
constexpr double kConfirmedContextDecayPicks = 3.0;
constexpr double kPendingContextWeight = 0.5;
// Entries below this weight cannot change a normalized context observably.
constexpr double kConfirmedContextMinimumWeight = 1e-6;
// 60 entries keep the oldest weight at 2^(-59/3), above the pruning threshold.
constexpr int kConfirmedContextMaximumEntries = 60;
constexpr int kMaximumCandidateYear = 9999;
constexpr double kMaximumCandidateTempoBpm = 1000.0;
constexpr double kMaximumCandidateEnergy = 1.0;
constexpr quint64 kOwnedRngMultiplier = 2685821657736338717ULL;

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

QStringList stringListFromJson(const QJsonValue &value);

QJsonObject candidateToJson(const TrackScorer::Candidate &candidate, const QString &sourcePath)
{
    QJsonObject object{
        {QStringLiteral("path"), candidate.path},
        {QStringLiteral("contentGroupId"), QString::number(candidate.contentGroupId)},
        {QStringLiteral("songKey"), candidate.songKey},
        {QStringLiteral("artistFolded"), candidate.artistFolded},
        {QStringLiteral("albumKey"), candidate.albumKey},
        {QStringLiteral("genresFolded"), stringListToJson(GenreTags::informative(candidate.genresFolded))},
        {QStringLiteral("year"), candidate.year},
        {QStringLiteral("tempoBpm"), candidate.tempoBpm},
        {QStringLiteral("energy"), candidate.energy},
        {QStringLiteral("effectiveRating0To100"), candidate.effectiveRating0To100},
        {QStringLiteral("hasUserRating"), candidate.hasUserRating},
        {QStringLiteral("sourcePath"), sourcePath},
    };
    return object;
}

bool qint64NumberFromJson(const QJsonValue &value, qint64 minimum, qint64 maximum,
                          qint64 *result)
{
    if (result == nullptr || !value.isDouble()) {
        return false;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::trunc(number) != number) {
        return false;
    }
    const long double widened = static_cast<long double>(number);
    if (widened < static_cast<long double>(minimum)
        || (maximum == std::numeric_limits<qint64>::max()
            ? number >= std::ldexp(1.0, 63)
            : widened > static_cast<long double>(maximum))) {
        return false;
    }
    *result = static_cast<qint64>(number);
    return true;
}

bool candidateGroupFromJson(const QJsonValue &value, qint64 *group)
{
    if (value.isString()) {
        bool ok = false;
        const qint64 parsed = value.toString().toLongLong(&ok, 10);
        if (!ok || parsed < -1) {
            return false;
        }
        if (group != nullptr) {
            *group = parsed;
        }
        return true;
    }
    return qint64NumberFromJson(value, -1, std::numeric_limits<qint64>::max(), group);
}

bool candidateIdentityCompatible(const TrackScorer::Candidate &source,
                                 const TrackScorer::Candidate &alias)
{
    if (!source.songKey.isEmpty() || !alias.songKey.isEmpty()) {
        return !source.songKey.isEmpty() && source.songKey == alias.songKey;
    }
    return !source.artistFolded.isEmpty() && source.artistFolded == alias.artistFolded
        && !source.albumKey.isEmpty() && source.albumKey == alias.albumKey;
}

bool candidateFromJson(const QJsonObject &object, TrackScorer::Candidate *candidate,
                       QString *sourcePath)
{
    if (candidate == nullptr || !object.value(QStringLiteral("path")).isString()) {
        return false;
    }
    const QString path = object.value(QStringLiteral("path")).toString();
    if (path.isEmpty()) {
        return false;
    }

    TrackScorer::Candidate restored;
    restored.path = path;
    if (object.contains(QStringLiteral("contentGroupId"))
        && !candidateGroupFromJson(object.value(QStringLiteral("contentGroupId")),
                                    &restored.contentGroupId)) {
        return false;
    }

    const auto readString = [&](const QString &key, QString *target) {
        if (!object.contains(key)) {
            return true;
        }
        const QJsonValue value = object.value(key);
        if (!value.isString()) {
            return false;
        }
        *target = value.toString();
        return true;
    };
    if (!readString(QStringLiteral("songKey"), &restored.songKey)
        || !readString(QStringLiteral("artistFolded"), &restored.artistFolded)
        || !readString(QStringLiteral("albumKey"), &restored.albumKey)) {
        return false;
    }

    if (object.contains(QStringLiteral("genresFolded"))) {
        const QJsonValue value = object.value(QStringLiteral("genresFolded"));
        if (!value.isArray()) {
            return false;
        }
        QStringList genres;
        for (const QJsonValue &item : value.toArray()) {
            if (!item.isString()) {
                return false;
            }
            genres.push_back(item.toString());
        }
        restored.genresFolded = GenreTags::informative(genres);
    }

    qint64 integer = 0;
    if (object.contains(QStringLiteral("year"))) {
        if (!qint64NumberFromJson(object.value(QStringLiteral("year")), 0, kMaximumCandidateYear,
                                  &integer)) {
            return false;
        }
        restored.year = static_cast<int>(integer);
    }
    if (object.contains(QStringLiteral("tempoBpm"))) {
        const QJsonValue value = object.value(QStringLiteral("tempoBpm"));
        if (!value.isDouble()) {
            return false;
        }
        const double tempo = value.toDouble();
        if (!std::isfinite(tempo)
            || (tempo != -1.0 && (tempo <= 0.0 || tempo > kMaximumCandidateTempoBpm))) {
            return false;
        }
        restored.tempoBpm = tempo;
    }
    if (object.contains(QStringLiteral("energy"))) {
        const QJsonValue value = object.value(QStringLiteral("energy"));
        if (!value.isDouble()) {
            return false;
        }
        const double energy = value.toDouble();
        if (!std::isfinite(energy)
            || (energy != -1.0 && (energy < 0.0 || energy > kMaximumCandidateEnergy))) {
            return false;
        }
        restored.energy = energy;
    }
    if (object.contains(QStringLiteral("effectiveRating0To100"))) {
        if (!qint64NumberFromJson(object.value(QStringLiteral("effectiveRating0To100")), -1, 100,
                                  &integer)) {
            return false;
        }
        restored.effectiveRating0To100 = static_cast<int>(integer);
    }
    if (object.contains(QStringLiteral("hasUserRating"))) {
        const QJsonValue value = object.value(QStringLiteral("hasUserRating"));
        if (!value.isBool()) {
            return false;
        }
        restored.hasUserRating = value.toBool();
    }

    *candidate = std::move(restored);
    if (sourcePath != nullptr) {
        if (object.contains(QStringLiteral("sourcePath"))) {
            const QJsonValue value = object.value(QStringLiteral("sourcePath"));
            if (!value.isString()) {
                return false;
            }
            *sourcePath = value.toString();
        }
        if (sourcePath->isEmpty()) {
            *sourcePath = path;
        }
    }
    return true;
}

QString anchorIdentity(const TrackScorer::Candidate &anchor)
{
    if (!anchor.path.isEmpty()) {
        return QStringLiteral("path:") + anchor.path;
    }
    if (!anchor.songKey.isEmpty()) {
        return QStringLiteral("song:") + anchor.songKey;
    }

    QStringList genres = GenreTags::informative(anchor.genresFolded);
    genres.sort();
    return QStringLiteral("value:%1\x1f%2\x1f%3\x1f%4\x1f%5\x1f%6\x1f%7\x1f%8\x1f%9")
        .arg(anchor.contentGroupId)
        .arg(anchor.artistFolded)
        .arg(anchor.albumKey)
        .arg(genres.join(QLatin1Char('\x1e')))
        .arg(anchor.year)
        .arg(anchor.tempoBpm, 0, 'g', 17)
        .arg(anchor.energy, 0, 'g', 17)
        .arg(anchor.effectiveRating0To100)
        .arg(anchor.hasUserRating ? 1 : 0);
}

int trackYear(const Track &track)
{
    for (const QString &date : {track.originalDate, track.date}) {
        const QString yearText = date.trimmed().left(4);
        bool ok = false;
        const int year = yearText.toInt(&ok);
        if (ok && year > 0) {
            return year;
        }
    }
    return 0;
}

TrackScorer::Candidate candidateFromTrack(const Track &track)
{
    TrackScorer::Candidate candidate;
    candidate.path = track.path;
    candidate.songKey = FoldKey::songKey(track.musicBrainz.recordingId,
                                         track.artistName, track.title);
    candidate.artistFolded = FoldKey::fold(track.artistName);
    candidate.albumKey = FoldKey::albumKey(track.albumArtistName, track.albumTitle);
    candidate.year = trackYear(track);
    candidate.effectiveRating0To100 = track.effectiveRating0To100;
    candidate.hasUserRating = track.hasUserRating;
    return candidate;
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

QJsonArray groupListToJson(const QList<qint64> &groups)
{
    QJsonArray json;
    for (qint64 group : groups) {
        json.append(QString::number(group));
    }
    return json;
}

QList<qint64> groupListFromJson(const QJsonValue &value)
{
    QList<qint64> groups;
    const QJsonArray array = value.toArray();
    groups.reserve(array.size());
    for (const QJsonValue &item : array) {
        bool ok = false;
        const qint64 group = item.toString().toLongLong(&ok);
        groups.push_back(ok ? group : -1);
    }
    return groups;
}

bool addEmbedding(QVector<double> &sum, int &count, const QVector<float> &embedding)
{
    if (embedding.isEmpty()) {
        return false;
    }
    if (sum.isEmpty()) {
        sum.resize(embedding.size());
    }
    if (sum.size() != embedding.size()) {
        return false;
    }
    for (qsizetype i = 0; i < embedding.size(); ++i) {
        sum[i] += static_cast<double>(embedding.at(i));
    }
    ++count;
    return true;
}

QVector<float> normalizedMean(const QVector<double> &sum, int count)
{
    if (sum.isEmpty() || count <= 0) {
        return {};
    }
    double norm = 0.0;
    for (double value : sum) {
        const double mean = value / static_cast<double>(count);
        norm += mean * mean;
    }
    norm = std::sqrt(norm);
    if (norm <= 0.0 || !std::isfinite(norm)) {
        return {};
    }

    QVector<float> centroid;
    centroid.reserve(sum.size());
    for (double value : sum) {
        centroid.push_back(static_cast<float>((value / static_cast<double>(count)) / norm));
    }
    return centroid;
}

bool addWeightedEmbedding(QVector<double> &sum, double &weightSum,
                          const QVector<float> &embedding, double weight)
{
    if (embedding.isEmpty() || weight <= 0.0 || !std::isfinite(weight)
        || !std::all_of(embedding.cbegin(), embedding.cend(), [](float value) {
               return std::isfinite(value);
           })) {
        return false;
    }
    if (sum.isEmpty()) {
        sum.resize(embedding.size());
    }
    if (sum.size() != embedding.size()) {
        return false;
    }
    for (qsizetype i = 0; i < embedding.size(); ++i) {
        sum[i] += static_cast<double>(embedding.at(i)) * weight;
    }
    weightSum += weight;
    return true;
}

QVector<float> normalizedWeightedMean(const QVector<double> &sum, double weightSum)
{
    if (sum.isEmpty() || weightSum <= 0.0 || !std::isfinite(weightSum)) {
        return {};
    }
    double norm = 0.0;
    for (double value : sum) {
        const double mean = value / weightSum;
        norm += mean * mean;
    }
    norm = std::sqrt(norm);
    if (norm <= 0.0 || !std::isfinite(norm)) {
        return {};
    }

    QVector<float> centroid;
    centroid.reserve(sum.size());
    for (double value : sum) {
        centroid.push_back(static_cast<float>((value / weightSum) / norm));
    }
    return centroid;
}

} // namespace

RadioSession::RadioSession(QVector<TrackScorer::Candidate> pool,
                           QHash<QString, TrackScorer::Affinity> affinities,
                           QHash<QString, double> genreIdf,
                           TrackScorer::Candidate seed,
                           int exploration0To100,
                           qint64 nowSecs,
                           QRandomGenerator *rng,
                           TrackScorer::Weights weights,
                           QHash<qint64, QVector<float>> embeddingsByGroup,
                           TrackScorer::RadioSessionDecay sessionDecay)
    : m_pool(std::move(pool))
    , m_affinities(std::move(affinities))
    , m_genreIdf(std::move(genreIdf))
    , m_embeddingsByGroup(std::move(embeddingsByGroup))
    , m_seed(std::move(seed))
    , m_weights(std::move(weights))
    , m_sessionDecay(std::move(sessionDecay))
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
    pushRecentArtist(m_recentArtists, m_seed.artistFolded, kThrottleArtists);
}

RadioSession::RadioSession(QVector<TrackScorer::Candidate> pool,
                           QHash<QString, TrackScorer::Affinity> affinities,
                           QHash<QString, double> genreIdf,
                           int exploration0To100,
                           qint64 nowSecs,
                           QRandomGenerator *rng,
                           TrackScorer::Weights weights,
                           QHash<qint64, QVector<float>> embeddingsByGroup,
                           TrackScorer::RadioSessionDecay sessionDecay)
    : RadioSession(std::move(pool), std::move(affinities), std::move(genreIdf),
                   TrackScorer::Candidate{}, exploration0To100, nowSecs, rng, std::move(weights),
                   std::move(embeddingsByGroup), std::move(sessionDecay))
{
}

RadioSession::RadioSession(QVector<TrackScorer::Candidate> pool,
                           QHash<QString, TrackScorer::Affinity> affinities,
                           QHash<QString, double> genreIdf,
                           QVector<TrackScorer::Candidate> anchors,
                           ContextMode contextMode,
                           int exploration0To100,
                           qint64 nowSecs,
                           QRandomGenerator *rng,
                           TrackScorer::Weights weights,
                           QHash<qint64, QVector<float>> embeddingsByGroup,
                           TrackScorer::RadioSessionDecay sessionDecay)
    : m_contextMode(contextMode)
    , m_vectorAnchorSession(true)
    , m_anchors(std::move(anchors))
    , m_pool(std::move(pool))
    , m_affinities(std::move(affinities))
    , m_genreIdf(std::move(genreIdf))
    , m_embeddingsByGroup(std::move(embeddingsByGroup))
    , m_weights(std::move(weights))
    , m_sessionDecay(std::move(sessionDecay))
    , m_exploration(std::clamp(exploration0To100, 0, 100))
    , m_nowSecs(nowSecs)
    , m_rng(rng != nullptr ? rng : QRandomGenerator::global())
{
    for (TrackScorer::Candidate &anchor : m_anchors) {
        anchor.genresFolded = GenreTags::informative(anchor.genresFolded);
        if (!anchor.path.isEmpty()) {
            m_anchorPaths.insert(anchor.path);
        }
        if (!anchor.songKey.isEmpty()) {
            m_anchorSongKeys.insert(anchor.songKey);
        }
    }
    if (!m_anchors.isEmpty()) {
        m_seed = m_anchors.first();
    }

    m_usesOwnedRng = m_contextMode == ContextMode::MovingContext || m_anchors.size() > 1;
    if (m_usesOwnedRng) {
        m_ownedRngState = m_rng->generate64();
        if (m_ownedRngState == 0) {
            m_ownedRngState = 1;
        }
    }

    m_byPath.reserve(m_pool.size() + m_anchors.size());
    for (const TrackScorer::Candidate &candidate : m_pool) {
        m_byPath.insert(candidate.path, candidate);
    }
    for (const TrackScorer::Candidate &anchor : m_anchors) {
        if (!anchor.path.isEmpty()) {
            m_byPath.insert(anchor.path, anchor);
        }
    }
    if (m_contextMode == ContextMode::PermanentAnchor) {
        if (!m_seed.songKey.isEmpty()) {
            m_usedSongKeys.insert(m_seed.songKey);
        }
        pushRecentArtist(m_recentArtists, m_seed.artistFolded, kThrottleArtists);
    }
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

double RadioSession::rollingTempoBpm() const
{
    double sum = 0.0;
    int count = 0;
    for (const PlayedScalars &scalars : m_playedScalars) {
        if (scalars.tempoBpm > 0.0) {
            sum += scalars.tempoBpm;
            ++count;
        }
    }
    return count > 0 ? sum / count : m_seed.tempoBpm;
}

double RadioSession::rollingEnergy() const
{
    double sum = 0.0;
    int count = 0;
    for (const PlayedScalars &scalars : m_playedScalars) {
        if (scalars.energy >= 0.0) {
            sum += scalars.energy;
            ++count;
        }
    }
    return count > 0 ? sum / count : m_seed.energy;
}

QVector<float> RadioSession::rollingAudioCentroid() const
{
    QVector<double> sum;
    int count = 0;
    addEmbedding(sum, count, m_embeddingsByGroup.value(m_seed.contentGroupId));
    for (qint64 groupId : m_playedContentGroups) {
        addEmbedding(sum, count, m_embeddingsByGroup.value(groupId));
    }
    return normalizedMean(sum, count);
}

QStringList RadioSession::pendingArtists() const
{
    QStringList artists = m_recentArtists;
    for (const QString &path : m_pendingPaths) {
        const auto it = m_byPath.constFind(path);
        if (it != m_byPath.constEnd()) {
            pushRecentArtist(artists, it->artistFolded, kThrottleArtists);
        }
    }
    return artists;
}

bool RadioSession::anchorExcluded(const TrackScorer::Candidate &candidate) const
{
    if (!m_vectorAnchorSession) {
        return false;
    }
    return (!candidate.path.isEmpty() && m_anchorPaths.contains(candidate.path))
        || (!candidate.songKey.isEmpty() && m_anchorSongKeys.contains(candidate.songKey));
}

quint64 RadioSession::nextOwnedRandom()
{
    if (m_ownedRngState == 0) {
        m_ownedRngState = 1;
    }
    quint64 state = m_ownedRngState;
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    m_ownedRngState = state;
    return state * kOwnedRngMultiplier;
}

quint64 RadioSession::ownedBounded(quint64 bound)
{
    if (bound <= 1) {
        return 0;
    }
    const quint64 threshold = -bound % bound;
    quint64 value = 0;
    do {
        value = nextOwnedRandom();
    } while (value < threshold);
    return value % bound;
}

double RadioSession::drawRandomDouble()
{
    if (!m_usesOwnedRng) {
        return m_rng->generateDouble();
    }
    constexpr double kUnit = 1.0 / 9007199254740992.0;
    return static_cast<double>(nextOwnedRandom() >> 11) * kUnit;
}

const TrackScorer::Candidate *RadioSession::nextAnchor()
{
    if (m_anchors.isEmpty()) {
        return nullptr;
    }
    if (m_anchors.size() == 1) {
        return &m_anchors.first();
    }
    if (m_anchorRoundOrder.isEmpty() || m_anchorCursor >= m_anchorRoundOrder.size()) {
        m_anchorRoundOrder.clear();
        m_anchorRoundOrder.reserve(m_anchors.size());
        for (int i = 0; i < m_anchors.size(); ++i) {
            m_anchorRoundOrder.push_back(i);
        }
        std::sort(m_anchorRoundOrder.begin(), m_anchorRoundOrder.end(), [&](int left, int right) {
            const QString leftIdentity = anchorIdentity(m_anchors.at(left));
            const QString rightIdentity = anchorIdentity(m_anchors.at(right));
            return leftIdentity == rightIdentity ? left < right : leftIdentity < rightIdentity;
        });
        const int orderSize = static_cast<int>(m_anchorRoundOrder.size());
        for (int i = orderSize - 1; i > 0; --i) {
            const int swapIndex = static_cast<int>(ownedBounded(static_cast<quint64>(i + 1)));
            std::swap(m_anchorRoundOrder[i], m_anchorRoundOrder[swapIndex]);
        }
        m_anchorCursor = 0;
    }
    return &m_anchors.at(m_anchorRoundOrder.at(m_anchorCursor));
}

void RadioSession::consumeAnchor()
{
    if (m_anchors.size() > 1 && m_anchorCursor < m_anchorRoundOrder.size()) {
        ++m_anchorCursor;
    }
}

TrackScorer::SeedContext RadioSession::movingContext(const TrackScorer::Candidate *anchor) const
{
    QHash<QString, double> weightedGenres;
    double genreWeightSum = 0.0;
    double yearSum = 0.0;
    double yearWeightSum = 0.0;
    double tempoSum = 0.0;
    double tempoWeightSum = 0.0;
    double energySum = 0.0;
    double energyWeightSum = 0.0;
    QVector<double> audioSum;
    double audioWeightSum = 0.0;

    const auto addCandidate = [&](const TrackScorer::Candidate &candidate, double weight) {
        if (weight <= 0.0 || !std::isfinite(weight)) {
            return;
        }
        const QStringList genres = GenreTags::informative(candidate.genresFolded);
        if (!genres.isEmpty()) {
            const double genreWeight = weight / static_cast<double>(genres.size());
            for (const QString &genre : genres) {
                weightedGenres[genre] += genreWeight;
            }
            genreWeightSum += weight;
        }
        if (candidate.year > 0) {
            yearSum += weight * static_cast<double>(candidate.year);
            yearWeightSum += weight;
        }
        if (candidate.tempoBpm > 0.0 && std::isfinite(candidate.tempoBpm)) {
            tempoSum += weight * candidate.tempoBpm;
            tempoWeightSum += weight;
        }
        if (candidate.energy >= 0.0 && std::isfinite(candidate.energy)) {
            energySum += weight * candidate.energy;
            energyWeightSum += weight;
        }
        if (candidate.contentGroupId >= 0) {
            const auto it = m_embeddingsByGroup.constFind(candidate.contentGroupId);
            if (it != m_embeddingsByGroup.constEnd()) {
                addWeightedEmbedding(audioSum, audioWeightSum, it.value(), weight);
            }
        }
    };

    if (anchor != nullptr) {
        addCandidate(*anchor, std::exp2(-static_cast<double>(m_generatedPickCount)
                                        / kAnchorContextHalfLifePicks));
    }
    for (const ConfirmedContextEntry &entry : m_confirmedContext) {
        const auto it = m_byPath.constFind(entry.path);
        if (it != m_byPath.constEnd()) {
            addCandidate(it.value(), entry.weight);
        }
    }
    for (const QString &path : m_pendingPaths) {
        const auto it = m_byPath.constFind(path);
        if (it != m_byPath.constEnd()) {
            addCandidate(it.value(), kPendingContextWeight);
        }
    }

    if (genreWeightSum > 0.0 && std::isfinite(genreWeightSum)) {
        for (auto it = weightedGenres.begin(); it != weightedGenres.end(); ++it) {
            it.value() /= genreWeightSum;
        }
    }

    QStringList genres = weightedGenres.keys();
    genres.sort();

    TrackScorer::SeedContext context;
    context.genresFolded = genres;
    context.weightedGenres = std::move(weightedGenres);
    context.genreIdf = m_genreIdf;
    const QStringList artists = pendingArtists();
    context.recentArtistsFolded = QSet<QString>(artists.cbegin(), artists.cend());
    context.contextYear = yearWeightSum > 0.0 ? yearSum / yearWeightSum : 0.0;
    context.contextTempoBpm = tempoWeightSum > 0.0 ? tempoSum / tempoWeightSum : -1.0;
    context.contextEnergy = energyWeightSum > 0.0 ? energySum / energyWeightSum : -1.0;
    context.audioCentroid = normalizedWeightedMean(audioSum, audioWeightSum);
    context.embeddingsByGroup = &m_embeddingsByGroup;
    context.nowSecs = m_nowSecs;
    context.exploration0To100 = m_exploration;
    context.sessionTrackNumber = m_generatedPickCount + 1;
    context.sessionDecay = m_sessionDecay;
    return context;
}

TrackScorer::SeedContext RadioSession::permanentMultiContext(const TrackScorer::Candidate *anchor,
                                                             const QStringList &recentArtists) const
{
    const TrackScorer::Candidate &base = anchor != nullptr ? *anchor : m_seed;
    QStringList genres = base.genresFolded;
    QSet<QString> seen(genres.cbegin(), genres.cend());
    for (const QStringList &played : m_playedGenres) {
        for (const QString &genre : played) {
            if (!seen.contains(genre)) {
                seen.insert(genre);
                genres.push_back(genre);
            }
        }
    }

    double tempo = 0.0;
    int tempoCount = 0;
    double energy = 0.0;
    int energyCount = 0;
    for (const PlayedScalars &scalars : m_playedScalars) {
        if (scalars.tempoBpm > 0.0) {
            tempo += scalars.tempoBpm;
            ++tempoCount;
        }
        if (scalars.energy >= 0.0) {
            energy += scalars.energy;
            ++energyCount;
        }
    }

    QVector<double> audioSum;
    int audioCount = 0;
    addEmbedding(audioSum, audioCount, m_embeddingsByGroup.value(base.contentGroupId));
    for (qint64 groupId : m_playedContentGroups) {
        addEmbedding(audioSum, audioCount, m_embeddingsByGroup.value(groupId));
    }

    TrackScorer::SeedContext context;
    context.genresFolded = genres;
    context.genreIdf = m_genreIdf;
    context.recentArtistsFolded = QSet<QString>(recentArtists.cbegin(), recentArtists.cend());
    context.year = base.year;
    context.contextTempoBpm = tempoCount > 0 ? tempo / tempoCount : base.tempoBpm;
    context.contextEnergy = energyCount > 0 ? energy / energyCount : base.energy;
    context.audioCentroid = normalizedMean(audioSum, audioCount);
    context.embeddingsByGroup = &m_embeddingsByGroup;
    context.nowSecs = m_nowSecs;
    context.exploration0To100 = m_exploration;
    context.sessionTrackNumber = m_generatedPickCount + 1;
    context.sessionDecay = m_sessionDecay;
    return context;
}

void RadioSession::recordPick(const TrackScorer::Candidate &candidate, const TrackScorer::Scored &scored,
                              const QString &resolvedPath)
{
    m_usedPaths.insert(candidate.path);
    const QString reasonPath = resolvedPath.isEmpty() ? candidate.path : resolvedPath;
    if (!reasonPath.isEmpty()) {
        m_usedPaths.insert(reasonPath);
    }
    if (!candidate.songKey.isEmpty()) {
        m_usedSongKeys.insert(candidate.songKey);
    }
    m_albumCounts[candidate.albumKey] += 1;
    m_pickReasons.insert(candidate.path, scored.components);
    if (reasonPath != candidate.path) {
        m_pickReasons.insert(reasonPath, scored.components);
        // A substituted best copy is not necessarily a pool row, but it carries
        // the same content as the scored candidate. Alias the row so notePlayed
        // and exclusion lookups resolve the played path to the candidate's
        // genres, scalars, and content group instead of silently missing.
        if (!m_byPath.contains(reasonPath)) {
            TrackScorer::Candidate alias = candidate;
            alias.path = reasonPath;
            m_byPath.insert(reasonPath, alias);
        }
    }
    if (m_contextMode == ContextMode::MovingContext) {
        m_contextSourcePaths.insert(reasonPath,
                                    m_contextSourcePaths.value(candidate.path, candidate.path));
        m_pendingPaths.push_back(reasonPath);
    }
    m_pickReasonOrder.push_back(reasonPath);
    ++m_generatedPickCount;
}

QVector<Track> RadioSession::nextTracks(int count, const QSet<QString> &excludePaths,
                                        const std::function<Track(const QString &path)> &resolveTrack)
{
    QVector<Track> result;
    if (count <= 0) {
        return result;
    }

    if (m_contextMode == ContextMode::PermanentAnchor && m_anchors.size() <= 1) {
        // A batch-local recent-artist list so a multi-pick call throttles within
        // itself the same way successive single picks (fed back via notePlayed) do.
        QStringList batchArtists = m_recentArtists;

        for (int picked = 0; picked < count; ++picked) {
            TrackScorer::SeedContext context;
            context.genresFolded = rollingGenres();
            context.genreIdf = m_genreIdf;
            context.recentArtistsFolded = QSet<QString>(batchArtists.cbegin(), batchArtists.cend());
            context.year = m_seed.year;
            context.contextTempoBpm = rollingTempoBpm();
            context.contextEnergy = rollingEnergy();
            context.audioCentroid = rollingAudioCentroid();
            context.embeddingsByGroup = &m_embeddingsByGroup;
            context.nowSecs = m_nowSecs;
            context.exploration0To100 = m_exploration;
            context.sessionTrackNumber = m_generatedPickCount + 1;
            context.sessionDecay = m_sessionDecay;

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
                    || excludePaths.contains(candidate.path) || anchorExcluded(candidate)) {
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
            const Track resolved = resolveTrack(chosen.path);
            recordPick(chosen, scored.at(chosenIndex).first, resolved.path);
            pushRecentArtist(batchArtists, chosen.artistFolded, kThrottleArtists);

            if (!resolved.path.isEmpty()) {
                result.push_back(resolved);
            }
        }
        return result;
    }

    QStringList batchArtists = m_contextMode == ContextMode::MovingContext ? pendingArtists() : m_recentArtists;
    for (int picked = 0; picked < count; ++picked) {
        const TrackScorer::Candidate *assignedAnchor = nextAnchor();
        QSet<QString> failedPaths;
        QSet<QString> failedSongKeys;
        bool producedPick = false;

        while (true) {
            const QSet<QString> throttled(batchArtists.cbegin(), batchArtists.cend());
            QSet<QString> excludedSongKeys;
            for (const QString &path : excludePaths) {
                const auto it = m_byPath.constFind(path);
                if (it != m_byPath.constEnd() && !it->songKey.isEmpty()) {
                    excludedSongKeys.insert(it->songKey);
                }
            }

            TrackScorer::SeedContext context = m_contextMode == ContextMode::MovingContext
                ? movingContext(assignedAnchor)
                : permanentMultiContext(assignedAnchor, batchArtists);
            context.recentArtistsFolded = throttled;

            QList<std::pair<TrackScorer::Scored, const TrackScorer::Candidate *>> scored;
            scored.reserve(m_pool.size());
            for (const TrackScorer::Candidate &candidate : m_pool) {
                if (candidate.path.isEmpty() || m_usedPaths.contains(candidate.path)
                    || excludePaths.contains(candidate.path) || anchorExcluded(candidate)
                    || failedPaths.contains(candidate.path)) {
                    continue;
                }
                if (!candidate.songKey.isEmpty()
                    && (m_usedSongKeys.contains(candidate.songKey)
                        || excludedSongKeys.contains(candidate.songKey)
                        || failedSongKeys.contains(candidate.songKey))) {
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
            double minScore = scored.front().first.score;
            for (int i = 0; i < topN; ++i) {
                minScore = std::min(minScore, scored.at(i).first.score);
            }
            double total = 0.0;
            for (int i = 0; i < topN; ++i) {
                total += (scored.at(i).first.score - minScore) + 0.001;
            }
            double roll = drawRandomDouble() * total;
            int chosenIndex = topN - 1;
            for (int i = 0; i < topN; ++i) {
                roll -= (scored.at(i).first.score - minScore) + 0.001;
                if (roll <= 0.0) {
                    chosenIndex = i;
                    break;
                }
            }

            const TrackScorer::Candidate &chosen = *scored.at(chosenIndex).second;
            const Track resolved = resolveTrack(chosen.path);
            if (resolved.path.isEmpty()) {
                failedPaths.insert(chosen.path);
                if (!chosen.songKey.isEmpty()) {
                    failedSongKeys.insert(chosen.songKey);
                }
                continue;
            }

            recordPick(chosen, scored.at(chosenIndex).first, resolved.path);
            pushRecentArtist(batchArtists, chosen.artistFolded, kThrottleArtists);
            result.push_back(resolved);
            consumeAnchor();
            producedPick = true;
            break;
        }

        if (!producedPick) {
            break;
        }
    }
    return result;
}

void RadioSession::aliasResolvedPath(const QString &candidatePath, const QString &resolvedPath)
{
    if (candidatePath.isEmpty() || resolvedPath.isEmpty() || candidatePath == resolvedPath) {
        return;
    }
    const auto candidate = m_byPath.constFind(candidatePath);
    if (candidate != m_byPath.constEnd() && !m_byPath.contains(resolvedPath)) {
        TrackScorer::Candidate alias = *candidate;
        alias.path = resolvedPath;
        m_byPath.insert(resolvedPath, alias);
    }
    m_usedPaths.insert(resolvedPath);
    if (m_contextMode == ContextMode::MovingContext) {
        m_contextSourcePaths.insert(resolvedPath,
                                    m_contextSourcePaths.value(candidatePath, candidatePath));
    }
    const auto reason = m_pickReasons.constFind(candidatePath);
    if (reason != m_pickReasons.constEnd()) {
        m_pickReasons.insert(resolvedPath, *reason);
    }
    for (QString &path : m_pickReasonOrder) {
        if (path == candidatePath) {
            path = resolvedPath;
        }
    }
    for (QString &path : m_pendingPaths) {
        if (path == candidatePath) {
            path = resolvedPath;
        }
    }
}

void RadioSession::retainPendingPaths(const QStringList &orderedPaths)
{
    if (m_contextMode != ContextMode::MovingContext) {
        return;
    }
    QStringList retained;
    QSet<QString> seen;
    for (const QString &path : orderedPaths) {
        if (!seen.contains(path) && m_pendingPaths.contains(path) && m_byPath.contains(path)) {
            seen.insert(path);
            retained.push_back(path);
        }
    }
    m_pendingPaths = std::move(retained);
}

void RadioSession::setExploration(int exploration0To100)
{
    m_exploration = std::clamp(exploration0To100, 0, 100);
}

void RadioSession::setWeights(TrackScorer::Weights weights)
{
    m_weights = std::move(weights);
}

void RadioSession::setSessionDecay(TrackScorer::RadioSessionDecay decay)
{
    m_sessionDecay = std::move(decay);
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
    if (m_contextMode == ContextMode::MovingContext) {
        m_pendingPaths.removeAll(track.path);
    }
    pushRecentArtist(m_recentArtists, FoldKey::fold(track.artistName), kThrottleArtists);

    QStringList genres;
    PlayedScalars scalars;
    qint64 contentGroupId = -1;
    QString albumKey = FoldKey::albumKey(track.albumArtistName, track.albumTitle);
    QString songKey = FoldKey::songKey(track.musicBrainz.recordingId, track.artistName, track.title);
    auto it = m_byPath.constFind(track.path);
    if (m_contextMode == ContextMode::MovingContext && it == m_byPath.constEnd()) {
        const TrackScorer::Candidate candidate = candidateFromTrack(track);
        m_byPath.insert(candidate.path, candidate);
        m_contextSourcePaths.insert(candidate.path, candidate.path);
        it = m_byPath.constFind(track.path);
    }
    if (it != m_byPath.constEnd()) {
        // Filter here too: pool candidates already carry canonical genre keys,
        // and this is the other chokepoint genres enter the rolling context
        // through.
        genres = GenreTags::informative(it->genresFolded);
        scalars.tempoBpm = it->tempoBpm;
        scalars.energy = it->energy;
        contentGroupId = it->contentGroupId;
        albumKey = it->albumKey;
        songKey = it->songKey;
    }
    if (m_contextMode == ContextMode::MovingContext) {
        const double decay = std::exp2(-1.0 / kConfirmedContextDecayPicks);
        for (auto entry = m_confirmedContext.begin(); entry != m_confirmedContext.end();) {
            entry->weight *= decay;
            if (entry->weight < kConfirmedContextMinimumWeight) {
                entry = m_confirmedContext.erase(entry);
            } else {
                ++entry;
            }
        }
        if (it != m_byPath.constEnd()) {
            m_confirmedContext.push_back({track.path, 1.0});
            m_contextSourcePaths.insert(track.path,
                                        m_contextSourcePaths.value(track.path, track.path));
        }
    }
    m_playedGenres.push_back(genres);
    while (m_playedGenres.size() > kThrottleArtists) {
        m_playedGenres.removeFirst();
    }
    m_playedScalars.push_back(scalars);
    while (m_playedScalars.size() > kThrottleArtists) {
        m_playedScalars.removeFirst();
    }
    m_playedContentGroups.push_back(contentGroupId);
    while (m_playedContentGroups.size() > kThrottleArtists) {
        m_playedContentGroups.removeFirst();
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

    QJsonArray playedScalars;
    for (const PlayedScalars &scalars : m_playedScalars) {
        QJsonObject object;
        if (scalars.tempoBpm > 0.0) {
            object.insert(QStringLiteral("tempoBpm"), scalars.tempoBpm);
        }
        if (scalars.energy >= 0.0) {
            object.insert(QStringLiteral("energy"), scalars.energy);
        }
        playedScalars.append(object);
    }

    QJsonObject state{
        {QStringLiteral("usedSongKeys"), stringSetToJson(m_usedSongKeys)},
        {QStringLiteral("usedPaths"), stringSetToJson(m_usedPaths)},
        {QStringLiteral("albumGroupCounts"), albumCounts},
        {QStringLiteral("recentArtists"), stringListToJson(m_recentArtists)},
        {QStringLiteral("playedGenres"), playedGenres},
        {QStringLiteral("playedScalars"), playedScalars},
        {QStringLiteral("playedContentGroups"), groupListToJson(m_playedContentGroups)},
    };
    if (m_contextMode == ContextMode::MovingContext) {
        QJsonArray confirmedContext;
        QSet<QString> contextPaths;
        for (const ConfirmedContextEntry &entry : m_confirmedContext) {
            confirmedContext.append(QJsonObject{
                {QStringLiteral("path"), entry.path},
                {QStringLiteral("weight"), entry.weight},
            });
            contextPaths.insert(entry.path);
        }
        for (const QString &path : m_pendingPaths) {
            contextPaths.insert(path);
        }

        QStringList sortedContextPaths = contextPaths.values();
        sortedContextPaths.sort();
        QJsonArray contextCandidates;
        for (const QString &path : sortedContextPaths) {
            const auto candidate = m_byPath.constFind(path);
            if (candidate != m_byPath.constEnd()) {
                contextCandidates.append(candidateToJson(
                    candidate.value(), m_contextSourcePaths.value(path, path)));
            }
        }

        state.insert(QStringLiteral("generatedPickCount"), m_generatedPickCount);
        state.insert(QStringLiteral("confirmedContext"), confirmedContext);
        state.insert(QStringLiteral("pendingPaths"), stringListToJson(m_pendingPaths));
        state.insert(QStringLiteral("contextCandidates"), contextCandidates);
    }
    if (m_contextMode == ContextMode::MovingContext || m_anchors.size() > 1) {
        QJsonArray anchorRoundOrder;
        for (const int index : m_anchorRoundOrder) {
            anchorRoundOrder.append(anchorIdentity(m_anchors.at(index)));
        }
        state.insert(QStringLiteral("anchorRoundOrder"), anchorRoundOrder);
        state.insert(QStringLiteral("anchorCursor"), m_anchorCursor);
    }
    if (m_anchors.size() > 1) {
        state.insert(QStringLiteral("generatedPickCount"), m_generatedPickCount);
    }
    if (m_usesOwnedRng) {
        state.insert(QStringLiteral("rngState"), QString::number(m_ownedRngState, 16));
    }
    return state;
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
    if (state.contains(QStringLiteral("playedScalars"))) {
        m_playedScalars.clear();
        const QJsonArray rows = state.value(QStringLiteral("playedScalars")).toArray();
        for (const QJsonValue &row : rows) {
            const QJsonObject object = row.toObject();
            PlayedScalars scalars;
            scalars.tempoBpm = object.value(QStringLiteral("tempoBpm")).toDouble(-1.0);
            scalars.energy = object.value(QStringLiteral("energy")).toDouble(-1.0);
            m_playedScalars.push_back(scalars);
        }
        while (m_playedScalars.size() > kThrottleArtists) {
            m_playedScalars.removeFirst();
        }
    }
    if (state.contains(QStringLiteral("playedContentGroups"))) {
        m_playedContentGroups = groupListFromJson(state.value(QStringLiteral("playedContentGroups")));
        while (m_playedContentGroups.size() > kThrottleArtists) {
            m_playedContentGroups.removeFirst();
        }
    }
    if (m_usesOwnedRng && state.contains(QStringLiteral("rngState"))) {
        const QString text = state.value(QStringLiteral("rngState")).toString();
        bool ok = false;
        quint64 rngState = text.toULongLong(&ok, 16);
        if (!ok) {
            rngState = text.toULongLong(&ok, 10);
        }
        if (ok && rngState != 0) {
            m_ownedRngState = rngState;
        }
    }

    if (m_contextMode == ContextMode::MovingContext || m_anchors.size() > 1) {
        if (state.contains(QStringLiteral("generatedPickCount"))) {
            const int generatedPickCount = state.value(QStringLiteral("generatedPickCount")).toInt(-1);
            if (generatedPickCount >= 0) {
                m_generatedPickCount = generatedPickCount;
            }
        }
    }

    if (m_contextMode == ContextMode::MovingContext) {
        QVector<ConfirmedContextEntry> restoredConfirmed;
        QSet<QString> confirmedReferencePaths;
        QSet<QString> pendingReferencePaths;
        if (state.contains(QStringLiteral("confirmedContext"))) {
            const QJsonValue value = state.value(QStringLiteral("confirmedContext"));
            if (value.isArray()) {
                for (const QJsonValue &row : value.toArray()) {
                    if (!row.isObject()) {
                        continue;
                    }
                    const QJsonObject object = row.toObject();
                    const QJsonValue pathValue = object.value(QStringLiteral("path"));
                    const QJsonValue weightValue = object.value(QStringLiteral("weight"));
                    if (!pathValue.isString() || pathValue.toString().isEmpty() || !weightValue.isDouble()) {
                        continue;
                    }
                    const double weight = weightValue.toDouble();
                    if (!std::isfinite(weight) || weight < kConfirmedContextMinimumWeight || weight > 1.0) {
                        continue;
                    }
                    restoredConfirmed.push_back({pathValue.toString(), weight});
                    confirmedReferencePaths.insert(pathValue.toString());
                }
            }
        }

        QStringList restoredPending;
        if (state.contains(QStringLiteral("pendingPaths"))) {
            const QJsonValue value = state.value(QStringLiteral("pendingPaths"));
            if (value.isArray()) {
                QSet<QString> seen;
                for (const QJsonValue &item : value.toArray()) {
                    if (!item.isString() || item.toString().isEmpty() || seen.contains(item.toString())) {
                        continue;
                    }
                    seen.insert(item.toString());
                    restoredPending.push_back(item.toString());
                    pendingReferencePaths.insert(item.toString());
                }
            }
        }
        QSet<QString> referencedPaths = confirmedReferencePaths;
        referencedPaths.unite(pendingReferencePaths);

        struct ContextSnapshot {
            TrackScorer::Candidate candidate;
            QString sourcePath;
        };
        QVector<ContextSnapshot> snapshots;
        QSet<QString> duplicateSnapshotPaths;
        QHash<QString, int> snapshotCounts;
        if (state.contains(QStringLiteral("contextCandidates"))) {
            m_contextSourcePaths.clear();
            const QJsonValue value = state.value(QStringLiteral("contextCandidates"));
            if (value.isArray()) {
                for (const QJsonValue &row : value.toArray()) {
                    if (!row.isObject()) {
                        continue;
                    }
                    const QJsonObject object = row.toObject();
                    const QJsonValue pathValue = object.value(QStringLiteral("path"));
                    if (!pathValue.isString() || !referencedPaths.contains(pathValue.toString())) {
                        continue;
                    }
                    const QString path = pathValue.toString();
                    if (++snapshotCounts[path] > 1) {
                        duplicateSnapshotPaths.insert(path);
                    }
                    ContextSnapshot snapshot;
                    if (candidateFromJson(object, &snapshot.candidate, &snapshot.sourcePath)) {
                        snapshots.push_back(std::move(snapshot));
                    }
                }
            }
        }

        QSet<QString> rejectedContextPaths;
        for (const ContextSnapshot &snapshot : snapshots) {
            if (duplicateSnapshotPaths.contains(snapshot.candidate.path)
                || snapshot.sourcePath != snapshot.candidate.path
                || !confirmedReferencePaths.contains(snapshot.candidate.path)) {
                continue;
            }
            if (!m_byPath.contains(snapshot.candidate.path)) {
                m_byPath.insert(snapshot.candidate.path, snapshot.candidate);
            }
            if (m_byPath.contains(snapshot.candidate.path)) {
                m_contextSourcePaths.insert(snapshot.candidate.path, snapshot.candidate.path);
            }
        }
        for (const ContextSnapshot &snapshot : snapshots) {
            if (duplicateSnapshotPaths.contains(snapshot.candidate.path)
                || snapshot.sourcePath == snapshot.candidate.path) {
                continue;
            }
            const auto source = m_byPath.constFind(snapshot.sourcePath);
            if (source == m_byPath.constEnd()
                || !candidateIdentityCompatible(source.value(), snapshot.candidate)) {
                continue;
            }
            const auto destination = m_byPath.constFind(snapshot.candidate.path);
            if (destination != m_byPath.constEnd()) {
                if (!candidateIdentityCompatible(source.value(), destination.value())) {
                    rejectedContextPaths.insert(snapshot.candidate.path);
                    continue;
                }
            } else {
                TrackScorer::Candidate alias = source.value();
                alias.path = snapshot.candidate.path;
                m_byPath.insert(alias.path, std::move(alias));
            }
            m_contextSourcePaths.insert(snapshot.candidate.path, snapshot.sourcePath);
        }

        QVector<ConfirmedContextEntry> validConfirmed;
        validConfirmed.reserve(restoredConfirmed.size());
        for (const ConfirmedContextEntry &entry : restoredConfirmed) {
            if (!rejectedContextPaths.contains(entry.path) && m_byPath.contains(entry.path)) {
                validConfirmed.push_back(entry);
            }
        }
        if (validConfirmed.size() > kConfirmedContextMaximumEntries) {
            validConfirmed.remove(0, validConfirmed.size() - kConfirmedContextMaximumEntries);
        }
        m_confirmedContext = std::move(validConfirmed);
        m_pendingPaths.clear();
        for (const QString &path : restoredPending) {
            if (!rejectedContextPaths.contains(path) && m_byPath.contains(path)) {
                m_pendingPaths.push_back(path);
            }
        }
    }

    if (m_anchors.size() > 1
        && (state.contains(QStringLiteral("anchorRoundOrder"))
            || state.contains(QStringLiteral("anchorCursor")))) {
        QHash<QString, int> indicesByIdentity;
        bool valid = true;
        for (int index = 0; index < m_anchors.size(); ++index) {
            const QString identity = anchorIdentity(m_anchors.at(index));
            if (identity.isEmpty() || indicesByIdentity.contains(identity)) {
                valid = false;
                break;
            }
            indicesByIdentity.insert(identity, index);
        }

        QVector<int> order;
        const QJsonArray values = state.value(QStringLiteral("anchorRoundOrder")).toArray();
        QSet<QString> seenIdentities;
        for (const QJsonValue &value : values) {
            const QString identity = value.toString();
            if (!value.isString() || identity.isEmpty() || seenIdentities.contains(identity)
                || !indicesByIdentity.contains(identity)) {
                valid = false;
                break;
            }
            seenIdentities.insert(identity);
            order.push_back(indicesByIdentity.value(identity));
        }
        if (valid && order.size() == m_anchors.size()) {
            const int cursor = state.value(QStringLiteral("anchorCursor")).toInt(-1);
            if (cursor >= 0 && cursor <= order.size()) {
                m_anchorRoundOrder = std::move(order);
                m_anchorCursor = cursor;
            } else {
                valid = false;
            }
        } else {
            valid = false;
        }
        if (!valid) {
            m_anchorRoundOrder.clear();
            m_anchorCursor = 0;
        }
    }
    m_pickReasons.clear();
    m_pickReasonOrder.clear();
}
