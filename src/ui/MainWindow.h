#pragma once

#include <QMainWindow>

class AlbumGrid;
class ArtistSidebar;
class TrackTable;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    ArtistSidebar *m_artistSidebar = nullptr;
    AlbumGrid *m_albumGrid = nullptr;
    TrackTable *m_trackTable = nullptr;
};

