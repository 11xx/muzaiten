#include "reco/TasteProfile.h"

#include "core/FoldKey.h"
#include "core/GenreTags.h"
#include "db/Database.h"
#include "features/FeatureStore.h"
#include "reco/TrackScorer.h"

#include <QFileInfo>
#include <QUuid>

namespace TasteProfiles {

bool fromLibrary(const QString &libraryPath, const QString &featuresPath, bool optIn,
                 bool includeDecades, TasteProfile *profile, QString *error)
{
    if (!optIn) {
        *error = QStringLiteral("Taste export requires explicit opt-in on every invocation: --opt-in.");
        return false;
    }
    QVector<TrackScorer::Candidate> tracks;
    return libraryCandidates(libraryPath, featuresPath, &tracks, error)
        && aggregate(tracks, optIn, includeDecades, profile, error);
}

bool libraryCandidates(const QString &libraryPath, const QString &featuresPath,
                       QVector<TrackScorer::Candidate> *candidates, QString *error)
{
    if (!QFileInfo(libraryPath).isFile()) {
        *error = QStringLiteral("No local library is available for taste export.");
        return false;
    }
    QVector<TrackScorer::Candidate> &tracks = *candidates;
    tracks.clear();
    {
        Database library(QStringLiteral("taste-export-%1").arg(QUuid::createUuid().toString()));
        const auto unreadable = [error](const QString &reason) {
            *error = QStringLiteral("Could not read taste aggregates from the local library: %1.").arg(reason);
            return false;
        };
        if (!library.openReadOnly(libraryPath)) {
            return unreadable(library.lastError());
        }
        QVector<RadioCandidateRow> rows;
        QString reason;
        if (!library.libraryRadioRows(&rows, &reason)) {
            return unreadable(reason);
        }
        const QHash<QString, QString> aliases = library.genreAliases();
        const QSet<QString> ignored = library.ignoredRadioGenres();
        // "Don't learn from this" keeps a track out of every taste signal;
        // "Never play on radio" does not, since the file describes the library.
        const QSet<QString> noLearn = library.flaggedPaths(Database::TrackFlag::NoLearn);
        tracks.reserve(rows.size());
        for (const RadioCandidateRow &row : rows) {
            if (noLearn.contains(row.path)) {
                continue;
            }
            TrackScorer::Candidate track;
            track.path = row.path;
            track.songKey = FoldKey::songKey(row.mbRecordingId, row.artistName, row.title);
            track.year = row.year;
            for (const QString &raw : row.genresFolded) {
                const QString genre = GenreTags::canonical(GenreTags::folded(raw), aliases);
                if (!ignored.contains(genre) && !GenreTags::isNonGenre(genre)) {
                    track.genresFolded.append(genre);
                }
            }
            tracks.append(track);
        }
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
    return true;
}

} // namespace TasteProfiles
