#include "scanner/RatingTagSyncWorker.h"

#include "db/Database.h"
#include "fs/LinkRoot.h"
#include "scanner/TagRatingWriter.h"
#include "scanner/TagReader.h"

#include <QFileInfo>
#include <QUuid>

RatingTagSyncWorker::RatingTagSyncWorker(QString databasePath, RatingTagSyncRequest request)
    : m_databasePath(std::move(databasePath))
    , m_request(std::move(request))
{
}

void RatingTagSyncWorker::cancel()
{
    m_cancel = true;
}

void RatingTagSyncWorker::run()
{
    RatingTagSyncSummary summary;
    Database database(QStringLiteral("rating-sync-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!database.open(m_databasePath)) {
        emit finished(summary, database.lastError());
        return;
    }

    const int total = static_cast<int>(m_request.tracks.size());
    const PathResolver resolver(m_request.linkRoots);
    const TagRatingWriter writer;
    const TagReader reader;

    for (const Track &track : std::as_const(m_request.tracks)) {
        if (m_cancel) {
            break;
        }

        ++summary.checked;
        emit progress(summary.checked, total, track.path);

        const int desired = track.effectiveRating0To100;
        if (desired < 0) {
            continue;
        }

        const PathResolution writePath = resolver.resolveLocalPath(track.path, PathUse::Write);
        if (writePath.preferredPath.isEmpty()) {
            database.setPendingTrackRatingWrite(track.path, desired, QStringLiteral("blocked_no_writable_path"), writePath.failureReason);
            ++summary.noWritablePath;
            continue;
        }

        const TagRatingWriteResult write = writer.writeRating(writePath.preferredPath, desired);
        if (write.ok) {
            const Track reread = reader.read(writePath.preferredPath);
            database.updateScannedTrackRating(track.path, reread.rating0To100, reread.ratingSource, reread.fileSize, reread.fileMtime);
            database.clearPendingTrackRatingWrite(track.path);
            ++summary.written;
            continue;
        }
        if (write.existingTagWon) {
            const QFileInfo info(writePath.preferredPath);
            database.updateScannedTrackRating(track.path,
                                             write.fileRating0To100,
                                             Rating::Source::MusicBeeCompatible,
                                             info.size(),
                                             info.lastModified().toSecsSinceEpoch());
            database.setUserTrackRating(track.path, write.fileRating0To100);
            database.clearPendingTrackRatingWrite(track.path);
            ++summary.tagWon;
            continue;
        }

        database.setPendingTrackRatingWrite(track.path, desired, QStringLiteral("failed"), write.error);
        ++summary.failed;
    }

    emit finished(summary, {});
}
