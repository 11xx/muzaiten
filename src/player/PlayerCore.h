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
    Library,  // queue shuffle, but with a tunable chance to pull a fresh track
              // from the whole library instead
    Radio     // queue shuffle, but library pulls come from the radio provider
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

    // The UI decides whether a track can start normally, needs native DSD output,
    // or must wait for a user-confirmed device takeover. Keeping the decision as
    // a narrow callback preserves PlayerCore's window-free ownership of transport
    // state while leaving PipeWire/device UI in MainWindow.
    struct PlaybackStartPlan {
        enum class Action { Normal, NativeDsd, DeferForDsdTakeover, Skip };
        Action action = Action::Normal;
        QString device;
        QString reason;
    };
    using PlaybackStartPlanner = std::function<PlaybackStartPlan(const Track &)>;

    // Takes ownership of the (required) backend; tests inject a fake.
    explicit PlayerCore(PlaybackBackend *backend, QObject *parent = nullptr);

    PlaybackBackend *backend() const { return m_backend; }
    void setPathResolver(PathResolver resolver) { m_resolvePath = std::move(resolver); }
    void setRandomTrackProvider(RandomTrackProvider provider) { m_randomTracks = std::move(provider); }
    // Radio's recommendation-driven provider. Explicit Start Radio consults it
    // while radioActive(); ambient Radio shuffle consults it for taste-aware
    // library pulls while radioActive() remains false.
    void setRadioProvider(RandomTrackProvider provider) { m_radioTracks = std::move(provider); }
    void setPlaybackStartPlanner(PlaybackStartPlanner planner) { m_playbackStartPlanner = std::move(planner); }

    const QVector<Track> &queue() const { return m_queue; }
    int queueIndex() const { return m_queueIndex; }
    int playNextInsertIndex() const { return m_playNextInsertIndex; }
    const Track &currentTrack() const { return m_currentTrack; }
    double volume() const { return m_volume; }

    // -- shuffle / repeat ---------------------------------------------------
    RepeatMode repeatMode() const { return m_repeatMode; }
    ShuffleMode shuffleMode() const { return m_shuffleMode; }
    int libraryShufflePercent() const { return m_libraryShufflePercent; }
    int radioShufflePercent() const { return m_radioShufflePercent; }
    void setRepeatMode(RepeatMode mode);
    void setShuffleMode(ShuffleMode mode);
    // Chance (0..100) that a library-wide-shuffle advance pulls a fresh track.
    void setLibraryShufflePercent(int percent);
    // Chance (0..100) that Radio shuffle pulls a taste-aware library track.
    void setRadioShufflePercent(int percent);

    // -- radio --------------------------------------------------------------
    // An explicit Start Radio session installs a scored provider
    // (setRadioProvider) and, while active, extends past the queue's end with
    // recommendation picks instead of stopping. Ambient Radio shuffle uses the
    // same provider with this flag false; deactivating clears nothing else.
    bool radioActive() const { return m_radioActive; }
    void setRadioActive(bool active);

    // -- transport ---------------------------------------------------------
    void playAt(int index, bool notifyScrobbler = true, bool startPaused = false, bool explicitJump = false);
    void next();
    void previous();
    void togglePlayPause();
    void play();  // resume, or start the queue when nothing is loaded (MPRIS Play)
    void seekRelative(qint64 offsetMs);
    void setVolume(double volume0To1);
    // Completes the one outstanding native-DSD takeover request. Stale UI timer
    // callbacks are harmless: they become no-ops after a different track starts.
    void resolveDsdTakeover(bool accepted);

    // -- queue mutation ------------------------------------------------------
    void appendAndPlay(const Track &track);
    void playTracksNext(const QVector<Track> &tracks);
    void appendTracks(const QVector<Track> &tracks);
    // Radio's batch-append path: appends each track through the SAME
    // aboutToInjectLibraryTrack signal a single JIT pick uses, one emission per
    // track, then updates the queue once. Deliberately distinct from
    // appendTracks()/aboutToAddTracks: for a playlist-sourced queue,
    // aboutToAddTracks tells the owner to mirror the tracks into that playlist
    // (see MainWindow::prepareQueueForTrackAddition) -- exactly what a batch of
    // radio picks must NOT do, since they were never part of that playlist.
    void injectTracks(const QVector<Track> &tracks);
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
    // Emitted before a library-wide-shuffle OR radio injection appends a fresh,
    // non-source track that the player itself chose (never a user edit). Distinct
    // from aboutToAddTracks: the owner must treat the track as queue-only and must
    // not mirror it into any backing playlist, since it was never part of that
    // source — exactly the semantics a radio session wants for its picks.
    void aboutToInjectLibraryTrack(const Track &track);
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
    // The playing track reached its natural end (backend-finished or gapless
    // advance). Telemetry-only: observers must not drive transport from it —
    // auto-advance is already handled internally. Covers both the
    // backend-finished path (onFinished) and the gapless-advance path
    // (onPreparedTrackStarted, where PlaybackBackend::finished never fires).
    void trackFinished(const Track &track);
    // Nothing is current anymore (queue emptied / current removed).
    void playbackCleared();
    void volumeChanged(double volume0To1);
    void trackUnresolvable(const Track &track);
    void dsdTakeoverRequested(const Track &track, const QString &device);
    void trackStartSkipped(const Track &track, const QString &reason);
    void repeatModeChanged(RepeatMode mode);
    void shuffleModeChanged(ShuffleMode mode);
    void libraryShufflePercentChanged(int percent);
    void radioShufflePercentChanged(int percent);
    void radioActiveChanged(bool active);

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
    // Records a forward auto-advance from `fromIndex` to `toIndex`: pushes the
    // departing row onto the back history and consumes the matching entry from
    // the forward trail (a remembered Next), or drops the trail on a fresh pick.
    void recordForwardStep(int fromIndex, int toIndex);
    // Navigates to `index` as an internal shuffle jump (Previous retrace / Next
    // replay): always collapses the play-next region so the rows skipped over
    // aren't badged, but keeps the shuffle forward trail intact.
    void playShuffleJump(int index);
    // A user-selected track starts a fresh no-repeat shuffle bucket from that
    // row, but keeps back navigation meaningful by preserving history and
    // adding the row we left as the immediate Previous target.
    void refreshShuffleForManualPick(int previousIndex, int selectedIndex);
    void resetShuffleState();

    void playCurrent(bool notifyScrobbler, bool startPaused);
    void startTrack(const Track &track, const QString &playbackPath, bool notifyScrobbler,
                    bool startPaused, const PlaybackStartPlan &plan);
    void skipCurrentTrack();
    void collapsePlayNextIfStale();
    void onPreparedTrackStarted();
    void onFinished();
    QString resolvePath(const Track &track) const;

    PlaybackBackend *m_backend = nullptr;
    PathResolver m_resolvePath;
    RandomTrackProvider m_randomTracks;
    RandomTrackProvider m_radioTracks;
    PlaybackStartPlanner m_playbackStartPlanner;
    QVector<Track> m_queue;
    int m_queueIndex = -1;
    int m_playNextInsertIndex = -1;
    Track m_currentTrack;
    double m_volume = 1.0;

    RepeatMode m_repeatMode = RepeatMode::Off;
    ShuffleMode m_shuffleMode = ShuffleMode::Off;
    int m_libraryShufflePercent = 20;
    int m_radioShufflePercent = 80;
    bool m_radioActive = false;
    // The advance gaplessly prepared for the current track; committed when the
    // backend reports the prepared track started (so the index lands on the row
    // actually preloaded, not a freshly re-rolled shuffle pick).
    AutoNext m_preparedNext;
    // Queue rows already played in the current shuffle cycle (includes current).
    QSet<int> m_shuffleVisited;
    // Previously-current rows, for retracing under shuffle on Previous (back trail).
    QVector<int> m_shuffleHistory;
    // Rows retraced past by Previous, so a subsequent Next/auto-advance replays the
    // same shuffle order forward instead of re-rolling. Top is the immediate next.
    // Populated only by Previous; consumed by forward navigation; dropped on any
    // explicit jump, fresh pick, or queue mutation.
    QVector<int> m_shuffleForward;
    struct PendingDsdTakeover {
        bool active = false;
        Track track;
        QString playbackPath;
        QString device;
        bool notifyScrobbler = true;
        bool startPaused = false;
    };
    PendingDsdTakeover m_pendingDsdTakeover;
    // Set after a declined/timed-out prompt. Native DSD tracks are then skipped
    // without further prompts until any track has successfully started.
    bool m_skipDsdTakeoverBlock = false;
    // Re-entrancy guard for skipCurrentTrack(): flattens a cascade of skips into
    // one loop instead of recursing a stack frame per skipped track.
    bool m_skipInProgress = false;
    bool m_skipPending = false;
};
