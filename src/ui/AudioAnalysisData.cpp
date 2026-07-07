#include "ui/AudioAnalysisData.h"

#include "core/MetadataBlob.h"
#include "core/Track.h"
#include "db/Database.h"
#include "features/QualityRank.h"

#include <QFileInfo>
#include <QHash>

#include <algorithm>

namespace {

QStringList mediaTagsForPath(const Database &db, const QString &path)
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

QualityRank::Copy qualityCopyForTrack(const Track &track, const QStringList &mediaTags)
{
    return QualityRank::Copy{
        track.path,
        track.codec,
        track.bitDepth,
        track.sampleRateHz,
        track.bitrateKbps,
        mediaTags,
    };
}

QString qualitySummary(const Track &track, const QStringList &mediaTags, int qualityScore)
{
    QStringList parts;
    if (!track.codec.isEmpty()) {
        parts << track.codec;
    }
    if (track.bitDepth > 0 || track.sampleRateHz > 0) {
        QString rateDepth;
        if (track.bitDepth > 0) {
            rateDepth += QStringLiteral("%1bit").arg(track.bitDepth);
        }
        if (track.sampleRateHz > 0) {
            if (!rateDepth.isEmpty()) {
                rateDepth += QLatin1Char('/');
            }
            rateDepth += QStringLiteral("%1Hz").arg(track.sampleRateHz);
        }
        parts << rateDepth;
    }
    if (track.bitrateKbps > 0) {
        parts << QStringLiteral("%1kbps").arg(track.bitrateKbps);
    }
    if (!mediaTags.isEmpty()) {
        parts << QStringLiteral("MEDIA=%1").arg(mediaTags.join(QLatin1Char('|')));
    }
    parts << QStringLiteral("score=%1").arg(qualityScore);
    return parts.join(QLatin1Char(' '));
}

} // namespace

namespace AudioAnalysisData {

StatusSummary loadStatus(const QString &featuresPath)
{
    StatusSummary summary;
    summary.path = featuresPath;
    summary.found = QFileInfo::exists(featuresPath);
    if (!summary.found) {
        summary.message = QStringLiteral("features.sqlite not found");
        return summary;
    }

    FeatureStore store(featuresPath);
    summary.open = store.isOpen();
    if (!summary.open) {
        summary.message = QStringLiteral("features.sqlite unsupported or unreadable");
        return summary;
    }

    summary.schemaVersion = store.schemaVersion();
    summary.status = store.status();
    return summary;
}

QVector<DuplicateGroup> loadDuplicateGroups(Database &db, const FeatureStore &features, int minSize, int limit)
{
    QVector<DuplicateGroup> groups;
    if (!features.isOpen() || minSize < 1 || limit <= 0) {
        return groups;
    }

    const QHash<qint64, QString> pins = db.contentGroupPins();
    const QVector<qint64> groupIds = features.contentGroupIds(minSize);
    groups.reserve(std::min(groupIds.size(), static_cast<qsizetype>(limit)));
    for (qint64 groupId : groupIds) {
        DuplicateGroup group;
        group.groupId = groupId;
        group.pinnedPath = pins.value(groupId);

        QVector<QualityRank::Copy> qualityCopies;
        const QStringList paths = features.pathsInGroup(groupId);
        group.copies.reserve(paths.size());
        qualityCopies.reserve(paths.size());
        for (const QString &path : paths) {
            Track track = db.trackForPath(path);
            if (track.path.isEmpty()) {
                continue;
            }
            db.enrichTrackForStatus(track);
            const QStringList mediaTags = mediaTagsForPath(db, path);
            const QualityRank::Copy copy = qualityCopyForTrack(track, mediaTags);
            const int score = QualityRank::score(copy);
            qualityCopies.push_back(copy);
            group.copies.push_back({
                groupId,
                track.path,
                track.title,
                track.artistName,
                track.albumTitle,
                track.codec,
                track.bitDepth,
                track.sampleRateHz,
                track.bitrateKbps,
                mediaTags,
                score,
                path == group.pinnedPath,
                false,
                qualitySummary(track, mediaTags, score),
            });
        }
        if (group.copies.size() < static_cast<qsizetype>(minSize)) {
            continue;
        }

        group.bestPath = QualityRank::bestPath(qualityCopies, group.pinnedPath);
        for (DuplicateCopy &copy : group.copies) {
            copy.best = copy.path == group.bestPath;
        }
        std::sort(group.copies.begin(), group.copies.end(), [](const DuplicateCopy &left, const DuplicateCopy &right) {
            if (left.best != right.best) {
                return left.best;
            }
            if (left.qualityScore != right.qualityScore) {
                return left.qualityScore > right.qualityScore;
            }
            return left.path < right.path;
        });

        groups.push_back(std::move(group));
        if (groups.size() >= static_cast<qsizetype>(limit)) {
            break;
        }
    }
    return groups;
}

QString copyDisplayTitle(const DuplicateCopy &copy)
{
    if (!copy.title.trimmed().isEmpty()) {
        return copy.title;
    }
    const QFileInfo info(copy.path);
    return info.fileName().isEmpty() ? copy.path : info.fileName();
}

} // namespace AudioAnalysisData
