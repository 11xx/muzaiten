#include "ui/RatingSyncController.h"
#include "ui/MainWindow.h"
#include "app/AppCore.h"
#include "db/Database.h"
#include "mpris/MprisService.h"
#include "player/PlayerCore.h"
#include "scanner/RatingTagSyncWorker.h"
#include "ui/AlbumGrid.h"
#include "ui/MusicExplorerView.h"
#include "ui/PlaylistView.h"
#include "ui/QueueStore.h"
#include "ui/TrackTable.h"
#include <QMessageBox>
#include <QStatusBar>
#include <QThread>
#include <QTimer>
#include <algorithm>

RatingSyncController::RatingSyncController(MainWindow &window) : QObject(&window), m_window(window) {}

void RatingSyncController::applyTrackRating(const Track &track, int rating0To100, const QString &sourceSurface)
{
    const auto oldRating = m_window.m_database->trackRatingSnapshot(track.path);
    const bool ok = rating0To100 < 0 ? m_window.m_database->clearUserTrackRating(track.path) : m_window.m_database->setUserTrackRating(track.path, rating0To100);
    if (!ok) {
        QMessageBox::warning(&m_window, QStringLiteral("Rating"), m_window.m_database->lastError());
        return;
    }
    if (rating0To100 >= 0) {
        m_window.m_database->setPendingTrackRatingWrite(track.path, rating0To100, QStringLiteral("pending"));
    } else {
        m_window.m_database->clearPendingTrackRatingWrite(track.path);
    }
    Track eventTrack = track;
    if (eventTrack.musicBrainz.recordingId.isEmpty()) {
        eventTrack.musicBrainz.recordingId = oldRating.mbRecordingId;
    }
    m_window.m_core->recordRatingEvent(eventTrack,
                              oldRating.hasUserRating,
                              oldRating.userRating0To100,
                              oldRating.effectiveRating0To100,
                              rating0To100,
                              sourceSurface);
    // Patch the rated row in place instead of rebuilding the whole track table
    // (a full reload also dropped scroll/selection, hence the old remember/restore
    // dance). The album grid still refreshes because its star reflects the album's
    // average track rating, which this edit can shift. track.rating0To100 already
    // carries the scanned file rating (or unset), so it is the right fallback when
    // a user rating is cleared.
    const bool nowHasUserRating = rating0To100 >= 0;
    m_window.m_trackTable->updateTrackRating(track.path, nowHasUserRating ? rating0To100 : track.rating0To100, nowHasUserRating);
    if (m_window.m_musicExplorerView != nullptr) {
        m_window.m_musicExplorerView->refreshExpandedTracks();
    }
    m_window.refreshAlbumGrid();
    m_window.m_player->updateTrackRating(track.path, rating0To100 >= 0 ? rating0To100 : track.rating0To100, rating0To100 >= 0);
    if (m_window.m_player->currentTrack().path == track.path) {
        m_window.presentNowPlaying(m_window.m_player->currentTrack());
        m_window.m_mpris->setTrack(m_window.m_player->currentTrack());
    }
    m_window.m_queueStore->updateTrackRating(track.path, rating0To100 >= 0 ? rating0To100 : track.rating0To100, rating0To100 >= 0);
    if (m_window.m_playlistView != nullptr) {
        m_window.m_playlistView->updateTrackRating(track.path, rating0To100 >= 0 ? rating0To100 : track.rating0To100);
    }
    m_window.scheduleQueueStateSave();

    if (rating0To100 >= 0 && m_window.m_librarySource == LibrarySource::Local) {
        schedulePendingRatingTagSync();
    }
}

void RatingSyncController::startRatingTagSync(const QVector<Track> &tracks, int scope)
{
    if (tracks.isEmpty()) {
        m_window.statusBar()->showMessage(QStringLiteral("No rating tags to sync"), 5000);
        return;
    }
    if (m_window.m_ratingTagSyncRunning) {
        m_window.m_ratingTagSyncPending = true;
        m_window.statusBar()->showMessage(QStringLiteral("Rating tag sync already running; queued latest pending writes"), 5000);
        return;
    }

    RatingTagSyncRequest request;
    request.scope = static_cast<RatingTagSyncRequest::Scope>(scope);
    request.tracks = tracks;
    request.linkRoots = m_window.m_database->linkRoots();

    auto *thread = new QThread(this);
    auto *worker = new RatingTagSyncWorker(m_window.databasePath(), request);
    m_window.m_ratingTagSyncRunning = true;
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, &RatingTagSyncWorker::run);
    connect(worker, &RatingTagSyncWorker::progress, this, [this](int checked, int total, const QString &) {
        m_window.statusBar()->showMessage(QStringLiteral("Rating tag sync: %1 / %2 checked").arg(checked).arg(total));
    });
    connect(worker, &RatingTagSyncWorker::finished, this, [this, thread, worker](const RatingTagSyncSummary &summary, const QString &error) {
        if (!error.isEmpty()) {
            QMessageBox::warning(&m_window, QStringLiteral("Rating tag sync"), error);
        } else {
            m_window.statusBar()->showMessage(QStringLiteral("Rating tag sync complete: %1 written, %2 no writable path, %3 failed")
                                         .arg(summary.written)
                                         .arg(summary.noWritablePath)
                                         .arg(summary.failed),
                                     10000);
        }
        // Patch only the rows the worker actually wrote, in place — no full table
        // reload and no per-queued-track DB requery (the old N+1 main-thread freeze
        // the user felt "when the tag is written"). The DB is already reconciled by
        // the worker; the effective rating equals the just-written value.
        bool currentTrackChanged = false;
        for (const RatingTagSyncUpdate &update : summary.updates) {
            const int effective = update.effectiveRating0To100;
            const bool hasUserRating = effective >= 0;
            m_window.m_trackTable->updateTrackRating(update.path, effective, hasUserRating);
            if (m_window.m_playlistView != nullptr) {
                m_window.m_playlistView->updateTrackRating(update.path, effective);
            }
            currentTrackChanged = m_window.m_player->applyRatingSync(update.path, effective) || currentTrackChanged;
        }
        if (!summary.updates.isEmpty() && m_window.m_musicExplorerView != nullptr) {
            m_window.m_musicExplorerView->refreshExpandedTracks();
        }
        if (currentTrackChanged) {
            m_window.presentNowPlaying(m_window.m_player->currentTrack());
            m_window.m_mpris->setTrack(m_window.m_player->currentTrack());
        }
        if (!summary.updates.isEmpty()) {
            m_window.m_queueStore->setSnapshot(m_window.m_player->queue(), m_window.m_player->queueIndex(),
                                      m_window.m_player->queueIndex() + 1, m_window.m_player->playNextInsertIndex());
            m_window.refreshPlayNextRange();
            m_window.scheduleQueueStateSave();
        }
        m_window.m_ratingTagSyncRunning = false;
        const bool runPendingAgain = m_window.m_ratingTagSyncPending;
        m_window.m_ratingTagSyncPending = false;
        worker->deleteLater();
        thread->quit();
        if (runPendingAgain) {
            QTimer::singleShot(0, this, [this]() {
                startRatingTagSync(m_window.m_database->tracksWithPendingRatingWrites(), static_cast<int>(RatingTagSyncRequest::Scope::PendingWrites));
            });
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void RatingSyncController::schedulePendingRatingTagSync()
{
    m_window.m_ratingTagSyncPending = true;
    m_window.statusBar()->showMessage(QStringLiteral("Queued rating tag write"), 3000);
    QTimer::singleShot(0, this, [this]() {
        if (m_window.m_ratingTagSyncRunning || !m_window.m_ratingTagSyncPending) {
            return;
        }
        m_window.m_ratingTagSyncPending = false;
        startRatingTagSync(m_window.m_database->tracksWithPendingRatingWrites(), static_cast<int>(RatingTagSyncRequest::Scope::PendingWrites));
    });
}

void RatingSyncController::syncCurrentTrackRatingTags()
{
    const Track current = m_window.m_player->currentTrack();
    if (m_window.m_librarySource != LibrarySource::Local || current.path.isEmpty() || current.effectiveRating0To100 < 0) {
        m_window.statusBar()->showMessage(QStringLiteral("No current local rated track to sync"), 5000);
        return;
    }
    startRatingTagSync({current}, static_cast<int>(RatingTagSyncRequest::Scope::Track));
}

void RatingSyncController::syncCurrentArtistRatingTags()
{
    if (m_window.m_librarySource != LibrarySource::Local || m_window.m_currentArtist.isEmpty()) {
        m_window.statusBar()->showMessage(QStringLiteral("No current local artist to sync"), 5000);
        return;
    }
    QVector<Track> tracks;
    const QVector<Track> userRated = m_window.m_database->tracksWithUserRatings();
    const QVector<Track> pending = m_window.m_database->tracksWithPendingRatingWrites();
    for (const Track &track : userRated + pending) {
        const bool alreadyQueued = std::any_of(tracks.cbegin(), tracks.cend(), [&track](const Track &queued) {
            return queued.path == track.path;
        });
        if (track.albumArtistName == m_window.m_currentArtist && !alreadyQueued) {
            tracks.push_back(track);
        }
    }
    startRatingTagSync(tracks, static_cast<int>(RatingTagSyncRequest::Scope::CurrentArtist));
}

void RatingSyncController::syncAllSavedRatingTags()
{
    startRatingTagSync(m_window.m_database->tracksWithUserRatings(), static_cast<int>(RatingTagSyncRequest::Scope::SavedRatedTracks));
}

void RatingSyncController::retryPendingRatingTags()
{
    startRatingTagSync(m_window.m_database->tracksWithPendingRatingWrites(), static_cast<int>(RatingTagSyncRequest::Scope::PendingWrites));
}

void RatingSyncController::applyAlbumRating(const QString &albumArtistName, const QString &albumTitle, int rating0To100)
{
    const bool ok = rating0To100 < 0 ? m_window.m_database->clearUserAlbumRating(albumArtistName, albumTitle) : m_window.m_database->setUserAlbumRating(albumArtistName, albumTitle, rating0To100);
    if (!ok) {
        QMessageBox::warning(&m_window, QStringLiteral("Rating"), m_window.m_database->lastError());
        return;
    }
    m_window.refreshAlbumGrid();
}
