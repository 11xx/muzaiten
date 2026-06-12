#pragma once

#include <QMainWindow>
#include <QStringList>

#include "core/Track.h"
#include "core/ScanRoot.h"
#include "playback/PlaybackTypes.h"

#include <memory>

class QJsonObject;
struct SavedQueuePlaylistEntry;
class Database;
class PlaylistDatabase;
class PlaylistView;
class SettingsStore;
class AlbumGrid;
class ArtistSidebar;
class FileExplorerView;
class QCloseEvent;
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
class QSystemTrayIcon;
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

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

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
    void saveMainWindowViewSettings();
    void saveAllViewSettings();
    void resetViewPreferences();
    void resetPanelOrder();
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
    QJsonObject queueSnapshotObject(const QString &name) const;
    QVector<Track> tracksFromSnapshotObject(const QJsonObject &snapshot) const;
    QJsonObject loadQueueSnapshotsRoot() const;
    void saveQueueSnapshotsRoot(const QJsonObject &root);
    QVector<SavedQueuePlaylistEntry> savedQueuePlaylistEntries() const;
    QJsonObject queueSnapshotById(const QString &id) const;
    void refreshSavedQueuePlaylistEntries();
    void playQueueSnapshotById(const QString &id, int startIndex);
    void addQueueSnapshotByIdToQueue(const QString &id);
    void playNextQueueSnapshotById(const QString &id);
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
    void configurePlaybackProfile();
    void configurePlaybackResume();
    void configureLinkRoots();
    void configureSourceDirectories();
    void findTrackFile(const Track &track);
    void showTrackProperties(const Track &track);
    void configureTrackInfoPanel();
    void configureAlbumArtResolution();
    void configureSearchRanking();
    void configureKeybindings();
    void loadSearchRankingConfig();
    void jumpToTrackInfoArtist(const QString &artistName);
    void jumpToTrackInfoAlbum(const QString &artistName, const QString &albumTitle);
    void configureMpdSource();
    void importMpdLibraryMetadata();
    QString mpdMusicDirectory() const;
    void configureListenBrainz();
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
    void notifyScrobblersTrackStarted(const Track &track);
    void resumeScrobblers(const Track &track, qint64 elapsedMs, bool playing);
    void appendAndPlayTrack(const Track &track);
    void playNextTracks(const QVector<Track> &tracks);
    void addTracksToQueue(const QVector<Track> &tracks);
    void moveQueueRows(const QVector<int> &rows, int destinationRow);
    void removeQueueRows(const QVector<int> &rows);
    void clearQueue();
    void clearPlayNextPriority();
    void patchQueueTracksFromMetadata(const QVector<Track> &tracks);
    void refreshPlayNextRange();
    // Handler for PlayerCore::queueChanged: re-derives and pushes every piece
    // of queue-dependent UI/persisted state (queue store snapshot, queue
    // identity, panel search, saved queue state) from the player's canonical
    // queue triple.
    void syncQueueState();
    void playAlbumNow(const QString &albumTitle);
    void playAlbumsNow(const QStringList &albumTitles);
    void playAlbumsReplacingQueue(const QStringList &albumTitles);
    void playNextAlbum(const QString &albumTitle);
    void addAlbumToQueue(const QString &albumTitle);
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
    // explicitJump=true marks a user-activated jump (clicking a queue row). A
    // backward explicit jump turns the skipped-over tracks into the play-next
    // region; forward jumps and sequential prev/next leave it alone.
    void playQueueIndex(int index, bool notifyScrobbler = true, bool startPaused = false, bool explicitJump = false);
    void playPreviousTrack();
    void playNextTrack();
    void togglePlayback();
    void playFromMpris();
    void setVolumeFromMpris(double volume0To1);
    void seekRelativeFromMpris(qint64 offsetMs);
    void setupIpcServer();
    void setupTrayIcon();
    void toggleWindowVisible();
    QJsonObject handleIpcCommand(const QString &command, const QJsonObject &args);
    QJsonObject ipcStatus() const;
    void updateMprisCapabilities();
    void updatePlaybackPosition();
    void startScan(const QString &rootPath);
    void startScan(const QString &rootPath, int scanRootId);
    void scanEnabledSourceDirectories();
    void forceRescanEnabledSourceDirectories();
    void scanSourceRoots(const QVector<ScanRoot> &roots);
    void startNextQueuedSourceScan();
    void cancelScan();
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
    PlayerCore *m_player = nullptr;
    PlaybackBackend *m_playback = nullptr;  // m_player->backend(), kept for read/connect convenience
    std::unique_ptr<Database> m_database;
    std::unique_ptr<PlaylistDatabase> m_playlistDb;
    std::unique_ptr<SettingsStore> m_state;
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
    std::unique_ptr<ArtworkCache> m_artworkCache;
    quint64 m_currentArtGeneration = 0;
    std::unique_ptr<ListenHistoryStore> m_listenHistory;
    ListenTracker *m_listenTracker = nullptr;
    QThread *m_listenBrainzThread = nullptr;
    ListenBrainzScrobbler *m_listenBrainzScrobbler = nullptr;
    QThread *m_lastFmThread = nullptr;
    LastFmScrobbler *m_lastFmScrobbler = nullptr;
    QThread *m_mpdImportThread = nullptr;
    MpdImportWorker *m_mpdImportWorker = nullptr;
    MprisService *m_mpris = nullptr;
    IpcServer *m_ipc = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    bool m_quitRequested = false;
    bool m_ratingTagSyncRunning = false;
    bool m_ratingTagSyncPending = false;
    QVector<ScanRoot> m_pendingScanRoots;
    int m_activeScanRootId = 0;
    QString m_activeScanRootPath;
    QTimer *m_playbackStateSaveTimer = nullptr;
    bool m_savePlaybackPositionEnabled = true;
    bool m_restorePlaybackStateEnabled = true;
    qint64 m_lastSavedPlaybackPositionMs = -1;
    QString m_lastSavedPlaybackTrackPath;
    QString m_lastSavedPlaybackState;
};
