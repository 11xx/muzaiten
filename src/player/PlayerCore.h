#pragma once

#include "core/Track.h"

#include <QObject>
#include <QVector>

#include <functional>

class PlaybackBackend;

// Window-free playback/queue state machine: owns the playback backend, the
// canonical queue triple (tracks, current index, play-next boundary), the
// current track and the volume. MainWindow (and MPRIS/IPC/scrobble wiring)
// observe it through signals; everything UI- or persistence-shaped stays in
// the owner. The play-next region is [queueIndex + 1, playNextInsertIndex).
class PlayerCore final : public QObject {
    Q_OBJECT

public:
    // Resolves a track to a readable file path (link roots etc.); an empty
    // result marks the track unplayable.
    using PathResolver = std::function<QString(const Track &)>;

    // Takes ownership of the (required) backend; tests inject a fake.
    explicit PlayerCore(PlaybackBackend *backend, QObject *parent = nullptr);

    PlaybackBackend *backend() const { return m_backend; }
    void setPathResolver(PathResolver resolver) { m_resolvePath = std::move(resolver); }

    const QVector<Track> &queue() const { return m_queue; }
    int queueIndex() const { return m_queueIndex; }
    int playNextInsertIndex() const { return m_playNextInsertIndex; }
    const Track &currentTrack() const { return m_currentTrack; }
    double volume() const { return m_volume; }

    // -- transport ---------------------------------------------------------
    void playAt(int index, bool notifyScrobbler = true, bool startPaused = false, bool explicitJump = false);
    void next();
    void previous();
    void togglePlayPause();
    void play();  // resume, or start the queue when nothing is loaded (MPRIS Play)
    void seekRelative(qint64 offsetMs);
    void setVolume(double volume0To1);

    // -- queue mutation ------------------------------------------------------
    void appendAndPlay(const Track &track);
    void playTracksNext(const QVector<Track> &tracks);
    void appendTracks(const QVector<Track> &tracks);
    void moveRows(const QVector<int> &rows, int destinationRow);
    void removeRows(const QVector<int> &rows);
    // Queue clearing; user confirmation and snapshot/backlog policy stay with
    // the caller.
    void clearKeepingCurrent();
    void clearAll();
    void collapsePlayNext();
    void patchTracksFromMetadata(const QVector<Track> &tracks);
    void updateTrackRating(const QString &path, int rating0To100, bool hasUserRating);
    // After a tag write the file rating equals the user rating; patch all three
    // rating fields. Returns true when the current track was affected.
    bool applyRatingSync(const QString &path, int effectiveRating0To100);

    // -- restore / snapshot support ----------------------------------------
    // Replaces the queue triple without touching playback and without signals;
    // restore/adopt paths drive their own follow-up.
    void resetQueue(const QVector<Track> &tracks, int index = -1, int playNextInsertIndex = -1);
    // Re-presents a track as current without starting playback.
    void presentTrack(const Track &track);
    void prepareNext();

signals:
    // Emitted before tracks are added, so the owner can apply its queue
    // identity policy (append to source playlist / mark spontaneous).
    void aboutToAddTracks(const QVector<Track> &tracks);
    // Queue contents/order changed; observers re-derive every queue view.
    void queueChanged();
    // Queue track metadata changed in place; row order/current index did not.
    void queueTracksChanged(const QVector<int> &rows);
    // userInitiated gates "reveal the playing row" behaviour (explicit plays
    // reveal, gapless auto-advance does not).
    void currentIndexChanged(int index, bool userInitiated);
    void playNextRangeChanged();
    // A track became current for playback (presentation + optional scrobble).
    void currentTrackChanged(const Track &track, bool notifyScrobbler);
    // The current track object changed in place (metadata/rating refresh);
    // update displays without resetting playback position or scrobble state.
    void currentTrackUpdated(const Track &track);
    // Nothing is current anymore (queue emptied / current removed).
    void playbackCleared();
    void volumeChanged(double volume0To1);
    void trackUnresolvable(const Track &track);

private:
    void playCurrent(bool notifyScrobbler, bool startPaused);
    void collapsePlayNextIfStale();
    void onPreparedTrackStarted();
    void onFinished();
    QString resolvePath(const Track &track) const;

    PlaybackBackend *m_backend = nullptr;
    PathResolver m_resolvePath;
    QVector<Track> m_queue;
    int m_queueIndex = -1;
    int m_playNextInsertIndex = -1;
    Track m_currentTrack;
    double m_volume = 1.0;
};
