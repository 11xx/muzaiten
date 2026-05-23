#pragma once

#include <QMainWindow>

#include "core/Track.h"

#include <memory>

class Database;
class AlbumGrid;
class ArtistSidebar;
class PlayerBar;
class QAudioOutput;
class QMediaPlayer;
class QProgressBar;
class QThread;
class RightSidebar;
class TrackTable;
class ScanWorker;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

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
    void applySharedTableSettings();
    void playTrack(const Track &track);
    void appendAndPlayTrack(const Track &track);
    void playQueueIndex(int index);
    void togglePlayback();
    void updatePlaybackPosition();
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
    PlayerBar *m_playerBar = nullptr;
    AlbumGrid *m_albumGrid = nullptr;
    TrackTable *m_trackTable = nullptr;
    RightSidebar *m_rightSidebar = nullptr;
    QProgressBar *m_scanProgress = nullptr;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audioOutput = nullptr;
    std::unique_ptr<Database> m_database;
    QString m_currentArtist;
    QString m_selectedAlbumTitle;
    Track m_currentTrack;
    QVector<Track> m_queue;
    int m_queueIndex = -1;
    int m_trackSortColumn = 0;
    Qt::SortOrder m_trackSortOrder = Qt::AscendingOrder;
    int m_trackScrollValue = 0;
    QThread *m_scanThread = nullptr;
    ScanWorker *m_scanWorker = nullptr;
    qint64 m_lastUiRefreshIndexedTracks = 0;
};
