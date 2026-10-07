#include "reco/TasteProfile.h"

#include "core/FoldKey.h"
#include "core/GenreTags.h"
#include "features/FeatureStore.h"
#include "reco/TrackScorer.h"

#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>

namespace TasteProfiles {

bool fromLibrary(const QString &libraryPath, const QString &featuresPath, bool optIn,
                 bool includeDecades, TasteProfile *profile, QString *error)
{
    if (!optIn) {
        *error = QStringLiteral("Taste export requires explicit opt-in on every invocation: --opt-in.");
        return false;
    }
    if (!QFileInfo(libraryPath).isFile()) {
        *error = QStringLiteral("No local library is available for taste export.");
        return false;
    }
    const QString connection = QStringLiteral("taste-export-%1").arg(QUuid::createUuid().toString());
    QVector<TrackScorer::Candidate> tracks;
    bool success = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000"));
        db.setDatabaseName(libraryPath);
        if (db.open()) {
            QHash<QString, QString> aliases;
            QSet<QString> ignored;
            QSqlQuery query(db);
            bool schemaOk = query.exec(QStringLiteral("SELECT alias_folded, canonical_folded FROM genre_aliases"));
            while (schemaOk && query.next()) {
                aliases.insert(query.value(0).toString(), query.value(1).toString());
            }
            schemaOk = schemaOk && query.exec(QStringLiteral("SELECT genre_folded FROM radio_ignored_genres"));
            while (schemaOk && query.next()) {
                ignored.insert(query.value(0).toString());
            }
            success = schemaOk && query.exec(QStringLiteral(
                "SELECT t.path, t.musicbrainz_recording_id, t.artist_name, t.title, "
                "t.original_date, t.date, GROUP_CONCAT(g.genre_folded, char(31)) "
                "FROM tracks t LEFT JOIN track_genres g ON g.track_id = t.id "
                "WHERE t.missing = 0 AND t.metadata_scanned = 1 "
                "GROUP BY t.id ORDER BY t.path"));
            while (success && query.next()) {
                TrackScorer::Candidate track;
                track.path = query.value(0).toString();
                track.songKey = FoldKey::songKey(query.value(1).toString(), query.value(2).toString(),
                                                query.value(3).toString());
                track.year = query.value(4).toString().left(4).toInt();
                if (track.year <= 0) {
                    track.year = query.value(5).toString().left(4).toInt();
                }
                for (const QString &raw : query.value(6).toString().split(QChar(31), Qt::SkipEmptyParts)) {
                    const QString genre = GenreTags::canonical(GenreTags::folded(raw), aliases);
                    if (!ignored.contains(genre) && !GenreTags::isNonGenre(genre)) {
                        track.genresFolded.append(genre);
                    }
                }
                tracks.append(track);
            }
            success = success && query.lastError().type() == QSqlError::NoError;
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    if (!success) {
        *error = QStringLiteral("Could not read taste aggregates from the local library.");
        return false;
    }
    FeatureStore features(featuresPath);
    QStringList paths;
    paths.reserve(tracks.size());
    for (const auto &track : tracks) {
        paths.append(track.path);
    }
    const auto groups = features.contentGroupsForPaths(paths);
    const QSet<qint64> uniqueGroups(groups.begin(), groups.end());
    const auto scalarsByGroup = features.scalarsForGroups(uniqueGroups.values());
    for (auto &track : tracks) {
        const qint64 group = groups.value(track.path, -1);
        track.contentGroupId = group;
        if (group < 0) {
            continue;
        }
        const FeatureStore::Scalars scalars = scalarsByGroup.value(group);
        if (scalars.valid) {
            track.tempoBpm = scalars.tempoBpm;
            track.energy = scalars.energy;
        }
    }
    return aggregate(tracks, optIn, includeDecades, profile, error);
}

} // namespace TasteProfiles
