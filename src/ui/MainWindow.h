#pragma once

#include <QMainWindow>

#include <memory>

class Database;
class AlbumGrid;
class ArtistSidebar;
class PlayerBar;
class QProgressBar;
class QThread;
class RightSidebar;
class TrackTable;
class ScanWorker;
struct Track;

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
    void startScan(const QString &rootPath);
    void ingestScanBatch(const QVector<Track> &tracks);
    void finishScan(qint64 visitedFiles, qint64 indexedTracks, bool canceled);
    QString databasePath() const;
    QString cacheRoot() const;
    QString stateRoot() const;
    bool useDevState() const;

    ArtistSidebar *m_artistSidebar = nullptr;
    PlayerBar *m_playerBar = nullptr;
    AlbumGrid *m_albumGrid = nullptr;
    TrackTable *m_trackTable = nullptr;
    RightSidebar *m_rightSidebar = nullptr;
    QProgressBar *m_scanProgress = nullptr;
    std::unique_ptr<Database> m_database;
    QString m_currentArtist;
    QThread *m_scanThread = nullptr;
    ScanWorker *m_scanWorker = nullptr;
    qint64 m_lastUiRefreshIndexedTracks = 0;
};
