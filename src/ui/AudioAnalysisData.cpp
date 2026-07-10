#include "ui/AudioAnalysisData.h"

#include "core/MetadataBlob.h"
#include "core/Track.h"
#include "db/Database.h"
#include "features/QualityRank.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QLocale>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>

#include <algorithm>
#include <cmath>

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

QHash<QString, QString> loadMetaRows(const QString &featuresPath)
{
    QHash<QString, QString> values;
    const QString connectionName =
        QStringLiteral("audio-analysis-meta-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        db.setDatabaseName(featuresPath);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(QStringLiteral("SELECT key, value FROM meta"))) {
                while (query.next()) {
                    values.insert(query.value(0).toString(), query.value(1).toString());
                }
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connectionName);
    return values;
}

int intMeta(const QHash<QString, QString> &meta, const QString &key)
{
    bool ok = false;
    const int value = meta.value(key).toInt(&ok);
    return ok ? value : 0;
}

double doubleMeta(const QHash<QString, QString> &meta, const QString &key)
{
    bool ok = false;
    const double value = meta.value(key).toDouble(&ok);
    return ok ? value : 0.0;
}

} // namespace

namespace AudioAnalysisData {

QString compactDuration(qint64 seconds)
{
    seconds = std::max<qint64>(0, seconds);
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 secs = seconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1h%2m").arg(hours).arg(minutes, 2, 10, QLatin1Char('0'));
    }
    if (minutes > 0) {
        return QStringLiteral("%1m%2s").arg(minutes).arg(secs, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1s").arg(secs);
}

QString clockDuration(qint64 seconds)
{
    seconds = std::max<qint64>(0, seconds);
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 secs = seconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(secs, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'));
}

QString spacedDuration(qint64 seconds)
{
    seconds = std::max<qint64>(0, seconds);
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 secs = seconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1h %2m %3s").arg(hours).arg(minutes).arg(secs);
    }
    if (minutes > 0) {
        return QStringLiteral("%1m %2s").arg(minutes).arg(secs);
    }
    return QStringLiteral("%1s").arg(secs);
}

QString phaseLabel(LiveStatus::Phase phase)
{
    switch (phase) {
    case LiveStatus::Phase::AnalyzingFiles:
        return QStringLiteral("Analyzing files");
    case LiveStatus::Phase::Grouping:
        return QStringLiteral("Grouping");
    case LiveStatus::Phase::WritingFeatures:
        return QStringLiteral("Writing features");
    case LiveStatus::Phase::SemanticEmbeddings:
        return QStringLiteral("Generating semantic embeddings");
    case LiveStatus::Phase::SemanticNeighbors:
        return QStringLiteral("Building audio-similarity neighbors");
    case LiveStatus::Phase::ModelDownload:
        return QStringLiteral("Downloading semantic model");
    case LiveStatus::Phase::Idle:
        break;
    }
    return QStringLiteral("Idle");
}

QString progressLabel(const LiveStatus &status)
{
    QString label;
    if (status.phase == LiveStatus::Phase::WritingFeatures
        || status.phase == LiveStatus::Phase::SemanticEmbeddings
        || status.phase == LiveStatus::Phase::SemanticNeighbors) {
        label = QStringLiteral("Writing features… %1/%2 groups").arg(status.analyzed).arg(status.total);
    } else {
        // File-phase label is byte-stable for existing tests and menu UX.
        label = QStringLiteral("Analyzing… %1/%2").arg(status.analyzed).arg(status.total);
    }
    if (status.rate >= 0.0) {
        label += QStringLiteral(" · %1/s").arg(QString::number(status.rate, 'f', 1));
    }
    if (status.etaSecs.has_value()) {
        label += QStringLiteral(" · ~%1 left").arg(compactDuration(*status.etaSecs));
    }
    label += QStringLiteral(" · %1 elapsed").arg(clockDuration(static_cast<qint64>(std::llround(status.elapsedSecs))));
    return label;
}

QString finalSummary(int scanned, int skipped, int failed, int groups, double elapsedSecs,
                     int featuresWritten)
{
    const double secsPerTrack = scanned > 0 ? elapsedSecs / static_cast<double>(scanned) : 0.0;
    QString summary =
        QStringLiteral("Audio analysis: scanned %1, skipped %2, failed %3, groups %4")
            .arg(scanned)
            .arg(skipped)
            .arg(failed)
            .arg(groups);
    if (featuresWritten >= 0) {
        summary += QStringLiteral(", features written %1").arg(featuresWritten);
    }
    summary += QStringLiteral(" — %1 (%2s/track)")
                   .arg(spacedDuration(static_cast<qint64>(std::llround(elapsedSecs))))
                   .arg(QString::number(secsPerTrack, 'f', 1));
    return summary;
}

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
    const QHash<QString, QString> meta = loadMetaRows(featuresPath);
    if (meta.contains(QStringLiteral("last_scan_finished_at"))) {
        summary.lastRun.present = true;
        summary.lastRun.finishedAt = meta.value(QStringLiteral("last_scan_finished_at")).toLongLong();
        summary.lastRun.elapsedSecs = doubleMeta(meta, QStringLiteral("last_scan_elapsed_secs"));
        summary.lastRun.scanned = intMeta(meta, QStringLiteral("last_scan_scanned"));
        summary.lastRun.skipped = intMeta(meta, QStringLiteral("last_scan_skipped"));
        summary.lastRun.failed = intMeta(meta, QStringLiteral("last_scan_failed"));
        summary.lastRun.meanMsPerTrack = doubleMeta(meta, QStringLiteral("last_scan_mean_ms_per_track"));
        summary.lastRun.power = meta.value(QStringLiteral("last_scan_power"));
    }
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
