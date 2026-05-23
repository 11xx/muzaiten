#pragma once

#include <QMainWindow>

#include <memory>

class Database;
class AlbumGrid;
class ArtistSidebar;
class TrackTable;

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
    QString databasePath() const;
    QString cacheRoot() const;

    ArtistSidebar *m_artistSidebar = nullptr;
    AlbumGrid *m_albumGrid = nullptr;
    TrackTable *m_trackTable = nullptr;
    std::unique_ptr<Database> m_database;
    QString m_currentArtist;
};
