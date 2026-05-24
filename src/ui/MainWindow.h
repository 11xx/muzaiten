#pragma once

#include <QMainWindow>

#include "core/Track.h"

#include <memory>

class Database;
class AlbumGrid;
class ArtistSidebar;
class QCloseEvent;
class ListenBrainzScrobbler;
class PlayerBar;
class PlaybackBackend;
class QProgressBar;
class QSplitter;
class QThread;
class RightSidebar;
class TrackTable;
class ScanWorker;

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
    void loadViewSettings();
    void saveTrackTableViewSettings();
    void saveAlbumGridViewSettings();
    void saveArtistSidebarViewSettings();
    void saveRightSidebarViewSettings();
    void saveMainWindowViewSettings();
    void applySharedTableSettings();
    void configureLinkRoots();
    void findTrackFile(const Track &track, bool writable);
    void configureMpdSource();
    void findMpdFile();
    QString mpdMusicDirectory() const;
    void configureListenBrainz();
    void setListenBrainzEnabled(bool enabled);
    void setListenBrainzToken();
    void playTrack(const Track &track);
    void presentTrack(const Track &track);
    void appendAndPlayTrack(const Track &track);
    void playNextTracks(const QVector<Track> &tracks);
    void addTracksToQueue(const QVector<Track> &tracks);
    void playNextAlbum(const QString &albumTitle);
    void addAlbumToQueue(const QString &albumTitle);
    void playQueueIndex(int index);
    void playPreviousTrack();
    void playNextTrack();
    void togglePlayback();
    void updatePlaybackPosition();
    void prepareNextQueueTrack();
    void advanceAfterPreparedTransition();
    void startScan(const QString &rootPath);
    void ingestScanBatch(const QVector<Track> &tracks);
    void finishScan(qint64 visitedFiles, qint64 indexedTracks, bool canceled);
    QString databasePath() const;
    QString cacheRoot() const;
    QString stateRoot() const;
    bool useDevState() const;
    void rememberTrackTableViewState();
    void restoreTrackTableViewState();
    void updateCurrentAlbumArt();

    ArtistSidebar *m_artistSidebar = nullptr;
    QSplitter *m_rootSplitter = nullptr;
    QSplitter *m_centerSplitter = nullptr;
    PlayerBar *m_playerBar = nullptr;
    AlbumGrid *m_albumGrid = nullptr;
    TrackTable *m_trackTable = nullptr;
    RightSidebar *m_rightSidebar = nullptr;
    QProgressBar *m_scanProgress = nullptr;
    PlaybackBackend *m_playback = nullptr;
    std::unique_ptr<Database> m_database;
    QString m_currentArtist;
    QString m_selectedAlbumTitle;
    Track m_currentTrack;
    QVector<Track> m_queue;
    int m_queueIndex = -1;
    int m_playNextInsertIndex = -1;
    int m_trackSortColumn = 0;
    Qt::SortOrder m_trackSortOrder = Qt::AscendingOrder;
    int m_trackScrollValue = 0;
    QThread *m_scanThread = nullptr;
    ScanWorker *m_scanWorker = nullptr;
    QThread *m_listenBrainzThread = nullptr;
    ListenBrainzScrobbler *m_listenBrainzScrobbler = nullptr;
    qint64 m_lastUiRefreshIndexedTracks = 0;
};
