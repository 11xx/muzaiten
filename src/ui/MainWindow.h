#pragma once

#include <QMainWindow>
#include <QSet>
#include <QStringList>

#include "core/Playlist.h"
#include "core/Track.h"
#include "core/ScanRoot.h"
#include "playback/PlaybackTypes.h"

#include <functional>
#include <memory>

class AppCore;
class QJsonObject;
struct SavedQueuePlaylistEntry;
class Database;
class PlaylistDatabase;
class PlaylistImportDialog;
class PlaylistView;
struct PlaylistImportMatch;
class PlaylistDropImportWorker;
namespace PlaylistImport { struct ImportHeader; }
class SettingsStore;
class AlbumGrid;
class ArtistSidebar;
class FileExplorerView;
class QCloseEvent;
class QEvent;
class QImage;
class ListenBrainzScrobbler;
class LastFmScrobbler;
class ListenHistoryStore;
class ListenTracker;
class MprisService;
class IpcServer;
class PlayerBar;
class PlayerCore;
class PanelSearchController;
class PlaybackBackend;
class QTimer;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QSplitter;
class QThread;
class MpdImportWorker;
class QueueScreen;
class QueueStore;
class RightSidebar;
class TrackTable;
class ScanPipeline;
class ArtworkCache;
class SearchView;

enum class LibrarySource { Local, Mpd };
enum class MainView { LibraryPanels, LibraryFileExplorer, FreeRoamFileExplorer, Search, Queue, Playlist };
enum class QueueAddMode { Append, PlayNext };
// Outcome of the warning shown when adding to a queue that is mirroring a
// playlist: append to both queue and playlist, append to the queue only
// (temporary, saved nowhere), or do nothing.
enum class PlaylistMirrorChoice { AddToPlaylist, QueueOnly, Cancel };

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(AppCore *core, QWidget *parent = nullptr);
    ~MainWindow() override;

    // Called by AppCore::releaseWindow() to snapshot ephemeral view state
    // before the widget tree is destroyed.
    void persistViewState();
    bool showDemoArtist(const QString &artistName);
    bool showDemoAlbum(const QString &artistName, const QString &albumTitle, QString *error = nullptr);
    bool showDemoNowPlaying(const QString &query, bool playing, double positionRatio, QString *error = nullptr);
    // AppCore keeps the widget tree alive while it owns a PipeWire takeover, so
    // the release timers can restore the card even when the window is tray-hidden.
    bool hasTakenOverDsdDevice() const { return !m_takenOverDsdDevice.isEmpty(); }

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void openLibraryFolder();
    void loadExistingLibrary();
    void refreshArtists();
    void selectArtist(const QString &artistName);
    void showArtist(const QString &artistName, bool forceReload, bool clearAlbumSelectionOnArtistChange);
    void selectAlbumFilter(const QString &albumTitle);
    void narrowAlbumFilters(const QStringList &albumTitles);
    void setAlbumNarrowFromGrid(const QStringList &albumTitles);
    void clearAlbumFilter();
    void refreshAlbumGrid(bool freshLoad = false);
    void refreshTrackTable();
    void applyTrackRating(const Track &track, int rating0To100);
    void applyAlbumRating(const QString &albumArtistName, const QString &albumTitle, int rating0To100);
    void startRatingTagSync(const QVector<Track> &tracks, int scope);
    void schedulePendingRatingTagSync();
    void syncCurrentTrackRatingTags();
    void syncCurrentArtistRatingTags();
    void syncAllSavedRatingTags();
    void retryPendingRatingTags();
    void loadViewSettings();
    void saveTrackTableViewSettings();
    void saveAlbumGridViewSettings();
    void saveArtistSidebarViewSettings();
    void saveRightSidebarViewSettings();
    void saveQueueScreenViewSettings();
    void savePlaylistViewSettings();
    void saveMainWindowViewSettings(bool captureSplitterSizes = false);
    void saveAllViewSettings();
    void resetViewPreferences();
    void resetPanelOrder();
    void restylePanelBorders();
    void openPanelOrderDialog();
    void switchMainView(MainView view);
    void toggleFileExplorerView();
    void jumpToPlayingSong();
    void revealTrackInLibrary(const Track &track);
    void setLibraryExplorerDirectory(const QString &path);
    void setFreeRoamDirectory(const QString &path);
    void refreshLibraryFileExplorer();
    void loadQueueState();
    void saveQueueState();
    void scheduleQueueStateSave(bool immediate = false);
    QJsonObject queueSnapshotObject(const QString &name) const;
    QVector<Track> tracksFromSnapshotObject(const QJsonObject &snapshot) const;
    QJsonObject loadQueueSnapshotsRoot() const;
    void saveQueueSnapshotsRoot(const QJsonObject &root);
    QVector<SavedQueuePlaylistEntry> savedQueuePlaylistEntries() const;
    QJsonObject queueSnapshotByKey(const QString &keyOrId) const;
    void refreshSavedQueuePlaylistEntries();
    void playQueueSnapshotById(const QString &id, int startIndex);
    void addQueueSnapshotByIdToQueue(const QString &id);
    void playNextQueueSnapshotById(const QString &id);
    void deleteQueueSnapshotById(const QString &id);
    void configureSavedQueueLimit();
    void ensureCurrentQueueIdentity();
    bool currentQueueBacklogEligible() const;
    void pushCurrentQueueToBacklog(const QString &name);
    void adoptQueueSnapshot(const QJsonObject &snapshot, const QVector<Track> &tracks, int playIndex);
    void prepareQueueForTrackAddition(const QVector<Track> &tracks);
    void appendTracksToCurrentPlaylist(const QVector<Track> &tracks);
    void markQueueAsSpontaneous(const QString &id = {});
    void loadExplorerState();
    void saveExplorerState();
    void applySharedTableSettings();
    void loadPlaybackProfile();
    void savePlaybackProfile();
    void loadPlaybackResumeSettings();
    void savePlaybackResumeSettings();
    void schedulePlaybackStateSave(bool immediate = false);
    void savePlaybackState(bool force = false);
    void restoreSavedPlaybackState();
    // Reopen the current queue entry and seek to positionMs without telling the
    // scrobblers a new track started (the listen session resumes at the restored
    // position). settleDelayMs waits for the backend to preroll before seeking.
    void resumePlaybackAt(int queueIndex, qint64 positionMs, bool playing, int settleDelayMs);
    void configurePlaybackProfile();
    // Shared output-transition path used by the profile dialog, the Release-device
    // action and the idle auto-release timer: persist `next`, free any held card
    // and rebuild the sink, resuming the current track. `manuallyReleasedHw`/
    // `manualSinkNodeId` carry a hand-back a caller (the dialog) already performed.
    void applyProfileTransition(const PlaybackProfile &previous, const PlaybackProfile &next,
                                const QString &manuallyReleasedHw = {}, int manualSinkNodeId = -1);
    // Single device target: hand an exclusively-held card (bit-perfect, or a DSD
    // takeover) back to PipeWire when the new profile stops wanting it that way,
    // so a shared sink aimed at the same card isn't left silent. Returns the hw
    // path it released (empty if it released nothing).
    QString releaseDeviceForProfileSwitch(const PlaybackProfile &previous, const PlaybackProfile &next,
                                          int *previousSinkNodeId);
    // Build the new output sink and restore playback, but first wait (poll, ~10s
    // budget) for any cross-profile device-ownership change to settle — a card
    // takes a couple of seconds to change hands, and building the sink onto a
    // half-transitioned device plays silently or fails to preroll.
    void applyOutputProfile(const PlaybackProfile &next, const QString &releasedHw,
                            int previousSinkNodeId, int queueIndex, qint64 positionMs,
                            bool wasActive, bool wasPlaying);
    // Poll until the requested cross-server hand-off is truly usable (a new
    // shared sink after release, or no shared sink before direct ALSA open), or
    // the ~10s budget elapses. Calls done immediately when already ready.
    void waitForDeviceOwnership(const QString &hw, bool wantHeld, int previousSinkNodeId,
                                std::function<void()> done);
    void configurePlaybackResume();
    // A shared-mode PCM track can't reach a card we hold off for native DSD;
    // hand the card back, wait for PipeWire to rebuild its sink, then restart the
    // track so it routes to the live device instead of silence. No-op in
    // bit-perfect (PCM plays straight to the ALSA device) or for a DSD follow-up.
    void releaseDsdDeviceForPcmTrack(const Track &track);
    void showDsdTakeoverPrompt(const Track &track, const QString &device);
    void resolveDsdTakeoverPrompt(bool accepted);
    void releaseTakenOverDsdDevice();
    // Menu-driven "Release device": hands back whatever card is held — a tracked
    // DSD takeover, or a bit-perfect profile's exclusive device (switching output
    // to shared so it won't immediately re-grab).
    void releaseHeldOutputDevice();
    bool canReleaseOutputDevice() const;
    void scheduleHeldDeviceRelease(int delayMs);
    void updateDsdTakeoverPromptText();
    void configureLinkRoots();
    void configureSourceDirectories();
    void findTrackFile(const Track &track);
    void showTrackProperties(const Track &track);
    void configureTrackInfoPanel();
    void configureAlbumArtResolution();
    void configurePlaylistMetadataDisplay();
    void configureSearchRanking();
    void configureKeybindings();
    void loadSearchRankingConfig();
    void loadPlaybackModes();
    void jumpToTrackInfoArtist(const QString &artistName);
    void jumpToTrackInfoAlbum(const QString &artistName, const QString &albumTitle);
    void configureMpdSource();
    void importMpdLibraryMetadata();
    QString mpdMusicDirectory() const;
    void configureListenBrainz();
    void showListeningHistory();
    void clearScrobbleBacklog(const QString &service);
    void triggerScrobbleUpload(const QString &service);
    void setListenBrainzEnabled(bool enabled);
    void setListenBrainzToken();
    void configureLastFm();
    void setLastFmEnabled(bool enabled);
    void setScrobbleOffline(bool offline);
    bool scrobbleOffline() const;
    QString listenHistoryPath() const;
    void showLastFmSettings();
    QString lastFmApiKey() const;
    QString lastFmSharedSecret() const;
    bool hasDefaultLastFmCredentials() const;
    void onLibrarySourceChanged(int index);
    void presentTrack(const Track &track, bool notifyScrobbler = true);
    void presentCurrentTrackUpdate(const Track &track);
    void clearPresentedTrack();
    void onPlayerIndexChanged(int index, bool userInitiated);
    void appendAndPlayTrack(const Track &track);
    void playNextTracks(const QVector<Track> &tracks);
    void addTracksToQueue(const QVector<Track> &tracks);
    // Menu-driven queue adds funnel through here so that, when the queue is
    // mirroring a playlist, the user is warned that the tracks would also be
    // saved to the playlist (with an option to add them to the queue only).
    // temporary=true is the explicit "(don't save to playlist)" path: it skips
    // the prompt and never mirrors. The raw playNextTracks/addTracksToQueue above
    // stay mirror-by-default and are reserved for internal, non-menu callers.
    void enqueueTracksFromMenu(const QVector<Track> &tracks, QueueAddMode mode, bool temporary);
    bool queueIsPlaylistSourced() const;
    void unlinkQueueFromPlaylist();
    PlaylistMirrorChoice promptPlaylistMirror(int trackCount);
    void moveQueueRows(const QVector<int> &rows, int destinationRow);
    void removeQueueRows(const QVector<int> &rows);
    void clearQueue();
    void clearPlayNextPriority();
    void patchQueueTracksFromMetadata(const QVector<Track> &tracks);
    void patchQueueRows(const QVector<int> &rows);
    void refreshPlayNextRange();
    // Handler for PlayerCore::queueChanged: re-derives and pushes every piece
    // of queue-dependent UI/persisted state (queue store snapshot, queue
    // identity, panel search, saved queue state) from the player's canonical
    // queue triple.
    void syncQueueState();
    // Pushes queueIsPlaylistSourced() to every view + the PlayerBar merge action.
    // Called from syncQueueState and from the queue-replace paths (play/restore),
    // which use resetQueue() and so never emit queueChanged.
    void refreshQueueSourceDependentUi();
    // Tells the playlist view which track is playing and which playlist (if any)
    // feeds the queue, so it can tint the matching tracklist row.
    void refreshPlaylistNowPlaying();
    void playAlbumNow(const QString &albumTitle);
    void playAlbumsNow(const QStringList &albumTitles);
    void playAlbumsReplacingQueue(const QStringList &albumTitles);
    void playNextAlbum(const QString &albumTitle);
    void addAlbumToQueue(const QString &albumTitle);
    void playNextAlbumTemporary(const QString &albumTitle);
    void addAlbumToQueueTemporary(const QString &albumTitle);
    QVector<Track> tracksForAlbumTitle(const QString &albumTitle) const;
    // Queue snapshots (stored in state.sqlite, distinct from library playlists).
    // Spontaneous queues keep a stable id and are moved through a short backlog
    // when displaced, so restoring/mutating one does not create duplicate queue
    // identities.
    void replaceQueueWithTracks(const QVector<Track> &tracks, int playIndex,
                                const QString &sourceKind = {},
                                qint64 sourcePlaylistId = 0,
                                const QString &sourceName = {});
    void snapshotCurrentQueueAsPrevious();
    void restorePreviousQueue();
    void saveCurrentQueueAs();
    void mergeSavedQueueViaPlayNext();
    int savedQueueLimitSetting() const;
    // explicitJump=true marks a user-activated jump (clicking a queue row). A
    // backward explicit jump turns the skipped-over tracks into the play-next
    // region; forward jumps and sequential prev/next leave it alone.
    void playQueueIndex(int index, bool notifyScrobbler = true, bool startPaused = false, bool explicitJump = false);
    void playPreviousTrack();
    void playNextTrack();
    void togglePlayback();
    void playFromMpris();
    void setVolumeFromMpris(double volume0To1);
    void applyPlayerVolume(double volume0To1);
    void seekRelativeFromMpris(qint64 offsetMs);
    void updatePlaybackPosition();
    void startScan(const QString &rootPath);
    void startScan(const QString &rootPath, int scanRootId);
    void scanEnabledSourceDirectories();
    void forceRescanEnabledSourceDirectories();
    void scanSourceRoots(const QVector<ScanRoot> &roots);
    void startNextQueuedSourceScan();
    void cancelScan();
    // Free the bit-perfect target device from PipeWire and retry playback.
    void attemptDeviceTakeover();
    void ingestScanBatch(const QVector<Track> &tracks);
    void ingestEnumeratedPlaceholders(const QVector<Track> &tracks);
    // Lazy background metadata fill of placeholder rows.
    void pumpMetadataFill();
    void startMetadataFill(const QStringList &paths);
    void finishMetadataFill(qint64 enumerated, qint64 indexed, qint64 skipped, bool canceled);
    void ensureDirectoryScanned(const QString &directory);
    QStringList nextFillChunk();
    int scanProfileSetting() const;  // 0 Background, 1 Balanced, 2 Turbo
    bool guessedPlaceholdersEnabled() const;  // scan.guessedPlaceholders setting (default on)
    void scheduleIncrementalRefresh();  // throttled browse/explorer refresh during ingest
    void flushIncrementalRefresh();     // force a refresh now and stop the throttle
    void ensureIngestSession();
    void endIngestSessionIfIdle();
    void markScannedTracksMissing(const QStringList &paths);
    void removeMissingTracks();
    void finishScan(qint64 enumerated, qint64 indexed, qint64 skipped, bool canceled);
    void onArtworkReady(const QString &token, const QImage &image, quint64 generation);
    void onArtworkMissing(const QString &token, quint64 generation);
    QString databasePath() const;
    QString playlistDatabasePath() const;
    QVector<Track> tracksForPaths(const QStringList &paths) const;
    void openPlaylistAddModal(qint64 playlistId);
    void openPlaylistImportDialog(qint64 playlistId);
    void importAsNewPlaylist();
    void importDroppedFiles(const QStringList &paths);
    void commitImportResults(qint64 playlistId, const PlaylistImportDialog &dialog);
    void commitImportItems(qint64 playlistId, const QVector<PlaylistImportMatch> &matches,
                           const PlaylistImport::ImportHeader &header,
                           const QHash<int, QString> &resolved);
    // One auto-resolved playlist item from a matcher outcome (no triage) — used for
    // both the bulk commit and the streamed live drop-import.
    PlaylistItem playlistItemFromImportMatch(const PlaylistImportMatch &match);
    // Background, interruptible drop-import: creates placeholder playlists at once,
    // then fills each live as the matcher streams results.
    void cancelDropImport();
    void onDropImportItemMatched(qint64 playlistId, const PlaylistImportMatch &match);
    void onDropImportPlaylistFinished(qint64 playlistId);
    void finishDropImport(bool interrupted);
    QString uniquePlaylistName(const QString &base) const;
    void openPlaylistEditModal(qint64 playlistId, qint64 itemId, const QString &query);
    void openAddToPlaylistDialog(const QVector<Track> &tracks);
    QString resolvedReadPathForTrack(const Track &track) const;
    void rememberTrackTableViewState();
    void restoreTrackTableViewState();
    void updateCurrentAlbumArt();
    void applyTrackInfoPaneVisible(bool visible);
    void applyCompactMenu(bool compact);
    void rememberCurrentSourceSelection();
    void restoreCurrentSourceSelection();

    ArtistSidebar *m_artistSidebar = nullptr;
    QStackedWidget *m_mainStack = nullptr;
    QSplitter *m_rootSplitter = nullptr;
    QSplitter *m_centerSplitter = nullptr;
    PlayerBar *m_playerBar = nullptr;
    AlbumGrid *m_albumGrid = nullptr;
    TrackTable *m_trackTable = nullptr;
    RightSidebar *m_rightSidebar = nullptr;
    QueueScreen *m_queueScreen = nullptr;
    QueueStore *m_queueStore = nullptr;
    FileExplorerView *m_libraryFileExplorer = nullptr;
    FileExplorerView *m_freeRoamFileExplorer = nullptr;
    SearchView       *m_searchView = nullptr;
    PlaylistView     *m_playlistView = nullptr;
    PanelSearchController *m_panelSearch = nullptr;
    QProgressBar *m_scanProgress = nullptr;
    QPushButton *m_stopScanButton = nullptr;
    // Offered in the status bar only when bit-perfect playback fails because
    // PipeWire is holding the target device; click frees it and retries.
    QPushButton *m_takeOverDeviceButton = nullptr;
    QTimer *m_dsdTakeoverPromptTimer = nullptr;
    QTimer *m_takenOverDsdReleaseTimer = nullptr;
    bool m_dsdTakeoverPromptActive = false;
    int m_dsdTakeoverSecondsRemaining = 0;
    QString m_pendingDsdTakeoverDevice;
    QString m_takenOverDsdDevice;
    int m_takenOverDsdRestoreProfile = -1;
    // Core engine pointers — borrowed from AppCore (non-owning)
    AppCore        *m_core = nullptr;
    PlayerCore     *m_player = nullptr;
    PlaybackBackend *m_playback = nullptr;  // cached m_core->backend(); sole backend accessor used below
    Database       *m_database = nullptr;
    PlaylistDatabase *m_playlistDb = nullptr;
    SettingsStore  *m_state = nullptr;
    QString m_currentArtist;
    QString m_selectedAlbumTitle;
    QStringList m_selectedAlbumTitles;
    QString m_loadedPanelArtist;
    QString m_loadedPanelAlbumFilter;
    QString m_localArtist;
    QString m_localAlbumTitle;
    QString m_mpdArtist;
    QString m_mpdAlbumTitle;
    PlaybackProfile m_playbackProfile;
    QString m_queueId;
    QString m_queueSourceKind = QStringLiteral("queue");
    qint64 m_queueSourcePlaylistId = 0;
    QString m_queueSourceName;
    // Set transiently around a queue add to tell prepareQueueForTrackAddition to
    // skip mirroring the added tracks into the source playlist (the "(don't save
    // to playlist)" / "queue only" path).
    bool m_suppressPlaylistMirror = false;
    // Playlist id for which the user ticked "don't ask again" in the mirror
    // warning. Auto-expires when a different playlist (or no playlist) backs the
    // queue, since it is compared against m_queueSourcePlaylistId.
    qint64 m_mirrorPromptSuppressedForPlaylist = 0;
    // The choice the user locked in alongside "don't ask again" — replayed for
    // every later add while that playlist backs the queue, so ticking the box on
    // "Queue only" keeps queueing rather than silently switching to saving.
    PlaylistMirrorChoice m_rememberedMirrorChoice = PlaylistMirrorChoice::AddToPlaylist;
    int m_trackSortColumn = 0;
    Qt::SortOrder m_trackSortOrder = Qt::AscendingOrder;
    int m_trackScrollValue = 0;
    LibrarySource m_librarySource = LibrarySource::Local;
    LibrarySource m_loadedPanelSource = LibrarySource::Local;
    MainView m_mainView = MainView::LibraryPanels;
    QString m_libraryExplorerDirectory;
    QString m_freeRoamDirectory;
    QThread *m_scanThread = nullptr;
    ScanPipeline *m_scanPipeline = nullptr;
    bool m_forceFullRescan = false;
    // Background metadata fill (lazy tag read of enumerated-only placeholders).
    QThread *m_fillThread = nullptr;
    ScanPipeline *m_fillPipeline = nullptr;
    QString m_priorityFillDir;       // directory to fill next (on-access prioritization)
    bool m_ingestSessionActive = false;  // spans scan + fill for the artist/album id cache
    QTimer *m_incrementalRefreshTimer = nullptr;  // throttles browse/explorer refresh during ingest
    bool m_incrementalRefreshDirty = false;
    ArtworkCache  *m_artworkCache = nullptr;
    quint64 m_currentArtGeneration = 0;
    ListenHistoryStore *m_listenHistory = nullptr;
    ListenTracker *m_listenTracker = nullptr;
    ListenBrainzScrobbler *m_listenBrainzScrobbler = nullptr;
    LastFmScrobbler *m_lastFmScrobbler = nullptr;
    QThread *m_mpdImportThread = nullptr;
    MpdImportWorker *m_mpdImportWorker = nullptr;
    QThread *m_dropImportThread = nullptr;
    PlaylistDropImportWorker *m_dropImportWorker = nullptr;
    QSet<qint64> m_dropImportPlaylists;  // placeholders still being filled
    MprisService *m_mpris = nullptr;
    IpcServer *m_ipc = nullptr;
    bool m_ratingTagSyncRunning = false;
    bool m_ratingTagSyncPending = false;
    QVector<ScanRoot> m_pendingScanRoots;
    int m_activeScanRootId = 0;
    QString m_activeScanRootPath;
    QTimer *m_playbackStateSaveTimer = nullptr;
    QTimer *m_queueStateSaveTimer = nullptr;
    bool m_savePlaybackPositionEnabled = true;
    bool m_restorePlaybackStateEnabled = true;
    qint64 m_lastSavedPlaybackPositionMs = -1;
    QString m_lastSavedPlaybackTrackPath;
    QString m_lastSavedPlaybackState;
    // Last position/track seen while playback was healthy (Playing/Paused), used
    // to resume after a bit-perfect takeover that left the pipeline in Error.
    QString m_lastHealthyTrackPath;
    qint64 m_lastHealthyPositionMs = 0;
    // Exact transport snapshot for a shared → BP switch that initially fails
    // because PipeWire still owns the card. This outlives the backend Error
    // state, whose zero position must never overwrite the requested resume.
    bool m_profileTakeoverResumePending = false;
    QString m_profileTakeoverTrackPath;
    qint64 m_profileTakeoverPositionMs = 0;
    bool m_profileTakeoverWasPlaying = false;
    bool m_loadingViewSettings = false;
};
