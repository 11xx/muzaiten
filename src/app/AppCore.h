#pragma once

#include "core/Track.h"
#include "reco/RadioProfile.h"
#include "reco/TrackScorer.h"
#include "scrobble/ScrobbleBackfill.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>

class ArtworkCache;
class Database;
class FeatureStore;
class IpcServer;
class LastFmScrobbler;
class ListenBrainzScrobbler;
class ListenHistoryStore;
class ListenTracker;
class MainWindow;
class PlayEventRecorder;
class MprisService;
class PlaybackBackend;
class PlayerCore;
class PlaylistDatabase;
class RadioSession;
class QSystemTrayIcon;
class QThread;
class SettingsStore;

class AppCore final : public QObject {
    Q_OBJECT

public:
    // Main-thread mirror of the backfill worker's state, refreshed from its
    // progress/finished/failed signals. Consumed by the Scrobblers menu (see
    // MainWindow::updateBackfillStatusDisplay) and the IPC "status" reply.
    struct BackfillStatus {
        QString service;          // ListenHistoryStore::ListenBrainz/LastFm, empty when never run
        bool running = false;
        int processed = 0;
        int inserted = 0;          // rows stored by the current/most recent run
        int storedTotal = 0;       // cumulative imported rows in the DB (ListenBrainz)
        qint64 reachedTs = 0;      // ListenBrainz cursor reached; 0 for Last.fm/unknown
        qint64 totalListens = 0;   // ListenBrainz listen-count total; 0 when unknown/Last.fm
        QString lastMessage;       // outcome of the most recent finished/failed run
    };

    explicit AppCore(QObject *parent = nullptr);
    ~AppCore() override;

    PlayerCore          *player() const;
    PlaybackBackend     *backend() const;
    Database            *database() const;
    PlaylistDatabase    *playlistDatabase() const;
    SettingsStore       *settings() const;
    ArtworkCache        *artworkCache() const;
    FeatureStore        *features() const;
    ListenHistoryStore  *listenHistory() const;
    ListenTracker       *listenTracker() const;
    PlayEventRecorder   *playEventRecorder() const;
    MprisService        *mpris() const;
    IpcServer           *ipc() const;
    MainWindow          *window() const;

    ListenBrainzScrobbler *listenBrainzScrobbler() const;
    LastFmScrobbler       *lastFmScrobbler() const;
    QThread               *listenBrainzThread() const;
    QThread               *lastFmThread() const;

    // Start a rule-based radio session seeded from a library track. An already
    // playing seed is kept in place without restarting; another seed starts
    // immediately. Recommendation preparation and queue extension follow
    // asynchronously. Returns false when the path is not in the library.
    bool startRadio(const QString &seedPath);
    // Start a radio session seeded from an album-artist name. Uses a synthetic
    // seed built from the artist's genre/era aggregate and opens with a
    // representative track by that artist.
    bool startArtistRadio(const QString &artistName);
    // Start a seedless, radio-backed library mix. Returns false without state
    // change when the mode is unknown or filtering cannot produce playable
    // candidates.
    bool startMix(const QString &mode);
    // End the session: deactivates radio and tears down the provider/session.
    // Queue contents stay as they are.
    void stopRadio();
    // Replace only unplayed radio picks below queueRow. Manual queue entries and
    // every row at or above the anchor are preserved. Returns true when a refresh
    // was started.
    bool refreshRadioPicksBelow(int queueRow);
    QString radioPickReason(const QString &path) const;
    bool trackFlag(const QString &trackPath, const QString &flag) const;
    bool setTrackFlagForSong(const QString &trackPath, const QString &flag, bool on);
    int forgetTrackBehaviorForSong(const QString &trackPath, bool includeImportedListens);
    bool recordRatingEvent(const Track &track,
                           bool hadOldUserRating,
                           int oldUserRating0To100,
                           int oldEffectiveRating0To100,
                           int newRating0To100,
                           const QString &sourceSurface);
    void recordUserQueueRemovals(const QVector<int> &rows);

    // Radio exploration/batch/refill knobs (plans/music-recommendation-plan.md,
    // "Batch radio queue"). Backed by library-DB settings and exposed through
    // PlayerBar's radio menus. radioExploration() reports the persisted value;
    // a live session can temporarily be higher while "adventurous" is armed.
    int radioExploration() const;
    void setRadioExploration(int value0To100, bool persist);
    int radioBatchSize() const;
    void setRadioBatchSize(int value1To100);
    int radioRefillThreshold() const;
    void setRadioRefillThreshold(int value0To100);
    // The "Adventurous (this session)" boost: while a session is active, toggles
    // the LIVE session exploration between 85 and the persisted value; with no
    // session active it arms the next startRadio to begin at 85 (one-shot --
    // consumed and reset by startRadio). Reset on stopRadio too.
    bool radioAdventurous() const;
    void setRadioAdventurous(bool on);

    QString databasePath() const;
    QString playlistDatabasePath() const;
    QString featuresPath() const;
    QString listenHistoryPath() const;
    bool scrobbleOffline() const;
    bool trayAvailable() const;
    bool isQuitting() const;

    void setTrayAlwaysVisible(bool visible);

    // Shared start path for the scrobbler backfill, used by both the IPC
    // handler and the Scrobblers menu. Returns "started" | "already-running" |
    // "missing-credentials" | "unknown-service". Starting ListenBrainz clears
    // any stale canceled flag so a later interruption can auto-resume again.
    QString startBackfill(const QString &service);
    // Explicit cancel: the ONLY thing that stops eager auto-resume. Marks the
    // ListenBrainz canceled flag (via the main-thread store, so it survives
    // even if the app quits before the worker acknowledges) then asks the
    // worker to abort.
    void cancelBackfill();
    // Clear a service's completed/synced marker so the next import re-walks full
    // history (recovers listens added behind an already-imported range). Returns
    // "reset" | "already-running" | "history-unavailable" | "unknown-service".
    // Refuses while any backfill is running so it can't race the live cursor.
    QString resetBackfill(const QString &service);
    BackfillStatus backfillStatus() const { return m_backfillStatus; }

    const QVector<RadioProfile> &radioProfiles() const;
    const RadioProfile &activeRadioProfile() const;
    bool selectRadioProfile(const QString &name);
    bool previewActiveRadioProfile(const RadioProfile &profile);
    bool commitActiveRadioProfilePreview();
    bool restoreActiveRadioProfile(const RadioProfile &profile);
    bool undoRadioProfile();
    bool redoRadioProfile();
    bool canUndoRadioProfile() const;
    bool canRedoRadioProfile() const;
    bool createRadioProfile(const QString &name, bool duplicateActive);
    bool renameActiveRadioProfile(const QString &name);
    bool deleteActiveRadioProfile();
    bool resetActiveRadioProfile();

signals:
    // Emitted on every backfill progress/finished/failed update, so the
    // Scrobblers menu (which stays open while browsing) can refresh live.
    void backfillStatusChanged();
    void radioLoadingChanged(bool loading);

public slots:
    void showWindow();
    void releaseWindow();
    void quit();

    // Called by MainWindow's resumePlaybackAt to resume scrobble state
    // after restoring a saved playback position.
    void resumeScrobblers(const Track &track, qint64 elapsedMs, bool playing);

private:
    // ~20s after construction, eagerly resume an interrupted ListenBrainz
    // import if one is pending (see startBackfill's cancel-vs-interrupt rule).
    void maybeAutoResumeListenBrainzBackfill();
    void setupMprisWiring();
    void setupScrobbleWiring();
    void setupIpcHandler();
    void setupTrayIcon();
    void restoreSavedPlayback();
    void updateMprisCapabilities();
    void releaseIdleMemory();
    void restoreInteractiveMemory();
    void notifyScrobblersTrackStarted(const Track &track);
    QJsonObject handleIpcCommand(const QString &command, const QJsonObject &args);
    QJsonObject ipcStatus() const;
    // Build the scrobbler-backfill match index from the library DB (folded
    // artist+title and recording MBID -> track path).
    ScrobbleBackfill::LibraryIndex buildLibraryIndex() const;
    QStringList radioFoldedGenresForTrack(const QString &path, const QHash<QString, QString> &genreAliases,
                                          const QSet<QString> &ignoredRadioGenres) const;
    QStringList pathsForSongKeyOfTrack(const QString &trackPath) const;
    QHash<QString, QString> buildResolvedSongKeyMap() const;
    void attachRadioFeatures(QVector<TrackScorer::Candidate> &candidates) const;
    void attachRadioFeatures(TrackScorer::Candidate &candidate) const;
    QStringList radioNeighborCandidatePaths(const QStringList &anchorPaths) const;
    QHash<qint64, QVector<float>> radioEmbeddingsForSession(const QVector<TrackScorer::Candidate> &pool,
                                                            const TrackScorer::Candidate &seed = {}) const;
    TrackScorer::Candidate buildRadioSeedCandidate(const Track &seed, const QStringList &seedGenresFolded,
                                                   const QHash<QString, QString> &resolvedSongKeys) const;
    QVector<TrackScorer::Candidate> buildRadioCandidatePool(const QStringList &informativeGenres,
                                                            const QHash<QString, QString> &genreAliases,
                                                            const QSet<QString> &ignoredRadioGenres,
                                                            const QHash<QString, QString> &resolvedSongKeys,
                                                            const QStringList &neighborAnchorPaths = {}) const;
    QVector<TrackScorer::Candidate> buildRadioFallbackPool(int limit,
                                                           const QHash<QString, QString> &genreAliases,
                                                           const QSet<QString> &ignoredRadioGenres,
                                                           const QHash<QString, QString> &resolvedSongKeys) const;
    QHash<QString, double> buildRadioGenreIdf(const QHash<QString, QString> &genreAliases,
                                              const QSet<QString> &ignoredRadioGenres) const;
    TrackScorer::Weights radioScoringWeights() const;
    TrackScorer::RadioSessionDecay radioSessionDecay() const;
    void applyActiveRadioProfile();
    QHash<QString, TrackScorer::Affinity> buildRadioAffinities(const QHash<QString, QString> &resolvedSongKeys) const;
    Track bestRadioCopyForPick(const Track &track, const QSet<QString> &blockedPaths) const;
    Track resolveRadioPick(const QString &path, const QSet<QString> &blockedPaths) const;
    void installRadioProvider(bool markPicksAsRadio);
    void recordRadioPicks(const QVector<Track> &picks);
    void saveRadioSessionState();
    void clearRadioSessionState();
    void maybeRestoreRadioSession();
    // Maintains the ambient, anchorless radio provider used only by
    // ShuffleMode::Radio. Explicit Start Radio owns m_radioSession while
    // PlayerCore::radioActive() is true and takes precedence.
    void syncRadioShuffleSession();

    // Generates up to `count` fresh picks from a snapshot of the rolling
    // context on a worker, resolves the chosen paths on the GUI thread, then
    // queues them via PlayerCore::injectTracks. Stale snapshots are discarded.
    void appendRadioBatch(int count);
    void finishSeededRadioStart(const QString &seedPath, quint64 requestId);
    void setRadioLoading(bool loading);
    // Hooked off PlayerCore::currentIndexChanged: while radio is active and
    // batching (batchSize > 1), keeps the configured number of rows queued
    // ahead of the current index so the recommendation stream stays padded.
    void maybeTopUpRadioQueue();
    std::unique_ptr<Database>          m_database;
    std::unique_ptr<PlaylistDatabase>  m_playlistDb;
    std::unique_ptr<SettingsStore>     m_state;
    std::unique_ptr<ArtworkCache>      m_artworkCache;
    std::unique_ptr<FeatureStore>      m_features;
    std::unique_ptr<ListenHistoryStore> m_listenHistory;
    RadioProfileStore                  m_radioProfileStore;
    std::unique_ptr<RadioSession>      m_radioSession;
    PlayerCore       *m_player = nullptr;
    PlaybackBackend  *m_playback = nullptr;
    ListenTracker    *m_listenTracker = nullptr;
    PlayEventRecorder *m_playEventRecorder = nullptr;
    QThread          *m_listenBrainzThread = nullptr;
    ListenBrainzScrobbler *m_listenBrainzScrobbler = nullptr;
    QThread          *m_lastFmThread = nullptr;
    LastFmScrobbler  *m_lastFmScrobbler = nullptr;
    QThread          *m_scrobbleBackfillThread = nullptr;
    ScrobbleBackfill *m_scrobbleBackfill = nullptr;
    // Main-thread mirror of the worker's busy state, so the IPC trigger can
    // report "already running" without a cross-thread query.
    bool              m_backfillRunning = false;
    BackfillStatus    m_backfillStatus;
    MprisService     *m_mpris = nullptr;
    IpcServer        *m_ipc = nullptr;
    QSystemTrayIcon  *m_tray = nullptr;
    MainWindow       *m_window = nullptr;
    bool              m_quitting = false;
    bool              m_resumeDone = false;
    bool              m_trayAlwaysVisible = false;
    // Track-start attribution for play events. currentIndexChanged(index,
    // userInitiated) always precedes the matching currentTrackChanged, and
    // aboutToInjectLibraryTrack precedes it for library-shuffle injections, so
    // these carry the attribution forward to the currentTrackChanged handler.
    bool              m_nextStartUserInitiated = false;
    bool              m_nextStartInjected = false;
    // Set before Previous or a direct queue-row jump. The next track start
    // finalizes the outgoing spin as navigation rather than rejection.
    bool              m_nextTransitionIsNavigation = false;
    QString           m_currentPlayingSource;
    // Paths this radio session has handed out (batch appends + JIT provider
    // picks alike), for telemetry (source "radio") and for the explicit queue
    // refresh action to find unplayed radio rows. User queue removals prune paths from
    // this set too so later playback attribution stays aligned with the queue.
    // Cleared on startRadio/stopRadio.
    QSet<QString>     m_radioPickPaths;
    // Paths that have become current in this app session. Queue-removal
    // telemetry uses path-level granularity: if the same path was heard once,
    // later duplicate rows are not counted as unheard.
    QSet<QString>     m_queueHeardPaths;
    // Cached "radio.batchSize" setting (1 = pure JIT, matching pre-batch
    // behaviour exactly); reloaded at each startRadio, otherwise updated live by
    // setRadioBatchSize.
    int               m_radioBatchSize = 15;
    int               m_radioRefillThreshold = 5;
    // Re-entrancy / setup guard for the batch-append paths. Also held across
    // startRadio's own initial clearAll()+appendAndPlay(seed)+appendRadioBatch
    // sequence: appendAndPlay(seed) fires currentIndexChanged synchronously
    // with a 1-row queue, which would otherwise trip maybeTopUpRadioQueue and
    // double-append ahead of the deliberate initial batch.
    bool              m_radioTopUpInProgress = false;
    bool              m_radioLoading = false;
    quint64           m_radioRequestId = 0;
    quint64           m_radioSessionRevision = 0;
    // "Adventurous" boost state -- see setRadioAdventurous's doc comment.
    bool              m_radioAdventurous = false;
    bool              m_radioShuffleSessionActive = false;
    // Restoring playback modes happens before restoring the saved queue. Keep
    // the initial anchorless pool usable, then rebuild it once the silent queue
    // presentation supplies the current library track.
    bool              m_radioShuffleAwaitingInitialTrack = false;
    bool              m_radioRestoreDone = false;
    QString           m_radioSessionKind;
    QString           m_radioSessionSeedPath;
    QString           m_radioSessionArtistName;
    int               m_radioSessionExploration = 30;
    TrackScorer::Weights m_radioSessionWeights;
    TrackScorer::RadioSessionDecay m_radioSessionDecay = TrackScorer::defaultSessionDecay();
};
