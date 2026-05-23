#include "ui/MainWindow.h"

#include "Version.h"
#include "db/Database.h"
#include "scanner/ScanWorker.h"
#include "scanner/ArtworkResolver.h"
#include "ui/AlbumGrid.h"
#include "ui/ArtistSidebar.h"
#include "ui/PlayerBar.h"
#include "ui/RightSidebar.h"
#include "ui/TrackTable.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QLoggingCategory>
#include <QMenuBar>
#include <QMessageBox>
#include <QAudioOutput>
#include <QMediaPlayer>
#include <QProgressBar>
#include <QSplitter>
#include <QStatusBar>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QUuid>

Q_LOGGING_CATEGORY(uiLog, "muzaiten.ui")

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("muzaiten %1").arg(QStringLiteral(MUZAITEN_VERSION)));
    resize(1440, 900);
    setMinimumSize(1100, 700);

    auto *central = new QWidget(this);
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);

    m_playerBar = new PlayerBar(central);
    centralLayout->addWidget(m_playerBar, 0);

    auto *root = new QSplitter(Qt::Horizontal, central);
    m_artistSidebar = new ArtistSidebar(root);

    auto *center = new QSplitter(Qt::Vertical, root);
    m_albumGrid = new AlbumGrid(center);
    m_trackTable = new TrackTable(center);
    center->setStretchFactor(0, 55);
    center->setStretchFactor(1, 45);

    m_rightSidebar = new RightSidebar(root);

    root->addWidget(m_artistSidebar);
    root->addWidget(center);
    root->addWidget(m_rightSidebar);
    root->setStretchFactor(0, 0);
    root->setStretchFactor(1, 1);
    root->setStretchFactor(2, 0);
    root->setSizes({260, 900, 300});

    centralLayout->addWidget(root, 1);
    setCentralWidget(central);

    m_scanProgress = new QProgressBar(this);
    m_scanProgress->setRange(0, 0);
    m_scanProgress->setVisible(false);
    statusBar()->addPermanentWidget(m_scanProgress);

    m_audioOutput = new QAudioOutput(this);
    m_audioOutput->setVolume(1.0);
    m_player = new QMediaPlayer(this);
    m_player->setAudioOutput(m_audioOutput);

    auto *libraryMenu = menuBar()->addMenu(QStringLiteral("&Library"));
    auto *openAction = libraryMenu->addAction(QStringLiteral("&Open Folder..."));
    connect(openAction, &QAction::triggered, this, &MainWindow::openLibraryFolder);

    m_database = std::make_unique<Database>(QStringLiteral("main-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!m_database->open(databasePath())) {
        QMessageBox::warning(this, QStringLiteral("Database"), m_database->lastError());
    }

    connect(m_artistSidebar, &ArtistSidebar::artistSelected, this, &MainWindow::selectArtist);
    connect(m_trackTable, &TrackTable::trackActivated, this, &MainWindow::appendAndPlayTrack);
    connect(m_rightSidebar, &RightSidebar::queueTrackActivated, this, &MainWindow::playQueueIndex);
    connect(m_playerBar, &PlayerBar::playPauseRequested, this, &MainWindow::togglePlayback);
    connect(m_playerBar, &PlayerBar::stopRequested, m_player, &QMediaPlayer::stop);
    connect(m_playerBar, &PlayerBar::seekRequested, m_player, &QMediaPlayer::setPosition);
    connect(m_player, &QMediaPlayer::positionChanged, this, &MainWindow::updatePlaybackPosition);
    connect(m_player, &QMediaPlayer::durationChanged, this, &MainWindow::updatePlaybackPosition);
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        m_playerBar->setPlaying(state == QMediaPlayer::PlayingState);
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &errorString) {
        if (!errorString.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("Playback error: %1").arg(errorString), 10000);
        }
    });

    m_albumGrid->setArtworkCacheRoot(cacheRoot());
    loadExistingLibrary();
}

MainWindow::~MainWindow()
{
    if (m_scanWorker != nullptr) {
        m_scanWorker->cancel();
    }
    if (m_scanThread != nullptr) {
        m_scanThread->quit();
        m_scanThread->wait(3000);
    }
}

void MainWindow::openLibraryFolder()
{
    const QString root = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose music library folder"));
    if (root.isEmpty()) {
        return;
    }

    startScan(root);
}

void MainWindow::startScan(const QString &rootPath)
{
    if (m_scanThread != nullptr) {
        statusBar()->showMessage(QStringLiteral("A scan is already running"), 5000);
        return;
    }

    qCInfo(uiLog) << "starting scan" << rootPath;
    statusBar()->showMessage(QStringLiteral("Scanning %1").arg(rootPath));
    m_scanProgress->setVisible(true);
    m_lastUiRefreshIndexedTracks = 0;

    m_scanThread = new QThread(this);
    m_scanWorker = new ScanWorker(rootPath, 128);
    m_scanWorker->moveToThread(m_scanThread);

    connect(m_scanThread, &QThread::started, m_scanWorker, &ScanWorker::run);
    connect(m_scanWorker, &ScanWorker::batchReady, this, &MainWindow::ingestScanBatch);
    connect(m_scanWorker, &ScanWorker::progress, this, [this](qint64 visitedFiles, qint64 indexedTracks, const QString &currentPath) {
        statusBar()->showMessage(QStringLiteral("Scanning: %1 files visited, %2 tracks indexed").arg(visitedFiles).arg(indexedTracks));
        qCDebug(uiLog) << "scan progress" << visitedFiles << indexedTracks << currentPath;
    });
    connect(m_scanWorker, &ScanWorker::finished, this, &MainWindow::finishScan);
    connect(m_scanWorker, &ScanWorker::finished, m_scanThread, &QThread::quit);
    connect(m_scanThread, &QThread::finished, m_scanWorker, &QObject::deleteLater);
    connect(m_scanThread, &QThread::finished, m_scanThread, &QObject::deleteLater);
    connect(m_scanThread, &QThread::finished, this, [this]() {
        m_scanThread = nullptr;
        m_scanWorker = nullptr;
    });

    m_scanThread->start();
}

void MainWindow::ingestScanBatch(const QVector<Track> &tracks)
{
    if (tracks.isEmpty()) {
        return;
    }

    if (!m_database->beginTransaction()) {
        QMessageBox::warning(this, QStringLiteral("Scanner"), m_database->lastError());
        return;
    }
    for (const Track &track : tracks) {
        if (!m_database->upsertTrack(track)) {
            QMessageBox::warning(this, QStringLiteral("Scanner"), m_database->lastError());
            break;
        }
    }
    if (!m_database->commitTransaction()) {
        QMessageBox::warning(this, QStringLiteral("Scanner"), m_database->lastError());
        return;
    }

    m_lastUiRefreshIndexedTracks += tracks.size();
    if (m_lastUiRefreshIndexedTracks >= 512) {
        m_lastUiRefreshIndexedTracks = 0;
        refreshArtists();
    }
}

void MainWindow::finishScan(qint64 visitedFiles, qint64 indexedTracks, bool canceled)
{
    qCInfo(uiLog) << "scan finished" << visitedFiles << indexedTracks << "canceled" << canceled;
    m_scanProgress->setVisible(false);
    statusBar()->showMessage(
        canceled
            ? QStringLiteral("Scan canceled: %1 files visited, %2 tracks indexed").arg(visitedFiles).arg(indexedTracks)
            : QStringLiteral("Scan complete: %1 files visited, %2 tracks indexed").arg(visitedFiles).arg(indexedTracks),
        10000);
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

    if (!m_currentArtist.isEmpty() && m_artistSidebar->selectArtist(m_currentArtist)) {
        selectArtist(m_currentArtist);
        return;
    }

    if (!artists.isEmpty() && m_currentArtist.isEmpty()) {
        selectArtist(artists.first().name);
        m_artistSidebar->selectArtist(artists.first().name);
        return;
    }

    if (artists.isEmpty()) {
        m_currentArtist.clear();
    }
}

void MainWindow::selectArtist(const QString &artistName)
{
    rememberTrackTableViewState();
    m_currentArtist = artistName;
    m_albumGrid->setAlbums(m_database->albumsForArtist(artistName));
    m_trackTable->setTracks(m_database->tracksForArtist(artistName));
    restoreTrackTableViewState();
}

void MainWindow::playTrack(const Track &track)
{
    if (track.path.isEmpty()) {
        return;
    }

    m_currentTrack = track;
    updateCurrentAlbumArt();
    const QString label = QStringLiteral("%1 - %2").arg(track.artistName, track.title);
    m_playerBar->setTrackText(label);
    m_playerBar->setPosition(0, track.durationMs);
    m_player->setSource(QUrl::fromLocalFile(track.path));
    m_player->play();
    statusBar()->showMessage(QStringLiteral("Playing %1").arg(label), 3000);
}

void MainWindow::appendAndPlayTrack(const Track &track)
{
    if (track.path.isEmpty()) {
        return;
    }

    m_queue.push_back(track);
    m_rightSidebar->setQueue(m_queue);
    playQueueIndex(static_cast<int>(m_queue.size() - 1));
}

void MainWindow::playQueueIndex(int index)
{
    if (index < 0 || index >= m_queue.size()) {
        return;
    }

    m_queueIndex = index;
    m_rightSidebar->setCurrentIndex(m_queueIndex);
    playTrack(m_queue.at(m_queueIndex));
}

void MainWindow::togglePlayback()
{
    if (m_player->source().isEmpty()) {
        return;
    }

    if (m_player->playbackState() == QMediaPlayer::PlayingState) {
        m_player->pause();
    } else {
        m_player->play();
    }
}

void MainWindow::updatePlaybackPosition()
{
    m_playerBar->setPosition(m_player->position(), m_player->duration());
}

void MainWindow::rememberTrackTableViewState()
{
    if (m_trackTable == nullptr) {
        return;
    }

    m_trackSortColumn = m_trackTable->sortColumn();
    m_trackSortOrder = m_trackTable->sortOrder();
    m_trackScrollValue = m_trackTable->verticalScrollValue();
}

void MainWindow::restoreTrackTableViewState()
{
    m_trackTable->restoreViewState(m_trackSortColumn, m_trackSortOrder, m_trackScrollValue);
}

void MainWindow::updateCurrentAlbumArt()
{
    const ArtworkResolver resolver(cacheRoot());
    const ArtworkResult artwork = resolver.resolveForDirectory(m_currentTrack.parentDir);
    m_rightSidebar->setAlbumArt(artwork.cachePath);
}

QString MainWindow::databasePath() const
{
    const QString root = stateRoot() + QStringLiteral("/data");
    QDir().mkpath(root);
    return QDir(root).filePath(QStringLiteral("library.sqlite"));
}

QString MainWindow::cacheRoot() const
{
    const QString root = stateRoot() + QStringLiteral("/cache");
    QDir().mkpath(root);
    return QDir(root).filePath(QStringLiteral("artwork"));
}

QString MainWindow::stateRoot() const
{
    const QString overrideRoot = qApp->property("muzaiten.stateRoot").toString();
    if (!overrideRoot.isEmpty()) {
        QDir().mkpath(overrideRoot);
        return overrideRoot;
    }

    if (useDevState()) {
        const QString root = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("dev-state"));
        QDir().mkpath(root);
        return root;
    }

    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(root);
    return root;
}

bool MainWindow::useDevState() const
{
    return qApp->property("muzaiten.devState").toBool()
        || QCoreApplication::applicationDirPath().endsWith(QStringLiteral("/build"));
}
