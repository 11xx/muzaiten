#include "ui/MainWindow.h"

#include "Version.h"
#include "db/Database.h"
#include "scanner/LibraryScanner.h"
#include "ui/AlbumGrid.h"
#include "ui/ArtistSidebar.h"
#include "ui/TrackTable.h"

#include <QAction>
#include <QDir>
#include <QFileDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStandardPaths>
#include <QUuid>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("muzaiten %1").arg(QStringLiteral(MUZAITEN_VERSION)));
    resize(1440, 900);
    setMinimumSize(1100, 700);

    auto *root = new QSplitter(Qt::Horizontal, this);
    m_artistSidebar = new ArtistSidebar(root);

    auto *center = new QSplitter(Qt::Vertical, root);
    m_albumGrid = new AlbumGrid(center);
    m_trackTable = new TrackTable(center);
    center->setStretchFactor(0, 55);
    center->setStretchFactor(1, 45);

    root->addWidget(m_artistSidebar);
    root->addWidget(center);
    root->setStretchFactor(0, 0);
    root->setStretchFactor(1, 1);
    root->setSizes({260, 1180});

    setCentralWidget(root);

    auto *libraryMenu = menuBar()->addMenu(QStringLiteral("&Library"));
    auto *openAction = libraryMenu->addAction(QStringLiteral("&Open Folder..."));
    connect(openAction, &QAction::triggered, this, &MainWindow::openLibraryFolder);

    m_database = std::make_unique<Database>(QStringLiteral("main-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!m_database->open(databasePath())) {
        QMessageBox::warning(this, QStringLiteral("Database"), m_database->lastError());
    }

    connect(m_artistSidebar, &ArtistSidebar::artistSelected, this, &MainWindow::selectArtist);
    m_albumGrid->setArtworkCacheRoot(cacheRoot());
    loadExistingLibrary();
}

MainWindow::~MainWindow() = default;

void MainWindow::openLibraryFolder()
{
    const QString root = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose music library folder"));
    if (root.isEmpty()) {
        return;
    }

    LibraryScanner scanner(this);
    const QVector<Track> tracks = scanner.scan(root);

    for (const Track &track : tracks) {
        if (!m_database->upsertTrack(track)) {
            QMessageBox::warning(this, QStringLiteral("Scanner"), m_database->lastError());
            break;
        }
    }

    statusBar()->showMessage(QStringLiteral("Indexed %1 tracks read-only").arg(tracks.size()), 5000);
    refreshArtists();
}

void MainWindow::loadExistingLibrary()
{
    refreshArtists();
}

void MainWindow::refreshArtists()
{
    const QVector<Artist> artists = m_database->albumArtists();
    m_artistSidebar->setArtists(artists);
    if (!artists.isEmpty()) {
        selectArtist(artists.first().name);
    }
}

void MainWindow::selectArtist(const QString &artistName)
{
    m_currentArtist = artistName;
    m_albumGrid->setAlbums(m_database->albumsForArtist(artistName));
    m_trackTable->setTracks(m_database->tracksForArtist(artistName));
}

QString MainWindow::databasePath() const
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(root);
    return QDir(root).filePath(QStringLiteral("library.sqlite"));
}

QString MainWindow::cacheRoot() const
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(root);
    return QDir(root).filePath(QStringLiteral("artwork"));
}
