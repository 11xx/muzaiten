#include "ui/MainWindow.h"

#include "Version.h"
#include "db/Database.h"
#include "fs/LinkRoot.h"
#include "playback/GStreamerPlaybackBackend.h"
#include "playback/PlaybackBackend.h"
#include "scanner/ScanWorker.h"
#include "scanner/ArtworkResolver.h"
#include "scrobble/ListenBrainzScrobbler.h"
#include "ui/AlbumGrid.h"
#include "ui/ArtistSidebar.h"
#include "ui/LinkRootsDialog.h"
#include "ui/PlayerBar.h"
#include "ui/RightSidebar.h"
#include "ui/TrackTable.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QCloseEvent>
#include <QProgressBar>
#include <QSplitter>
#include <QStatusBar>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QUuid>

#include <algorithm>

Q_LOGGING_CATEGORY(uiLog, "muzaiten.ui")

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("muzaiten"));
    resize(1440, 900);
    setMinimumSize(1100, 700);

    auto *central = new QWidget(this);
    auto *centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);

    m_playerBar = new PlayerBar(central);
    centralLayout->addWidget(m_playerBar, 0);

    m_rootSplitter = new QSplitter(Qt::Horizontal, central);
    m_artistSidebar = new ArtistSidebar(m_rootSplitter);

    m_centerSplitter = new QSplitter(Qt::Vertical, m_rootSplitter);
    m_albumGrid = new AlbumGrid(m_centerSplitter);
    m_trackTable = new TrackTable(m_centerSplitter);
    m_centerSplitter->setStretchFactor(0, 55);
    m_centerSplitter->setStretchFactor(1, 45);

    m_rightSidebar = new RightSidebar(m_rootSplitter);

    m_rootSplitter->addWidget(m_artistSidebar);
    m_rootSplitter->addWidget(m_centerSplitter);
    m_rootSplitter->addWidget(m_rightSidebar);
    m_rootSplitter->setStretchFactor(0, 0);
    m_rootSplitter->setStretchFactor(1, 1);
    m_rootSplitter->setStretchFactor(2, 0);
    m_rootSplitter->setSizes({260, 900, 300});

    centralLayout->addWidget(m_rootSplitter, 1);
    setCentralWidget(central);

    m_scanProgress = new QProgressBar(this);
    m_scanProgress->setRange(0, 0);
    m_scanProgress->setVisible(false);
    statusBar()->addPermanentWidget(m_scanProgress);

    m_playback = new GStreamerPlaybackBackend(this);

    m_database = std::make_unique<Database>(QStringLiteral("main-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!m_database->open(databasePath())) {
        QMessageBox::warning(this, QStringLiteral("Database"), m_database->lastError());
    }

    m_listenBrainzThread = new QThread(this);
    m_listenBrainzScrobbler = new ListenBrainzScrobbler;
    m_listenBrainzScrobbler->moveToThread(m_listenBrainzThread);
    connect(m_listenBrainzThread, &QThread::finished, m_listenBrainzScrobbler, &QObject::deleteLater);
    m_listenBrainzThread->start();

    connect(m_artistSidebar, &ArtistSidebar::artistSelected, this, &MainWindow::selectArtist);
    connect(m_trackTable, &TrackTable::trackActivated, this, &MainWindow::appendAndPlayTrack);
    connect(m_trackTable, &TrackTable::playNextRequested, this, &MainWindow::playNextTracks);
    connect(m_trackTable, &TrackTable::addToQueueRequested, this, &MainWindow::addTracksToQueue);
    connect(m_trackTable, &TrackTable::trackRatingChanged, this, &MainWindow::applyTrackRating);
    connect(m_trackTable, &TrackTable::viewSettingsChanged, this, &MainWindow::saveTrackTableViewSettings);
    connect(m_albumGrid, &AlbumGrid::albumSelectionToggled, this, &MainWindow::selectAlbumFilter);
    connect(m_albumGrid, &AlbumGrid::albumPlayNextRequested, this, &MainWindow::playNextAlbum);
    connect(m_albumGrid, &AlbumGrid::albumAddToQueueRequested, this, &MainWindow::addAlbumToQueue);
    connect(m_albumGrid, &AlbumGrid::albumRatingChanged, this, &MainWindow::applyAlbumRating);
    connect(m_albumGrid, &AlbumGrid::viewSettingsChanged, this, &MainWindow::saveAlbumGridViewSettings);
    connect(m_artistSidebar, &ArtistSidebar::viewSettingsChanged, this, &MainWindow::saveArtistSidebarViewSettings);
    connect(m_rightSidebar, &RightSidebar::viewSettingsChanged, this, &MainWindow::saveRightSidebarViewSettings);
    connect(m_rootSplitter, &QSplitter::splitterMoved, this, &MainWindow::saveMainWindowViewSettings);
    connect(m_centerSplitter, &QSplitter::splitterMoved, this, &MainWindow::saveMainWindowViewSettings);
    connect(m_rightSidebar, &RightSidebar::queueTrackActivated, this, &MainWindow::playQueueIndex);
    connect(m_playerBar, &PlayerBar::openLibraryRequested, this, &MainWindow::openLibraryFolder);
    connect(m_playerBar, &PlayerBar::linkRootsRequested, this, &MainWindow::configureLinkRoots);
    connect(m_playerBar, &PlayerBar::listenBrainzEnabledChanged, this, &MainWindow::setListenBrainzEnabled);
    connect(m_playerBar, &PlayerBar::listenBrainzTokenRequested, this, &MainWindow::setListenBrainzToken);
    connect(m_playerBar, &PlayerBar::previousRequested, this, &MainWindow::playPreviousTrack);
    connect(m_playerBar, &PlayerBar::playPauseRequested, this, &MainWindow::togglePlayback);
    connect(m_playerBar, &PlayerBar::nextRequested, this, &MainWindow::playNextTrack);
    connect(m_playerBar, &PlayerBar::stopRequested, m_playback, &PlaybackBackend::stop);
    connect(m_playerBar, &PlayerBar::seekRequested, m_playback, &PlaybackBackend::seek);
    connect(m_playerBar, &PlayerBar::volumeChanged, this, [this](int volume) {
        m_playback->setVolume(static_cast<double>(std::clamp(volume, 0, 100)) / 100.0);
    });
    connect(m_playerBar, &PlayerBar::currentTrackRatingChanged, this, [this](int rating) {
        if (!m_currentTrack.path.isEmpty()) {
            applyTrackRating(m_currentTrack, rating);
        }
    });
    connect(m_trackTable, &TrackTable::findFileRequested, this, &MainWindow::findTrackFile);
    connect(m_rightSidebar, &RightSidebar::findFileRequested, this, &MainWindow::findTrackFile);
    connect(m_playback, &PlaybackBackend::positionChanged, this, &MainWindow::updatePlaybackPosition);
    connect(m_playback, &PlaybackBackend::durationChanged, this, &MainWindow::updatePlaybackPosition);
    connect(m_playback, &PlaybackBackend::preparedTrackStarted, this, &MainWindow::advanceAfterPreparedTransition);
    connect(m_playback, &PlaybackBackend::stateChanged, this, [this](PlaybackBackend::State state) {
        const bool playing = state == PlaybackBackend::State::Playing;
        m_playerBar->setPlaying(playing);
        QMetaObject::invokeMethod(m_listenBrainzScrobbler, "playbackStateChanged", Qt::QueuedConnection, Q_ARG(bool, playing));
    });
    connect(m_playback, &PlaybackBackend::finished, this, [this]() {
        if (m_queueIndex + 1 < m_queue.size()) {
            playQueueIndex(m_queueIndex + 1);
        }
    });
    connect(m_playback, &PlaybackBackend::errorOccurred, this, [this](const QString &errorString) {
        if (!errorString.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("Playback error: %1").arg(errorString), 10000);
        }
    });

    m_albumGrid->setArtworkCacheRoot(cacheRoot());
    loadViewSettings();
    configureListenBrainz();
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
    if (m_listenBrainzThread != nullptr) {
        m_listenBrainzThread->quit();
        m_listenBrainzThread->wait(3000);
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

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveMainWindowViewSettings();
    QMainWindow::closeEvent(event);
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
    if (m_currentArtist != artistName) {
        m_selectedAlbumTitle.clear();
    }
    m_currentArtist = artistName;
    refreshAlbumGrid();
    refreshTrackTable();
    restoreTrackTableViewState();
}

void MainWindow::selectAlbumFilter(const QString &albumTitle)
{
    rememberTrackTableViewState();
    m_selectedAlbumTitle = (m_selectedAlbumTitle == albumTitle) ? QString() : albumTitle;
    refreshAlbumGrid();
    refreshTrackTable();
    restoreTrackTableViewState();
}

void MainWindow::refreshAlbumGrid()
{
    if (m_currentArtist.isEmpty()) {
        return;
    }
    m_albumGrid->setAlbums(m_database->albumsForArtist(m_currentArtist));
    m_albumGrid->setSelectedAlbumTitle(m_selectedAlbumTitle);
}

void MainWindow::refreshTrackTable()
{
    if (m_currentArtist.isEmpty()) {
        return;
    }
    m_trackTable->setTracks(m_database->tracksForArtist(m_currentArtist, m_selectedAlbumTitle));
}

void MainWindow::applyTrackRating(const Track &track, int rating0To100)
{
    const bool ok = rating0To100 < 0 ? m_database->clearUserTrackRating(track.path) : m_database->setUserTrackRating(track.path, rating0To100);
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("Rating"), m_database->lastError());
        return;
    }
    rememberTrackTableViewState();
    refreshTrackTable();
    refreshAlbumGrid();
    for (Track &queuedTrack : m_queue) {
        if (queuedTrack.path != track.path) {
            continue;
        }
        queuedTrack.hasUserRating = rating0To100 >= 0;
        queuedTrack.effectiveRating0To100 = rating0To100 >= 0 ? rating0To100 : queuedTrack.rating0To100;
    }
    if (m_currentTrack.path == track.path) {
        m_currentTrack.hasUserRating = rating0To100 >= 0;
        m_currentTrack.effectiveRating0To100 = rating0To100 >= 0 ? rating0To100 : m_currentTrack.rating0To100;
        const QString title = m_currentTrack.title.isEmpty() ? m_currentTrack.filename : m_currentTrack.title;
        QString subtitle = QStringLiteral("%1 - %2").arg(m_currentTrack.artistName, m_currentTrack.albumTitle);
        if (!m_currentTrack.date.isEmpty()) {
            subtitle += QStringLiteral(" (%1)").arg(m_currentTrack.date.left(4));
        }
        m_playerBar->setTrackInfo(title, subtitle, m_currentTrack.effectiveRating0To100);
    }
    m_rightSidebar->setQueue(m_queue);
    m_rightSidebar->setCurrentIndex(m_queueIndex);
    restoreTrackTableViewState();
}

void MainWindow::applyAlbumRating(const QString &albumArtistName, const QString &albumTitle, int rating0To100)
{
    const bool ok = rating0To100 < 0 ? m_database->clearUserAlbumRating(albumArtistName, albumTitle) : m_database->setUserAlbumRating(albumArtistName, albumTitle, rating0To100);
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("Rating"), m_database->lastError());
        return;
    }
    refreshAlbumGrid();
}

void MainWindow::loadViewSettings()
{
    m_trackTable->applyViewSettingsJson(m_database->setting(QStringLiteral("trackTable.view")));
    m_rightSidebar->applyViewSettingsJson(m_database->setting(QStringLiteral("rightSidebar.view")));
    m_albumGrid->applyViewSettingsJson(m_database->setting(QStringLiteral("albumGrid.view")));
    m_artistSidebar->applyViewSettingsJson(m_database->setting(QStringLiteral("artistSidebar.view")));
    const QJsonObject mainWindow = QJsonDocument::fromJson(m_database->setting(QStringLiteral("mainWindow.view")).toUtf8()).object();
    const QByteArray geometry = QByteArray::fromBase64(mainWindow.value(QStringLiteral("geometry")).toString().toLatin1());
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    auto restoreSplitter = [](QSplitter *splitter, const QJsonArray &array) {
        QList<int> sizes;
        for (const QJsonValue &value : array) {
            sizes.push_back(value.toInt());
        }
        if (sizes.size() == splitter->count()) {
            splitter->setSizes(sizes);
        }
    };
    restoreSplitter(m_rootSplitter, mainWindow.value(QStringLiteral("rootSplitter")).toArray());
    restoreSplitter(m_centerSplitter, mainWindow.value(QStringLiteral("centerSplitter")).toArray());
    applySharedTableSettings();
}

void MainWindow::saveTrackTableViewSettings()
{
    m_database->setSetting(QStringLiteral("trackTable.view"), m_trackTable->viewSettingsJson());
    applySharedTableSettings();
}

void MainWindow::saveAlbumGridViewSettings()
{
    m_database->setSetting(QStringLiteral("albumGrid.view"), m_albumGrid->viewSettingsJson());
}

void MainWindow::saveArtistSidebarViewSettings()
{
    m_database->setSetting(QStringLiteral("artistSidebar.view"), m_artistSidebar->viewSettingsJson());
}

void MainWindow::saveRightSidebarViewSettings()
{
    m_database->setSetting(QStringLiteral("rightSidebar.view"), m_rightSidebar->viewSettingsJson());
    applySharedTableSettings();
}

void MainWindow::saveMainWindowViewSettings()
{
    auto sizesToJson = [](const QList<int> &sizes) {
        QJsonArray array;
        for (int size : sizes) {
            array.append(size);
        }
        return array;
    };

    QJsonObject root;
    root.insert(QStringLiteral("geometry"), QString::fromLatin1(saveGeometry().toBase64()));
    root.insert(QStringLiteral("rootSplitter"), sizesToJson(m_rootSplitter->sizes()));
    root.insert(QStringLiteral("centerSplitter"), sizesToJson(m_centerSplitter->sizes()));
    m_database->setSetting(QStringLiteral("mainWindow.view"), QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

void MainWindow::applySharedTableSettings()
{
    const QJsonObject sharedSettings = QJsonDocument::fromJson(m_database->setting(QStringLiteral("tables.view")).toUtf8()).object();
    const QJsonObject trackSettings = QJsonDocument::fromJson(m_database->setting(QStringLiteral("trackTable.view")).toUtf8()).object();
    const QJsonObject sidebarSettings = QJsonDocument::fromJson(m_database->setting(QStringLiteral("rightSidebar.view")).toUtf8()).object();
    const int headerHeight = sharedSettings.value(QStringLiteral("headerHeight")).toInt(trackSettings.value(QStringLiteral("headerHeight")).toInt(sidebarSettings.value(QStringLiteral("headerHeight")).toInt(22)));
    m_trackTable->setHeaderHeight(headerHeight);
    m_rightSidebar->setHeaderHeight(headerHeight);

    QJsonObject shared;
    shared.insert(QStringLiteral("headerHeight"), headerHeight);
    m_database->setSetting(QStringLiteral("tables.view"), QString::fromUtf8(QJsonDocument(shared).toJson(QJsonDocument::Compact)));
}

void MainWindow::configureLinkRoots()
{
    LinkRootsDialog dialog(this);
    dialog.setLinkRoots(m_database->linkRoots());
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QVector<LinkRoot> existing = m_database->linkRoots();
    for (const LinkRoot &root : existing) {
        if (!m_database->removeLinkRoot(root.id)) {
            QMessageBox::warning(this, QStringLiteral("Link roots"), m_database->lastError());
            return;
        }
    }

    for (const LinkRoot &root : dialog.linkRoots()) {
        LinkRoot saved = root;
        saved.id = 0;
        if (!m_database->saveLinkRoot(saved)) {
            QMessageBox::warning(this, QStringLiteral("Link roots"), m_database->lastError());
            return;
        }
    }

    statusBar()->showMessage(QStringLiteral("Link roots updated"), 3000);
}

void MainWindow::findTrackFile(const Track &track, bool writable)
{
    if (track.path.isEmpty()) {
        return;
    }

    const PathResolver resolver(m_database->linkRoots());
    const PathResolution resolution = resolver.resolveLocalPath(track.path, writable ? PathUse::Write : PathUse::Read);
    if (resolution.preferredPath.isEmpty()) {
        QMessageBox::warning(this,
                             writable ? QStringLiteral("Find writable file") : QStringLiteral("Find file"),
                             QStringLiteral("%1\n\nCandidates:\n%2")
                                 .arg(resolution.failureReason, resolution.candidates.join(QLatin1Char('\n'))));
        return;
    }

    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(resolution.preferredPath).absolutePath()));
    statusBar()->showMessage(QStringLiteral("Resolved %1").arg(resolution.preferredPath), 5000);
}

void MainWindow::configureListenBrainz()
{
    const bool enabled = m_database->setting(QStringLiteral("listenbrainz.enabled"), QStringLiteral("false")) == QStringLiteral("true");
    QString token = m_database->setting(QStringLiteral("listenbrainz.token"));
    if (token.isEmpty()) {
        token = QString::fromLocal8Bit(qgetenv("LISTENBRAINZ_TOKEN")).trimmed();
    }

    m_playerBar->setListenBrainzEnabled(enabled);
    QMetaObject::invokeMethod(m_listenBrainzScrobbler,
                              "configure",
                              Qt::QueuedConnection,
                              Q_ARG(bool, enabled),
                              Q_ARG(QString, token),
                              Q_ARG(QString, stateRoot() + QStringLiteral("/listenbrainz-pending.json")));
}

void MainWindow::setListenBrainzEnabled(bool enabled)
{
    m_database->setSetting(QStringLiteral("listenbrainz.enabled"), enabled ? QStringLiteral("true") : QStringLiteral("false"));
    configureListenBrainz();
    statusBar()->showMessage(enabled ? QStringLiteral("ListenBrainz scrobbling enabled") : QStringLiteral("ListenBrainz scrobbling disabled"), 3000);
}

void MainWindow::setListenBrainzToken()
{
    const QString current = m_database->setting(QStringLiteral("listenbrainz.token"));
    bool ok = false;
    const QString token = QInputDialog::getText(this,
                                                QStringLiteral("ListenBrainz token"),
                                                QStringLiteral("User token"),
                                                QLineEdit::Password,
                                                current,
                                                &ok)
                              .trimmed();
    if (!ok) {
        return;
    }

    m_database->setSetting(QStringLiteral("listenbrainz.token"), token);
    configureListenBrainz();
    statusBar()->showMessage(QStringLiteral("ListenBrainz token updated"), 3000);
}

void MainWindow::playTrack(const Track &track)
{
    if (track.path.isEmpty()) {
        return;
    }

    presentTrack(track);
    m_playback->play(QUrl::fromLocalFile(track.path));
    prepareNextQueueTrack();
}

void MainWindow::presentTrack(const Track &track)
{
    m_currentTrack = track;
    updateCurrentAlbumArt();
    const QString title = track.title.isEmpty() ? track.filename : track.title;
    QString subtitle = QStringLiteral("%1 - %2").arg(track.artistName, track.albumTitle);
    if (!track.date.isEmpty()) {
        subtitle += QStringLiteral(" (%1)").arg(track.date.left(4));
    }
    m_playerBar->setTrackInfo(title, subtitle, track.effectiveRating0To100);
    m_playerBar->setPosition(0, track.durationMs);
    QMetaObject::invokeMethod(m_listenBrainzScrobbler, "trackStarted", Qt::QueuedConnection, Q_ARG(Track, track));
    statusBar()->showMessage(QStringLiteral("Playing %1").arg(title), 3000);
}

void MainWindow::appendAndPlayTrack(const Track &track)
{
    if (track.path.isEmpty()) {
        return;
    }

    for (int index = 0; index < m_queue.size(); ++index) {
        if (m_queue.at(index).path == track.path) {
            playQueueIndex(index);
            return;
        }
    }

    m_queue.push_back(track);
    m_rightSidebar->setQueue(m_queue);
    playQueueIndex(static_cast<int>(m_queue.size() - 1));
}

void MainWindow::playNextTracks(const QVector<Track> &tracks)
{
    if (tracks.isEmpty()) {
        return;
    }

    if (m_queueIndex < 0 || m_queue.isEmpty()) {
        const int start = static_cast<int>(m_queue.size());
        for (const Track &track : tracks) {
            if (!track.path.isEmpty()) {
                m_queue.push_back(track);
            }
        }
        m_rightSidebar->setQueue(m_queue);
        if (m_queueIndex < 0 && start < m_queue.size()) {
            playQueueIndex(start);
        }
        return;
    }

    int insertAt = m_playNextInsertIndex;
    if (insertAt <= m_queueIndex || insertAt > m_queue.size()) {
        insertAt = m_queueIndex + 1;
    }

    int inserted = 0;
    for (const Track &track : tracks) {
        if (track.path.isEmpty()) {
            continue;
        }
        m_queue.insert(insertAt + inserted, track);
        ++inserted;
    }

    m_playNextInsertIndex = insertAt + inserted;
    m_rightSidebar->setQueue(m_queue);
    m_rightSidebar->setCurrentIndex(m_queueIndex);
}

void MainWindow::addTracksToQueue(const QVector<Track> &tracks)
{
    for (const Track &track : tracks) {
        if (!track.path.isEmpty()) {
            m_queue.push_back(track);
        }
    }
    m_rightSidebar->setQueue(m_queue);
    m_rightSidebar->setCurrentIndex(m_queueIndex);
}

void MainWindow::playNextAlbum(const QString &albumTitle)
{
    if (!m_currentArtist.isEmpty() && !albumTitle.isEmpty()) {
        playNextTracks(m_database->tracksForArtist(m_currentArtist, albumTitle));
    }
}

void MainWindow::addAlbumToQueue(const QString &albumTitle)
{
    if (!m_currentArtist.isEmpty() && !albumTitle.isEmpty()) {
        addTracksToQueue(m_database->tracksForArtist(m_currentArtist, albumTitle));
    }
}

void MainWindow::playQueueIndex(int index)
{
    if (index < 0 || index >= m_queue.size()) {
        return;
    }

    m_queueIndex = index;
    if (m_playNextInsertIndex <= m_queueIndex || m_playNextInsertIndex > m_queue.size()) {
        m_playNextInsertIndex = m_queueIndex + 1;
    }
    m_rightSidebar->setCurrentIndex(m_queueIndex);
    playTrack(m_queue.at(m_queueIndex));
}

void MainWindow::playPreviousTrack()
{
    if (m_queue.isEmpty()) {
        return;
    }
    playQueueIndex(std::max(0, m_queueIndex - 1));
}

void MainWindow::playNextTrack()
{
    if (m_queue.isEmpty()) {
        return;
    }
    playQueueIndex(std::min(static_cast<int>(m_queue.size() - 1), m_queueIndex + 1));
}

void MainWindow::togglePlayback()
{
    if (!m_playback->hasSource()) {
        return;
    }

    if (m_playback->state() == PlaybackBackend::State::Playing) {
        m_playback->pause();
    } else {
        m_playback->resume();
    }
}

void MainWindow::updatePlaybackPosition()
{
    m_playerBar->setPosition(m_playback->position(), m_playback->duration());
}

void MainWindow::prepareNextQueueTrack()
{
    if (m_queueIndex < 0 || m_queueIndex + 1 >= m_queue.size()) {
        m_playback->prepareNext({});
        return;
    }
    const Track &nextTrack = m_queue.at(m_queueIndex + 1);
    m_playback->prepareNext(QUrl::fromLocalFile(nextTrack.path));
}

void MainWindow::advanceAfterPreparedTransition()
{
    if (m_queueIndex + 1 >= m_queue.size()) {
        return;
    }

    ++m_queueIndex;
    if (m_playNextInsertIndex <= m_queueIndex || m_playNextInsertIndex > m_queue.size()) {
        m_playNextInsertIndex = m_queueIndex + 1;
    }
    m_rightSidebar->setCurrentIndex(m_queueIndex);
    presentTrack(m_queue.at(m_queueIndex));
    prepareNextQueueTrack();
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
