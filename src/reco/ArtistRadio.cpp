#include "reco/ArtistRadio.h"

#include "core/FoldKey.h"
#include "core/GenreTags.h"

#include <algorithm>

namespace {

int trackYear(const Track &track)
{
    const QString date = !track.originalDate.isEmpty() ? track.originalDate : track.date;
    if (date.size() < 4) {
        return 0;
    }
    bool ok = false;
    const int year = QStringView(date).left(4).toInt(&ok);
    return ok ? year : 0;
}

} // namespace

namespace ArtistRadio {

QStringList aggregateSeedGenres(const QVector<QPair<QString, int>> &genreCounts,
                                const QHash<QString, QString> &aliases,
                                const QSet<QString> &ignored,
                                int cap)
{
    QHash<QString, int> canonicalCounts;
    for (const auto &[genre, count] : genreCounts) {
        const QString canonical = GenreTags::canonical(genre, aliases);
        if (canonical.isEmpty() || GenreTags::isNonGenre(canonical) || ignored.contains(canonical) || count <= 0) {
            continue;
        }
        canonicalCounts[canonical] += count;
    }

    QVector<QPair<QString, int>> ordered;
    ordered.reserve(canonicalCounts.size());
    for (auto it = canonicalCounts.cbegin(); it != canonicalCounts.cend(); ++it) {
        ordered.push_back({it.key(), it.value()});
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto &left, const auto &right) {
        if (left.second != right.second) {
            return left.second > right.second;
        }
        return left.first < right.first;
    });

    QStringList genres;
    const int limit = std::min(std::max(0, cap), static_cast<int>(ordered.size()));
    genres.reserve(limit);
    for (int i = 0; i < limit; ++i) {
        genres.push_back(ordered.at(i).first);
    }
    return genres;
}

int medianTrackYear(const QVector<Track> &tracks)
{
    QVector<int> years;
    years.reserve(tracks.size());
    for (const Track &track : tracks) {
        const int year = trackYear(track);
        if (year > 0) {
            years.push_back(year);
        }
    }
    if (years.isEmpty()) {
        return 0;
    }
    std::sort(years.begin(), years.end());
    const int mid = static_cast<int>(years.size() / 2);
    if (years.size() % 2 == 1) {
        return years.at(mid);
    }
    return (years.at(mid - 1) + years.at(mid)) / 2;
}

Track representativeTrack(const QVector<Track> &tracks,
                          const QHash<QString, TrackScorer::Affinity> &affinities)
{
    if (tracks.isEmpty()) {
        return {};
    }

    const auto affinityScore = [&affinities](const Track &track) {
        const TrackScorer::Affinity affinity = affinities.value(track.path);
        return affinity.finished + affinity.listenCount + affinity.playEvents - affinity.skipped;
    };
    const auto lessPreferred = [&affinityScore](const Track &left, const Track &right) {
        if (left.effectiveRating0To100 != right.effectiveRating0To100) {
            return left.effectiveRating0To100 < right.effectiveRating0To100;
        }
        const int leftAffinity = affinityScore(left);
        const int rightAffinity = affinityScore(right);
        if (leftAffinity != rightAffinity) {
            return leftAffinity < rightAffinity;
        }
        return left.path > right.path;
    };
    return *std::max_element(tracks.cbegin(), tracks.cend(), lessPreferred);
}

TrackScorer::Candidate syntheticSeedCandidate(const QString &artistName,
                                              const QStringList &genresFolded,
                                              int year)
{
    TrackScorer::Candidate seed;
    seed.artistFolded = FoldKey::fold(artistName);
    seed.genresFolded = genresFolded;
    seed.year = year;
    return seed;
}

} // namespace ArtistRadio
