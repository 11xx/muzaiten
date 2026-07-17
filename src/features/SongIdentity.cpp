#include "features/SongIdentity.h"

#include "core/FoldKey.h"

#include <QSet>
#include <QVector>

#include <algorithm>
#include <limits>

namespace {

class UnionFind {
public:
    explicit UnionFind(qsizetype size)
        : m_parent(size)
        , m_rank(size, 0)
    {
        for (qsizetype i = 0; i < size; ++i) {
            m_parent[i] = static_cast<int>(i);
        }
    }

    int find(int value)
    {
        if (m_parent[value] != value) {
            m_parent[value] = find(m_parent[value]);
        }
        return m_parent[value];
    }

    void unite(int left, int right)
    {
        int leftRoot = find(left);
        int rightRoot = find(right);
        if (leftRoot == rightRoot) {
            return;
        }
        if (m_rank[leftRoot] < m_rank[rightRoot]) {
            std::swap(leftRoot, rightRoot);
        }
        m_parent[rightRoot] = leftRoot;
        if (m_rank[leftRoot] == m_rank[rightRoot]) {
            ++m_rank[leftRoot];
        }
    }

private:
    QVector<int> m_parent;
    QVector<int> m_rank;
};

QString contentGroupKey(qint64 groupId)
{
    return QStringLiteral("cg:") + QString::number(groupId);
}

} // namespace

namespace SongIdentity {

QHash<QString, QString> resolvedSongKeys(const QList<TrackIdentity> &tracks)
{
    QHash<QString, QString> result;
    if (tracks.isEmpty()) {
        return result;
    }

    UnionFind unions(tracks.size());
    QHash<qint64, int> firstByGroup;
    QHash<QString, int> firstByMbid;
    QHash<QString, int> firstByFallback;

    for (qsizetype i = 0; i < tracks.size(); ++i) {
        const TrackIdentity &track = tracks.at(i);
        const int index = static_cast<int>(i);
        if (track.contentGroupId >= 0) {
            const auto it = firstByGroup.constFind(track.contentGroupId);
            if (it == firstByGroup.constEnd()) {
                firstByGroup.insert(track.contentGroupId, index);
            } else {
                unions.unite(*it, index);
            }
        }
        if (!track.mbRecordingId.isEmpty()) {
            const auto it = firstByMbid.constFind(track.mbRecordingId);
            if (it == firstByMbid.constEnd()) {
                firstByMbid.insert(track.mbRecordingId, index);
            } else {
                unions.unite(*it, index);
            }
        } else {
            const QString fallback = FoldKey::songKey({}, track.artist, track.title);
            const auto it = firstByFallback.constFind(fallback);
            if (it == firstByFallback.constEnd()) {
                firstByFallback.insert(fallback, index);
            } else {
                unions.unite(*it, index);
            }
        }
    }

    struct Representative {
        qint64 groupId = std::numeric_limits<qint64>::max();
        QString mbRecordingId;
        QString fallbackKey;
    };
    QHash<int, Representative> representatives;
    for (qsizetype i = 0; i < tracks.size(); ++i) {
        const TrackIdentity &track = tracks.at(i);
        Representative &representative = representatives[unions.find(static_cast<int>(i))];
        if (track.contentGroupId >= 0) {
            representative.groupId = std::min(representative.groupId, track.contentGroupId);
        }
        if (!track.mbRecordingId.isEmpty()
            && (representative.mbRecordingId.isEmpty() || track.mbRecordingId < representative.mbRecordingId)) {
            representative.mbRecordingId = track.mbRecordingId;
        }
        if (track.mbRecordingId.isEmpty()) {
            const QString fallback = FoldKey::songKey({}, track.artist, track.title);
            if (representative.fallbackKey.isEmpty() || fallback < representative.fallbackKey) {
                representative.fallbackKey = fallback;
            }
        }
    }

    result.reserve(tracks.size());
    for (qsizetype i = 0; i < tracks.size(); ++i) {
        const TrackIdentity &track = tracks.at(i);
        if (track.path.isEmpty()) {
            continue;
        }
        const Representative representative = representatives.value(unions.find(static_cast<int>(i)));
        if (representative.groupId != std::numeric_limits<qint64>::max()) {
            result.insert(track.path, contentGroupKey(representative.groupId));
        } else if (!representative.mbRecordingId.isEmpty()) {
            result.insert(track.path, QStringLiteral("mbid:") + representative.mbRecordingId);
        } else {
            result.insert(track.path, representative.fallbackKey);
        }
    }
    return result;
}

QStringList pathsConnectedToTrack(const QList<TrackIdentity> &tracks, const QString &targetPath)
{
    if (tracks.isEmpty() || targetPath.isEmpty()) {
        return {};
    }

    QHash<QString, int> indexByPath;
    QHash<qint64, QList<int>> indicesByGroup;
    QHash<QString, QList<int>> indicesByMbid;
    QHash<QString, QList<int>> indicesByFallback;
    indexByPath.reserve(tracks.size());
    for (qsizetype i = 0; i < tracks.size(); ++i) {
        const TrackIdentity &track = tracks.at(i);
        const int index = static_cast<int>(i);
        if (!track.path.isEmpty()) {
            indexByPath.insert(track.path, index);
        }
        if (track.contentGroupId >= 0) {
            indicesByGroup[track.contentGroupId].push_back(index);
        }
        if (!track.mbRecordingId.isEmpty()) {
            indicesByMbid[track.mbRecordingId].push_back(index);
        } else {
            indicesByFallback[FoldKey::songKey({}, track.artist, track.title)].push_back(index);
        }
    }

    const auto target = indexByPath.constFind(targetPath);
    if (target == indexByPath.constEnd()) {
        return {};
    }

    QVector<int> pending{*target};
    QSet<int> visited{*target};
    QSet<qint64> expandedGroups;
    QSet<QString> expandedMbids;
    QSet<QString> expandedFallbacks;
    const auto enqueue = [&pending, &visited](const QList<int> &indices) {
        for (const int index : indices) {
            if (!visited.contains(index)) {
                visited.insert(index);
                pending.push_back(index);
            }
        }
    };

    for (qsizetype cursor = 0; cursor < pending.size(); ++cursor) {
        const TrackIdentity &track = tracks.at(pending.at(cursor));
        if (track.contentGroupId >= 0 && !expandedGroups.contains(track.contentGroupId)) {
            expandedGroups.insert(track.contentGroupId);
            enqueue(indicesByGroup.value(track.contentGroupId));
        }
        if (!track.mbRecordingId.isEmpty()) {
            if (!expandedMbids.contains(track.mbRecordingId)) {
                expandedMbids.insert(track.mbRecordingId);
                enqueue(indicesByMbid.value(track.mbRecordingId));
            }
        } else {
            const QString fallback = FoldKey::songKey({}, track.artist, track.title);
            if (!expandedFallbacks.contains(fallback)) {
                expandedFallbacks.insert(fallback);
                enqueue(indicesByFallback.value(fallback));
            }
        }
    }

    QStringList paths;
    paths.reserve(visited.size());
    for (const int index : visited) {
        const QString &path = tracks.at(index).path;
        if (!path.isEmpty()) {
            paths.push_back(path);
        }
    }
    paths.removeDuplicates();
    paths.sort(Qt::CaseInsensitive);
    return paths;
}

} // namespace SongIdentity
