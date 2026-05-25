#include "ui/MainWindow.h"

#include "Version.h"
#include "core/Rating.h"
#include "db/Database.h"
#include "fs/LinkRoot.h"
#include "playback/GStreamerPlaybackBackend.h"
#include "playback/PlaybackBackend.h"
#include "mpd/MpdConfig.h"
#include "mpd/MpdImportWorker.h"
#include "scanner/ScanWorker.h"
#include "scanner/ArtworkResolver.h"
#include "scanner/RatingTagSyncWorker.h"
#include "scrobble/ListenBrainzScrobbler.h"
#include "ui/AlbumGrid.h"
#include "ui/ArtistSidebar.h"
#include "ui/LinkRootsDialog.h"
#include "ui/PlayerBar.h"
#include "ui/PlaybackProfileDialog.h"
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
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>
#include <QUuid>

#include <algorithm>

Q_LOGGING_CATEGORY(uiLog, "muzaiten.ui")

namespace {

PlaybackProfile playbackProfileFromJson(const QString &json)
{
    PlaybackProfile profile;
    if (json.isEmpty()) {
        return profile;
    }

    const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
    profile.id = root.value(QStringLiteral("id")).toString(profile.id);
    profile.name = root.value(QStringLiteral("name")).toString(profile.name);
    profile.backend = root.value(QStringLiteral("backend")).toString(profile.backend);
    profile.mode = root.value(QStringLiteral("mode")).toString(profile.mode);
    profile.sink = root.value(QStringLiteral("sink")).toString(profile.sink);
    profile.device = root.value(QStringLiteral("device")).toString(profile.device);
    profile.softwareVolume = root.value(QStringLiteral("softwareVolume")).toBool(profile.softwareVolume);
    profile.replayGain = root.value(QStringLiteral("replayGain")).toBool(profile.replayGain);
    profile.allowResample = root.value(QStringLiteral("allowResample")).toBool(profile.allowResample);
    return profile;
}

QString playbackProfileToJson(const PlaybackProfile &profile)
{
    QJsonObject root;
    root.insert(QStringLiteral("id"), profile.id);
    root.insert(QStringLiteral("name"), profile.name);
    root.insert(QStringLiteral("backend"), profile.backend);
    root.insert(QStringLiteral("mode"), profile.mode);
    root.insert(QStringLiteral("sink"), profile.sink);
    root.insert(QStringLiteral("device"), profile.device);
    root.insert(QStringLiteral("softwareVolume"), profile.softwareVolume);
    root.insert(QStringLiteral("replayGain"), profile.replayGain);
    root.insert(QStringLiteral("allowResample"), profile.allowResample);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QJsonObject trackToJson(const Track &track)
{
    QJsonObject root;
    root.insert(QStringLiteral("path"), track.path);
    root.insert(QStringLiteral("parentDir"), track.parentDir);
    root.insert(QStringLiteral("filename"), track.filename);
    root.insert(QStringLiteral("title"), track.title);
    root.insert(QStringLiteral("artistName"), track.artistName);
    root.insert(QStringLiteral("albumArtistName"), track.albumArtistName);
    root.insert(QStringLiteral("albumTitle"), track.albumTitle);
    root.insert(QStringLiteral("date"), track.date);
    root.insert(QStringLiteral("originalDate"), track.originalDate);
    root.insert(QStringLiteral("trackNumber"), track.trackNumber);
    root.insert(QStringLiteral("discNumber"), track.discNumber);
    root.insert(QStringLiteral("durationMs"), QString::number(track.durationMs));
    root.insert(QStringLiteral("rating0To100"), track.rating0To100);
    root.insert(QStringLiteral("effectiveRating0To100"), track.effectiveRating0To100);
    root.insert(QStringLiteral("hasUserRating"), track.hasUserRating);
    root.insert(QStringLiteral("fileSize"), QString::number(track.fileSize));
    return root;
}

Track trackFromJson(const QJsonObject &root)
{
    Track track;
    track.path = root.value(QStringLiteral("path")).toString();
    track.parentDir = root.value(QStringLiteral("parentDir")).toString();
    track.filename = root.value(QStringLiteral("filename")).toString(QFileInfo(track.path).fileName());
    track.title = root.value(QStringLiteral("title")).toString();
    track.artistName = root.value(QStringLiteral("artistName")).toString();
    track.albumArtistName = root.value(QStringLiteral("albumArtistName")).toString();
    track.albumTitle = root.value(QStringLiteral("albumTitle")).toString();
    track.date = root.value(QStringLiteral("date")).toString();
    track.originalDate = root.value(QStringLiteral("originalDate")).toString();
    track.trackNumber = root.value(QStringLiteral("trackNumber")).toInt();
    track.discNumber = root.value(QStringLiteral("discNumber")).toInt();
    track.durationMs = root.value(QStringLiteral("durationMs")).toString().toLongLong();
    track.rating0To100 = root.value(QStringLiteral("rating0To100")).toInt(Rating::unset);
    track.effectiveRating0To100 = root.value(QStringLiteral("effectiveRating0To100")).toInt(track.rating0To100);
    track.hasUserRating = root.value(QStringLiteral("hasUserRating")).toBool();
    track.fileSize = root.value(QStringLiteral("fileSize")).toString().toLongLong();
    return track;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("muzaiten"));
    qRegisterMetaType<RatingTagSyncSummary>("RatingTagSyncSummary");
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
    m_stopScanButton = new QPushButton(QStringLiteral("Stop scan"), this);
    m_stopScanButton->setVisible(false);
    m_stopScanButton->setToolTip(QStringLiteral("Cancel the current library scan"));
    statusBar()->addPermanentWidget(m_stopScanButton);

    m_playback = new GStreamerPlaybackBackend(this);

    m_database = std::make_unique<Database>(QStringLiteral("main-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!m_database->open(databasePath())) {
        QMessageBox::warning(this, QStringLiteral("Database"), m_database->lastError());
    }

    m_listenBrainzThread = new QThread(this);
    m_listenBrainzScrobbler = new ListenBrainzScrobbler;
    m_listenBrainzScrobbler->moveToThread(m_listenBrainzThread);
    connect(m_listenBrainzThread, &QThread::finished, m_listenBrainzScrobbler, &QObject::deleteLater);
    connect(m_listenBrainzScrobbler, &ListenBrainzScrobbler::submissionFailed, this, [this](const QString &message) {
        statusBar()->showMessage(message, 10000);
    });
    connect(m_listenBrainzScrobbler, &ListenBrainzScrobbler::disabledAfterFailures, this, [this](const QString &message) {
        m_database->setSetting(QStringLiteral("listenbrainz.enabled"), QStringLiteral("false"));
        m_playerBar->setListenBrainzEnabled(false);
        statusBar()->showMessage(message, 15000);
        QMessageBox::warning(this, QStringLiteral("ListenBrainz"), message);
    });
    m_listenBrainzThread->start();

    connect(m_artistSidebar, &ArtistSidebar::artistSelected, this, &MainWindow::selectArtist);
    connect(m_stopScanButton, &QPushButton::clicked, this, &MainWindow::cancelScan);
    connect(m_artistSidebar, &ArtistSidebar::librarySourceChanged, this, &MainWindow::onLibrarySourceChanged);
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
    connect(m_playerBar, &PlayerBar::syncCurrentTrackRatingTagsRequested, this, &MainWindow::syncCurrentTrackRatingTags);
    connect(m_playerBar, &PlayerBar::syncCurrentArtistRatingTagsRequested, this, &MainWindow::syncCurrentArtistRatingTags);
    connect(m_playerBar, &PlayerBar::syncAllSavedRatingTagsRequested, this, &MainWindow::syncAllSavedRatingTags);
    connect(m_playerBar, &PlayerBar::retryPendingRatingTagsRequested, this, &MainWindow::retryPendingRatingTags);
    connect(m_playerBar, &PlayerBar::playbackProfileRequested, this, &MainWindow::configurePlaybackProfile);
    connect(m_playerBar, &PlayerBar::linkRootsRequested, this, &MainWindow::configureLinkRoots);
    connect(m_playerBar, &PlayerBar::mpdSourceRequested, this, &MainWindow::configureMpdSource);
    connect(m_playerBar, &PlayerBar::mpdImportRequested, this, &MainWindow::importMpdLibraryMetadata);
    connect(m_playerBar, &PlayerBar::compactMenuChanged, this, &MainWindow::applyCompactMenu);
    connect(m_playerBar, &PlayerBar::trackInfoPaneVisibleChanged, this, &MainWindow::applyTrackInfoPaneVisible);
    connect(m_playerBar, &PlayerBar::trackInfoPaneSettingsRequested, this, &MainWindow::configureTrackInfoPanel);
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
    connect(m_rightSidebar, &RightSidebar::artistRequested, this, &MainWindow::jumpToTrackInfoArtist);
    connect(m_rightSidebar, &RightSidebar::albumRequested, this, &MainWindow::jumpToTrackInfoAlbum);
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
    loadPlaybackProfile();
    loadViewSettings();
    loadQueueState();
    loadExplorerState();
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
    if (m_mpdImportThread != nullptr) {
        m_mpdImportThread->quit();
        m_mpdImportThread->wait(3000);
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
    saveQueueState();
    saveExplorerState();
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
    m_stopScanButton->setEnabled(true);
    m_stopScanButton->setVisible(true);
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

void MainWindow::cancelScan()
{
    if (m_scanWorker == nullptr) {
        return;
    }

    m_scanWorker->cancel();
    m_stopScanButton->setEnabled(false);
    statusBar()->showMessage(QStringLiteral("Canceling scan..."), 5000);
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
    m_stopScanButton->setVisible(false);
    m_stopScanButton->setEnabled(false);
    statusBar()->showMessage(
        canceled
            ? QStringLiteral("Scan canceled: %1 files visited, %2 tracks indexed").arg(visitedFiles).arg(indexedTracks)
            : QStringLiteral("Scan complete: %1 files visited, %2 tracks indexed").arg(visitedFiles).arg(indexedTracks),
        10000);
    refreshArtists();
}

void MainWindow::loadExistingLibrary()
{
    const qint64 mpdSourceId = m_database->mpdSourceId();
    m_artistSidebar->setMpdAvailable(mpdSourceId > 0);
    m_artistSidebar->setLibrarySourceIndex(m_librarySource == LibrarySource::Mpd ? 1 : 0);
    if (m_librarySource == LibrarySource::Mpd && mpdSourceId <= 0) {
        m_librarySource = LibrarySource::Local;
        m_artistSidebar->setLibrarySourceIndex(0);
    }
    refreshArtists();
}

void MainWindow::refreshArtists()
{
    if (m_librarySource == LibrarySource::Mpd) {
        const qint64 sourceId = m_database->mpdSourceId();
        m_artistSidebar->setMpdAvailable(sourceId > 0);
        const QVector<Artist> artists = m_database->mpdAlbumArtists();
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
        return;
    }

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
    saveExplorerState();
}

void MainWindow::selectAlbumFilter(const QString &albumTitle)
{
    rememberTrackTableViewState();
    m_selectedAlbumTitle = (m_selectedAlbumTitle == albumTitle) ? QString() : albumTitle;
    refreshAlbumGrid();
    refreshTrackTable();
    restoreTrackTableViewState();
    saveExplorerState();
}

void MainWindow::refreshAlbumGrid()
{
    if (m_currentArtist.isEmpty()) {
        return;
    }
    if (m_librarySource == LibrarySource::Mpd) {
        m_albumGrid->setArtworkCacheRoot(mpdCacheRoot());
        m_albumGrid->setAlbums(m_database->mpdAlbumsForArtist(m_currentArtist, mpdMusicDirectory()));
    } else {
        m_albumGrid->setArtworkCacheRoot(cacheRoot());
        m_albumGrid->setAlbums(m_database->albumsForArtist(m_currentArtist));
    }
    m_albumGrid->setSelectedAlbumTitle(m_selectedAlbumTitle);
}

void MainWindow::refreshTrackTable()
{
    if (m_currentArtist.isEmpty()) {
        return;
    }
    if (m_librarySource == LibrarySource::Mpd) {
        m_trackTable->setTracks(m_database->mpdTracksForArtist(m_currentArtist, mpdMusicDirectory(), m_selectedAlbumTitle));
    } else {
        m_trackTable->setTracks(m_database->tracksForArtist(m_currentArtist, m_selectedAlbumTitle));
    }
}

void MainWindow::applyTrackRating(const Track &track, int rating0To100)
{
    const bool ok = rating0To100 < 0 ? m_database->clearUserTrackRating(track.path) : m_database->setUserTrackRating(track.path, rating0To100);
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("Rating"), m_database->lastError());
        return;
    }
    if (rating0To100 >= 0) {
        m_database->setPendingTrackRatingWrite(track.path, rating0To100, QStringLiteral("pending"));
    } else {
        m_database->clearPendingTrackRatingWrite(track.path);
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
        m_rightSidebar->setTrackInfo(m_currentTrack);
    }
    m_rightSidebar->setQueue(m_queue);
    m_rightSidebar->setCurrentIndex(m_queueIndex);
    restoreTrackTableViewState();

    if (rating0To100 >= 0 && m_librarySource == LibrarySource::Local) {
        Track syncTrack = track;
        syncTrack.hasUserRating = true;
        syncTrack.effectiveRating0To100 = rating0To100;
        startRatingTagSync({syncTrack}, static_cast<int>(RatingTagSyncRequest::Scope::Track));
    }
}

void MainWindow::startRatingTagSync(const QVector<Track> &tracks, int scope)
{
    if (tracks.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("No rating tags to sync"), 5000);
        return;
    }

    RatingTagSyncRequest request;
    request.scope = static_cast<RatingTagSyncRequest::Scope>(scope);
    request.tracks = tracks;
    request.linkRoots = m_database->linkRoots();

    auto *thread = new QThread(this);
    auto *worker = new RatingTagSyncWorker(databasePath(), request);
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, &RatingTagSyncWorker::run);
    connect(worker, &RatingTagSyncWorker::progress, this, [this](int checked, int total, const QString &) {
        statusBar()->showMessage(QStringLiteral("Rating tag sync: %1 / %2 checked").arg(checked).arg(total));
    });
    connect(worker, &RatingTagSyncWorker::finished, this, [this, thread, worker](const RatingTagSyncSummary &summary, const QString &error) {
        if (!error.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Rating tag sync"), error);
        } else {
            statusBar()->showMessage(QStringLiteral("Rating tag sync complete: %1 written, %2 tag-won, %3 no writable path, %4 failed")
                                         .arg(summary.written)
                                         .arg(summary.tagWon)
                                         .arg(summary.noWritablePath)
                                         .arg(summary.failed),
                                     10000);
        }
        rememberTrackTableViewState();
        refreshTrackTable();
        refreshAlbumGrid();
        for (Track &queuedTrack : m_queue) {
            const QVector<Track> refreshed = m_database->tracksForArtist(queuedTrack.albumArtistName, queuedTrack.albumTitle);
            const auto it = std::find_if(refreshed.cbegin(), refreshed.cend(), [&queuedTrack](const Track &track) {
                return track.path == queuedTrack.path;
            });
            if (it != refreshed.cend()) {
                queuedTrack.rating0To100 = it->rating0To100;
                queuedTrack.hasUserRating = it->hasUserRating;
                queuedTrack.effectiveRating0To100 = it->effectiveRating0To100;
            }
        }
        if (!m_currentTrack.path.isEmpty()) {
            const QVector<Track> refreshed = m_database->tracksForArtist(m_currentTrack.albumArtistName, m_currentTrack.albumTitle);
            const auto it = std::find_if(refreshed.cbegin(), refreshed.cend(), [this](const Track &track) {
                return track.path == m_currentTrack.path;
            });
            if (it != refreshed.cend()) {
                m_currentTrack = *it;
                const QString title = m_currentTrack.title.isEmpty() ? m_currentTrack.filename : m_currentTrack.title;
                QString subtitle = QStringLiteral("%1 - %2").arg(m_currentTrack.artistName, m_currentTrack.albumTitle);
                if (!m_currentTrack.date.isEmpty()) {
                    subtitle += QStringLiteral(" (%1)").arg(m_currentTrack.date.left(4));
                }
                m_playerBar->setTrackInfo(title, subtitle, m_currentTrack.effectiveRating0To100);
                m_rightSidebar->setTrackInfo(m_currentTrack);
            }
        }
        m_rightSidebar->setQueue(m_queue);
        m_rightSidebar->setCurrentIndex(m_queueIndex);
        saveQueueState();
        restoreTrackTableViewState();
        worker->deleteLater();
        thread->quit();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MainWindow::syncCurrentTrackRatingTags()
{
    if (m_librarySource != LibrarySource::Local || m_currentTrack.path.isEmpty() || m_currentTrack.effectiveRating0To100 < 0) {
        statusBar()->showMessage(QStringLiteral("No current local rated track to sync"), 5000);
        return;
    }
    startRatingTagSync({m_currentTrack}, static_cast<int>(RatingTagSyncRequest::Scope::Track));
}

void MainWindow::syncCurrentArtistRatingTags()
{
    if (m_librarySource != LibrarySource::Local || m_currentArtist.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("No current local artist to sync"), 5000);
        return;
    }
    QVector<Track> tracks;
    const QVector<Track> userRated = m_database->tracksWithUserRatings();
    const QVector<Track> pending = m_database->tracksWithPendingRatingWrites();
    for (const Track &track : userRated + pending) {
        const bool alreadyQueued = std::any_of(tracks.cbegin(), tracks.cend(), [&track](const Track &queued) {
            return queued.path == track.path;
        });
        if (track.albumArtistName == m_currentArtist && !alreadyQueued) {
            tracks.push_back(track);
        }
    }
    startRatingTagSync(tracks, static_cast<int>(RatingTagSyncRequest::Scope::CurrentArtist));
}

void MainWindow::syncAllSavedRatingTags()
{
    startRatingTagSync(m_database->tracksWithUserRatings(), static_cast<int>(RatingTagSyncRequest::Scope::SavedRatedTracks));
}

void MainWindow::retryPendingRatingTags()
{
    startRatingTagSync(m_database->tracksWithPendingRatingWrites(), static_cast<int>(RatingTagSyncRequest::Scope::PendingWrites));
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
    const QString rightSidebarSettings = m_database->setting(QStringLiteral("rightSidebar.view"));
    m_rightSidebar->applyViewSettingsJson(rightSidebarSettings);
    m_playerBar->setTrackInfoPaneVisible(QJsonDocument::fromJson(rightSidebarSettings.toUtf8()).object().value(QStringLiteral("showTrackInfo")).toBool(true));
    const QJsonObject playerBar = QJsonDocument::fromJson(m_database->setting(QStringLiteral("playerBar.view")).toUtf8()).object();
    m_playerBar->setCompactMenu(playerBar.value(QStringLiteral("compactMenu")).toBool(false));
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

void MainWindow::loadQueueState()
{
    const QJsonObject root = QJsonDocument::fromJson(m_database->setting(QStringLiteral("queue.state")).toUtf8()).object();
    const QJsonArray tracks = root.value(QStringLiteral("tracks")).toArray();
    m_queue.clear();
    m_queue.reserve(tracks.size());
    for (const QJsonValue &value : tracks) {
        const Track track = trackFromJson(value.toObject());
        if (!track.path.isEmpty()) {
            m_queue.push_back(track);
        }
    }

    m_queueIndex = std::clamp(root.value(QStringLiteral("index")).toInt(-1), -1, static_cast<int>(m_queue.size()) - 1);
    m_playNextInsertIndex = std::clamp(root.value(QStringLiteral("playNextInsertIndex")).toInt(m_queueIndex + 1), 0, static_cast<int>(m_queue.size()));
    m_rightSidebar->setQueue(m_queue);
    m_rightSidebar->setCurrentIndex(m_queueIndex);
    if (m_queueIndex >= 0 && m_queueIndex < m_queue.size()) {
        presentTrack(m_queue.at(m_queueIndex), false);
    }
}

void MainWindow::saveQueueState()
{
    QJsonArray tracks;
    for (const Track &track : m_queue) {
        tracks.append(trackToJson(track));
    }

    QJsonObject root;
    root.insert(QStringLiteral("tracks"), tracks);
    root.insert(QStringLiteral("index"), m_queueIndex);
    root.insert(QStringLiteral("playNextInsertIndex"), m_playNextInsertIndex);
    m_database->setSetting(QStringLiteral("queue.state"), QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

void MainWindow::loadExplorerState()
{
    const QJsonObject root = QJsonDocument::fromJson(m_database->setting(QStringLiteral("libraryExplorer.state")).toUtf8()).object();
    m_librarySource = root.value(QStringLiteral("source")).toString() == QStringLiteral("mpd") ? LibrarySource::Mpd : LibrarySource::Local;
    m_currentArtist = root.value(QStringLiteral("artist")).toString();
    m_selectedAlbumTitle = root.value(QStringLiteral("album")).toString();
}

void MainWindow::saveExplorerState()
{
    QJsonObject root;
    root.insert(QStringLiteral("source"), m_librarySource == LibrarySource::Mpd ? QStringLiteral("mpd") : QStringLiteral("local"));
    root.insert(QStringLiteral("artist"), m_currentArtist);
    root.insert(QStringLiteral("album"), m_selectedAlbumTitle);
    m_database->setSetting(QStringLiteral("libraryExplorer.state"), QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

void MainWindow::applySharedTableSettings()
{
    const QJsonObject sharedSettings = QJsonDocument::fromJson(m_database->setting(QStringLiteral("tables.view")).toUtf8()).object();
    const QJsonObject trackSettings = QJsonDocument::fromJson(m_database->setting(QStringLiteral("trackTable.view")).toUtf8()).object();
    const QJsonObject sidebarSettings = QJsonDocument::fromJson(m_database->setting(QStringLiteral("rightSidebar.view")).toUtf8()).object();
    const int headerHeight = sharedSettings.value(QStringLiteral("headerHeight")).toInt(trackSettings.value(QStringLiteral("headerHeight")).toInt(sidebarSettings.value(QStringLiteral("headerHeight")).toInt(20)));
    m_trackTable->setHeaderHeight(headerHeight);
    m_rightSidebar->setHeaderHeight(headerHeight);

    QJsonObject shared;
    shared.insert(QStringLiteral("headerHeight"), headerHeight);
    m_database->setSetting(QStringLiteral("tables.view"), QString::fromUtf8(QJsonDocument(shared).toJson(QJsonDocument::Compact)));
}

void MainWindow::applyTrackInfoPaneVisible(bool visible)
{
    m_playerBar->setTrackInfoPaneVisible(visible);
    m_rightSidebar->setTrackInfoVisible(visible);
    saveRightSidebarViewSettings();
}

void MainWindow::applyCompactMenu(bool compact)
{
    m_playerBar->setCompactMenu(compact);
    QJsonObject root;
    root.insert(QStringLiteral("compactMenu"), compact);
    m_database->setSetting(QStringLiteral("playerBar.view"), QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

void MainWindow::loadPlaybackProfile()
{
    m_playbackProfile = playbackProfileFromJson(m_database->setting(QStringLiteral("playback.profile")));
    m_playback->setProfile(m_playbackProfile);
}

void MainWindow::savePlaybackProfile()
{
    m_database->setSetting(QStringLiteral("playback.profile"), playbackProfileToJson(m_playbackProfile));
}

void MainWindow::configurePlaybackProfile()
{
    PlaybackProfileDialog dialog(this);
    dialog.setProfile(m_playbackProfile);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    m_playbackProfile = dialog.profile();
    savePlaybackProfile();
    m_playback->setProfile(m_playbackProfile);
    statusBar()->showMessage(QStringLiteral("Playback output updated"), 3000);
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

void MainWindow::findTrackFile(const Track &track)
{
    if (track.path.isEmpty()) {
        return;
    }

    const PathResolver resolver(m_database->linkRoots());
    const PathResolution resolution = resolver.resolveLocalPath(track.path, PathUse::Read);
    if (resolution.preferredPath.isEmpty()) {
        QMessageBox::warning(this,
                             QStringLiteral("Find file"),
                             QStringLiteral("%1\n\nCandidates:\n%2")
                                 .arg(resolution.failureReason, resolution.candidates.join(QLatin1Char('\n'))));
        return;
    }

    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(resolution.preferredPath).absolutePath()));
    statusBar()->showMessage(QStringLiteral("Resolved %1").arg(resolution.preferredPath), 5000);
}

void MainWindow::configureTrackInfoPanel()
{
    m_rightSidebar->configureTrackInfoPanel(this);
}

void MainWindow::jumpToTrackInfoArtist(const QString &artistName)
{
    if (artistName.isEmpty()) {
        return;
    }
    if (m_artistSidebar->selectArtist(artistName)) {
        selectArtist(artistName);
    }
}

void MainWindow::jumpToTrackInfoAlbum(const QString &artistName, const QString &albumTitle)
{
    if (artistName.isEmpty()) {
        return;
    }
    jumpToTrackInfoArtist(artistName);
    if (!albumTitle.isEmpty() && m_selectedAlbumTitle != albumTitle) {
        selectAlbumFilter(albumTitle);
    }
}

void MainWindow::configureMpdSource()
{
    QString startPath = m_database->setting(QStringLiteral("mpd.configPath"));
    if (startPath.isEmpty()) {
        for (const QString &candidate : MpdConfigParser::defaultConfigCandidates()) {
            if (QFileInfo::exists(candidate)) {
                startPath = candidate;
                break;
            }
        }
    }

    const QString path = QFileDialog::getOpenFileName(this,
                                                      QStringLiteral("Choose MPD config"),
                                                      startPath.isEmpty() ? QDir::homePath() : startPath,
                                                      QStringLiteral("MPD config (*.conf);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    QString error;
    const MpdConfig config = MpdConfigParser::parseFile(path, &error);
    if (config.musicDirectory.isEmpty()) {
        QMessageBox::warning(this,
                             QStringLiteral("MPD source"),
                             error.isEmpty()
                                 ? QStringLiteral("Could not find music_directory in %1").arg(path)
                                 : QStringLiteral("%1\n%2").arg(path, error));
        return;
    }

    m_database->setSetting(QStringLiteral("mpd.configPath"), path);
    m_database->setSetting(QStringLiteral("mpd.musicDirectory"), config.musicDirectory);
    m_database->setSetting(QStringLiteral("mpd.playlistDirectory"), config.playlistDirectory);
    statusBar()->showMessage(QStringLiteral("MPD music directory: %1").arg(config.musicDirectory), 5000);
}

void MainWindow::importMpdLibraryMetadata()
{
    if (m_mpdImportThread != nullptr) {
        statusBar()->showMessage(QStringLiteral("MPD import is already running"), 5000);
        return;
    }

    const QString musicDirectory = mpdMusicDirectory();
    if (musicDirectory.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("MPD import"), QStringLiteral("Configure an MPD source first."));
        return;
    }

    const QString host = m_database->setting(QStringLiteral("mpd.host"), QStringLiteral("127.0.0.1"));
    const quint16 port = static_cast<quint16>(m_database->setting(QStringLiteral("mpd.port"), QStringLiteral("6600")).toUShort());
    const QString configPath = m_database->setting(QStringLiteral("mpd.configPath"));

    m_mpdImportThread = new QThread(this);
    m_mpdImportWorker = new MpdImportWorker(databasePath(), configPath, musicDirectory, host, port, 5000);
    m_mpdImportWorker->moveToThread(m_mpdImportThread);

    connect(m_mpdImportThread, &QThread::started, m_mpdImportWorker, &MpdImportWorker::run);
    connect(m_mpdImportWorker, &MpdImportWorker::progress, this, [this](int imported, int total) {
        statusBar()->showMessage(QStringLiteral("Importing MPD metadata: %1 / %2 tracks").arg(imported).arg(total));
    });
    connect(m_mpdImportWorker, &MpdImportWorker::finished, this, [this](int imported, const QString &error) {
        if (!error.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("MPD import"), error);
        } else {
            statusBar()->showMessage(QStringLiteral("Imported %1 MPD tracks").arg(imported), 10000);
            const qint64 mpdSourceId = m_database->mpdSourceId();
            m_artistSidebar->setMpdAvailable(mpdSourceId > 0);
            if (m_librarySource == LibrarySource::Mpd) {
                refreshArtists();
            }
        }
        m_mpdImportThread->quit();
    });
    connect(m_mpdImportWorker, &MpdImportWorker::finished, m_mpdImportWorker, &QObject::deleteLater);
    connect(m_mpdImportThread, &QThread::finished, m_mpdImportThread, &QObject::deleteLater);
    connect(m_mpdImportThread, &QThread::finished, this, [this]() {
        m_mpdImportThread = nullptr;
        m_mpdImportWorker = nullptr;
    });

    statusBar()->showMessage(QStringLiteral("Starting MPD import from %1:%2").arg(host).arg(port));
    m_mpdImportThread->start();
}

void MainWindow::onLibrarySourceChanged(int index)
{
    m_librarySource = (index == 1) ? LibrarySource::Mpd : LibrarySource::Local;

    if (m_librarySource == LibrarySource::Mpd && m_database->mpdSourceId() <= 0) {
        m_artistSidebar->setMpdAvailable(false);
        m_currentArtist.clear();
        m_selectedAlbumTitle.clear();
        m_albumGrid->setAlbums({});
        m_trackTable->setTracks({});
        saveExplorerState();
        statusBar()->showMessage(QStringLiteral("No MPD source configured. Use the menu to configure and import."), 5000);
        return;
    }

    m_albumGrid->setArtworkCacheRoot(
        m_librarySource == LibrarySource::Mpd ? mpdCacheRoot() : cacheRoot());

    m_currentArtist.clear();
    m_selectedAlbumTitle.clear();
    saveExplorerState();
    refreshArtists();
}

QString MainWindow::mpdMusicDirectory() const
{
    QString musicDirectory = m_database->setting(QStringLiteral("mpd.musicDirectory"));
    if (!musicDirectory.isEmpty()) {
        return musicDirectory;
    }

    const QString configPath = m_database->setting(QStringLiteral("mpd.configPath"));
    if (!configPath.isEmpty()) {
        QString error;
        const MpdConfig config = MpdConfigParser::parseFile(configPath, &error);
        Q_UNUSED(error)
        if (!config.musicDirectory.isEmpty()) {
            return config.musicDirectory;
        }
    }

    for (const QString &candidate : MpdConfigParser::defaultConfigCandidates()) {
        if (!QFileInfo::exists(candidate)) {
            continue;
        }
        QString error;
        const MpdConfig config = MpdConfigParser::parseFile(candidate, &error);
        Q_UNUSED(error)
        if (!config.musicDirectory.isEmpty()) {
            return config.musicDirectory;
        }
    }

    return {};
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

    const QString playbackPath = resolvedReadPathForTrack(track);
    if (playbackPath.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Playback"), QStringLiteral("Could not resolve a readable file for %1").arg(track.path));
        return;
    }

    presentTrack(track);
    m_playback->play(QUrl::fromLocalFile(playbackPath));
    prepareNextQueueTrack();
}

void MainWindow::presentTrack(const Track &track, bool notifyScrobbler)
{
    m_currentTrack = track;
    updateCurrentAlbumArt();
    const QString title = track.title.isEmpty() ? track.filename : track.title;
    QString subtitle = QStringLiteral("%1 - %2").arg(track.artistName, track.albumTitle);
    if (!track.date.isEmpty()) {
        subtitle += QStringLiteral(" (%1)").arg(track.date.left(4));
    }
    m_playerBar->setTrackInfo(title, subtitle, track.effectiveRating0To100);
    m_rightSidebar->setTrackInfo(track);
    m_playerBar->setPosition(0, track.durationMs);
    if (notifyScrobbler) {
        QMetaObject::invokeMethod(m_listenBrainzScrobbler, "trackStarted", Qt::QueuedConnection, Q_ARG(Track, track));
        statusBar()->showMessage(QStringLiteral("Playing %1").arg(title), 3000);
    }
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
    saveQueueState();
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
        saveQueueState();
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
    saveQueueState();
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
    saveQueueState();
}

void MainWindow::playNextAlbum(const QString &albumTitle)
{
    if (!m_currentArtist.isEmpty() && !albumTitle.isEmpty()) {
        if (m_librarySource == LibrarySource::Mpd) {
            playNextTracks(m_database->mpdTracksForArtist(m_currentArtist, mpdMusicDirectory(), albumTitle));
        } else {
            playNextTracks(m_database->tracksForArtist(m_currentArtist, albumTitle));
        }
    }
}

void MainWindow::addAlbumToQueue(const QString &albumTitle)
{
    if (!m_currentArtist.isEmpty() && !albumTitle.isEmpty()) {
        if (m_librarySource == LibrarySource::Mpd) {
            addTracksToQueue(m_database->mpdTracksForArtist(m_currentArtist, mpdMusicDirectory(), albumTitle));
        } else {
            addTracksToQueue(m_database->tracksForArtist(m_currentArtist, albumTitle));
        }
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
    saveQueueState();
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
    const QString nextPath = resolvedReadPathForTrack(nextTrack);
    m_playback->prepareNext(nextPath.isEmpty() ? QUrl() : QUrl::fromLocalFile(nextPath));
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
    saveQueueState();
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
    const QString cache = (m_librarySource == LibrarySource::Mpd) ? mpdCacheRoot() : cacheRoot();
    const ArtworkResolver resolver(cache);
    const QString resolvedPath = resolvedReadPathForTrack(m_currentTrack);
    const QString directory = resolvedPath.isEmpty() ? m_currentTrack.parentDir : QFileInfo(resolvedPath).absolutePath();
    const ArtworkResult artwork = resolver.resolveForDirectory(directory);
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

QString MainWindow::mpdCacheRoot() const
{
    const QString root = stateRoot() + QStringLiteral("/cache");
    QDir().mkpath(root);
    return QDir(root).filePath(QStringLiteral("mpd-artwork"));
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

QString MainWindow::resolvedReadPathForTrack(const Track &track) const
{
    if (track.path.isEmpty()) {
        return {};
    }

    const PathResolver resolver(m_database->linkRoots());
    const PathResolution resolution = resolver.resolveLocalPath(track.path, PathUse::Read);
    return resolution.preferredPath;
}
