#pragma once
#include <QObject>
class MainWindow;
class ViewStatePersistence final : public QObject {
public:
    explicit ViewStatePersistence(MainWindow &window);
    void loadViewSettings();
    void saveTrackTableViewSettings();
    void saveAlbumGridViewSettings();
    void saveMusicExplorerAlbumGridViewSettings();
    void saveArtistSidebarViewSettings();
    void saveRightSidebarViewSettings();
    void saveQueueScreenViewSettings();
    void savePlaylistViewSettings();
    void saveMainWindowViewSettings(bool captureSplitterSizes = false);
    void saveAllViewSettings();
private:
    MainWindow &m_window;
};
