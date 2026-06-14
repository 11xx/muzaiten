#pragma once

#include "core/Track.h"

#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVector>

#include <functional>

class PlaybackBackend;

// Auto-advance behaviour when a track ends or the queue runs out.
enum class RepeatMode {
    Off,   // stop at end of queue
    All,   // wrap to the first track at the end of the queue
    One    // replay the current track
};

// How the next track is chosen on auto-advance / Next.
enum class ShuffleMode {
    Off,      // linear queue order
    Queue,    // random order within the current queue
    Library   // queue shuffle, but with a tunable chance to pull a fresh track
              // from the whole library instead
};

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
    // Supplies up to `count` library tracks for library-wide shuffle, excluding
    // anything already in the queue. Keeps PlayerCore free of any DB dependency.
    using RandomTrackProvider = std::function<QVector<Track>(int count, const QSet<QString> &excludePaths)>;

    // Takes ownership of the (required) backend; tests inject a fake.
    explicit PlayerCore(PlaybackBackend *backend, QObject *parent = nullptr);

    PlaybackBackend *backend() const { return m_backend; }
    void setPathResolver(PathResolver resolver) { m_resolvePath = std::move(resolver); }
    void setRandomTrackProvider(RandomTrackProvider provider) { m_randomTracks = std::move(provider); }

    const QVector<Track> &queue() const { return m_queue; }
    int queueIndex() const { return m_queueIndex; }
    int playNextInsertIndex() const { return m_playNextInsertIndex; }
    const Track &currentTrack() const { return m_currentTrack; }
    double volume() const { return m_volume; }

    // -- shuffle / repeat ---------------------------------------------------
    RepeatMode repeatMode() const { return m_repeatMode; }
    ShuffleMode shuffleMode() const { return m_shuffleMode; }
    int libraryShufflePercent() const { return m_libraryShufflePercent; }
    void setRepeatMode(RepeatMode mode);
    void setShuffleMode(ShuffleMode mode);
    // Chance (0..100) that a library-wide-shuffle advance pulls a fresh track.
    void setLibraryShufflePercent(int percent);

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
    void markTracksMissing(const QStringList &paths);
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
    void repeatModeChanged(RepeatMode mode);
    void shuffleModeChanged(ShuffleMode mode);
    void libraryShufflePercentChanged(int percent);

private:
    // A resolved auto-advance target: either an existing queue row (`index`) or
    // a fresh library track to append and play (`injected`).
    struct AutoNext {
        int index = -1;
        Track injected;
    };
    AutoNext decideAutoNext();
    int pickShuffleIndex();
    void applyAutoNext(const AutoNext &next);
    void markVisited(int index);
    void pushHistory(int index);
    void resetShuffleState();

    void playCurrent(bool notifyScrobbler, bool startPaused);
    void collapsePlayNextIfStale();
    void onPreparedTrackStarted();
    void onFinished();
    QString resolvePath(const Track &track) const;

    PlaybackBackend *m_backend = nullptr;
    PathResolver m_resolvePath;
    RandomTrackProvider m_randomTracks;
    QVector<Track> m_queue;
    int m_queueIndex = -1;
    int m_playNextInsertIndex = -1;
    Track m_currentTrack;
    double m_volume = 1.0;

    RepeatMode m_repeatMode = RepeatMode::Off;
    ShuffleMode m_shuffleMode = ShuffleMode::Off;
    int m_libraryShufflePercent = 20;
    // The advance gaplessly prepared for the current track; committed when the
    // backend reports the prepared track started (so the index lands on the row
    // actually preloaded, not a freshly re-rolled shuffle pick).
    AutoNext m_preparedNext;
    // Queue rows already played in the current shuffle cycle (includes current).
    QSet<int> m_shuffleVisited;
    // Previously-current rows, for retracing under shuffle on Previous.
    QVector<int> m_shuffleHistory;
};
