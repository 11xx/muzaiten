#pragma once

#include <QMainWindow>

#include "core/Track.h"
#include "core/ScanRoot.h"
#include "playback/PlaybackTypes.h"

#include <memory>

class Database;
class AlbumGrid;
class ArtistSidebar;
class FileExplorerView;
class QCloseEvent;
class ListenBrainzScrobbler;
class LastFmScrobbler;
class MprisService;
class PlayerBar;
class PlaybackBackend;
class QTimer;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QSplitter;
class QThread;
class MpdImportWorker;
class RightSidebar;
class TrackTable;
class ScanWorker;

enum class LibrarySource { Local, Mpd };
enum class MainView { LibraryPanels, LibraryFileExplorer, FreeRoamFileExplorer };

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
    void selectAlbumFilter(const QString &albumTitle);
    void refreshAlbumGrid();
    void refreshTrackTable();
    void applyTrackRating(const Track &track, int rating0To100);
    void applyAlbumRating(const QString &albumArtistName, const QString &albumTitle, int rating0To100);
    void startRatingTagSync(const QVector<Track> &tracks, int scope);
    void syncCurrentTrackRatingTags();
    void syncCurrentArtistRatingTags();
    void syncAllSavedRatingTags();
    void retryPendingRatingTags();
    void loadViewSettings();
    void saveTrackTableViewSettings();
    void saveAlbumGridViewSettings();
    void saveArtistSidebarViewSettings();
    void saveRightSidebarViewSettings();
    void saveMainWindowViewSettings();
    void switchMainView(MainView view);
    void toggleFileExplorerView();
    void setLibraryExplorerDirectory(const QString &path);
    void setFreeRoamDirectory(const QString &path);
    void refreshLibraryFileExplorer();
    void loadQueueState();
    void saveQueueState();
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
    void configureTrackInfoPanel();
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
    void configureLastFmAccount();
    void startLastFmAuthentication();
    void finishLastFmAuthentication();
    void onLibrarySourceChanged(int index);
    void playTrack(const Track &track);
    void presentTrack(const Track &track, bool notifyScrobbler = true);
    void appendAndPlayTrack(const Track &track);
    void playNextTracks(const QVector<Track> &tracks);
    void addTracksToQueue(const QVector<Track> &tracks);
    void moveQueueRows(const QVector<int> &rows, int destinationRow);
    void removeQueueRows(const QVector<int> &rows);
    void clearQueue();
    void playNextAlbum(const QString &albumTitle);
    void addAlbumToQueue(const QString &albumTitle);
    void playQueueIndex(int index);
    void playPreviousTrack();
    void playNextTrack();
    void togglePlayback();
    void playFromMpris();
    void setVolumeFromMpris(double volume0To1);
    void seekRelativeFromMpris(qint64 offsetMs);
    void updateMprisCapabilities();
    void updatePlaybackPosition();
    void prepareNextQueueTrack();
    void advanceAfterPreparedTransition();
    void startScan(const QString &rootPath);
    void startScan(const QString &rootPath, int scanRootId);
    void scanEnabledSourceDirectories();
    void scanSourceRoots(const QVector<ScanRoot> &roots);
    void startNextQueuedSourceScan();
    void cancelScan();
    void ingestScanBatch(const QVector<Track> &tracks);
    void finishScan(qint64 visitedFiles, qint64 indexedTracks, bool canceled);
    QString databasePath() const;
    QString cacheRoot() const;
    QString mpdCacheRoot() const;
    QString stateRoot() const;
    bool useDevState() const;
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
    FileExplorerView *m_libraryFileExplorer = nullptr;
    FileExplorerView *m_freeRoamFileExplorer = nullptr;
    QProgressBar *m_scanProgress = nullptr;
    QPushButton *m_stopScanButton = nullptr;
    PlaybackBackend *m_playback = nullptr;
    std::unique_ptr<Database> m_database;
    QString m_currentArtist;
    QString m_selectedAlbumTitle;
    QString m_localArtist;
    QString m_localAlbumTitle;
    QString m_mpdArtist;
    QString m_mpdAlbumTitle;
    Track m_currentTrack;
    PlaybackProfile m_playbackProfile;
    QVector<Track> m_queue;
    int m_queueIndex = -1;
    int m_playNextInsertIndex = -1;
    int m_trackSortColumn = 0;
    Qt::SortOrder m_trackSortOrder = Qt::AscendingOrder;
    int m_trackScrollValue = 0;
    LibrarySource m_librarySource = LibrarySource::Local;
    MainView m_mainView = MainView::LibraryPanels;
    QString m_libraryExplorerDirectory;
    QString m_freeRoamDirectory;
    QThread *m_scanThread = nullptr;
    ScanWorker *m_scanWorker = nullptr;
    QThread *m_listenBrainzThread = nullptr;
    ListenBrainzScrobbler *m_listenBrainzScrobbler = nullptr;
    QThread *m_lastFmThread = nullptr;
    LastFmScrobbler *m_lastFmScrobbler = nullptr;
    QThread *m_mpdImportThread = nullptr;
    MpdImportWorker *m_mpdImportWorker = nullptr;
    MprisService *m_mpris = nullptr;
    double m_volume = 1.0;
    qint64 m_lastUiRefreshIndexedTracks = 0;
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
