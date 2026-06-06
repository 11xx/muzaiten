#include "ui/MainWindow.h"

#include "Version.h"
#include "app/AppPaths.h"
#include "core/MusicSort.h"
#include "core/Rating.h"
#include "db/Database.h"
#include "db/SettingsStore.h"
#include "fs/LinkRoot.h"
#include "playback/GStreamerPlaybackBackend.h"
#include "playback/PlaybackBackend.h"
#include "mpd/MpdConfig.h"
#include "mpd/MpdImportWorker.h"
#include "mpris/MprisService.h"
#include "scanner/ArtworkCache.h"
#include "scanner/ScanPipeline.h"
#include "scanner/RatingTagSyncWorker.h"
#include "scrobble/LastFmCredentials.h"
#include "scrobble/LastFmScrobbler.h"
#include "scrobble/ListenBrainzScrobbler.h"
#include "ui/AlbumGrid.h"
#include "ui/ArtistSidebar.h"
#include "ui/FileExplorerKeybindings.h"
#include "ui/FileExplorerView.h"
#include "ui/KeybindingsDialog.h"
#include "ui/LinkRootsDialog.h"
#include "ui/PlayerBar.h"
#include "ui/PanelOrderDialog.h"
#include "ui/PanelSearchController.h"
#include "ui/PlaybackProfileDialog.h"
#include "ui/PlaybackResumeDialog.h"
#include "ui/QueueKeybindings.h"
#include "ui/QueueScreen.h"
#include "ui/QueueStore.h"
#include "ui/RankingDialog.h"
#include "ui/RightSidebar.h"
#include "ui/SearchView.h"
#include "ui/SourceDirectoriesDialog.h"
#include "ui/MainPanelKeybindings.h"
#include "ui/TableNavigationScroll.h"
#include "search/RankConfig.h"
#include "ui/TrackTable.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QCloseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QUrl>
#include <QVBoxLayout>
#include <QUuid>

#include <algorithm>

Q_LOGGING_CATEGORY(uiLog, "muzaiten.ui")

namespace {

// Album titles are joined into one key string (for persistence and change
// detection) and split back. Use the ASCII Unit Separator (0x1F), which cannot
// occur in a real tag value, so a title that itself contains a newline still
// round-trips to a single filter entry instead of several bogus ones.
constexpr char16_t kAlbumFilterSeparator = u'\x1F';

QString albumFilterKey(const QStringList &albumTitles)
{
    return albumTitles.join(QChar(kAlbumFilterSeparator));
}

QStringList normalizedAlbumTitles(QStringList albumTitles)
{
    albumTitles.removeAll(QString());
    albumTitles.removeDuplicates();
    return albumTitles;
}

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
    const QString mode = root.value(QStringLiteral("mode")).toString(profile.mode);
    profile.mode = mode == QStringLiteral("exclusive") ? QStringLiteral("bit-perfect") : mode;
    profile.sink = root.value(QStringLiteral("sink")).toString(profile.sink);
    profile.device = root.value(QStringLiteral("device")).toString(profile.device);
    profile.softwareVolume = root.value(QStringLiteral("softwareVolume")).toBool(profile.softwareVolume);
    profile.replayGain = root.value(QStringLiteral("replayGain")).toBool(profile.replayGain);
    profile.allowResample = root.value(QStringLiteral("allowResample")).toBool(profile.allowResample);
    profile.releaseSinkOnPause = root.value(QStringLiteral("releaseSinkOnPause")).toBool(profile.releaseSinkOnPause);
    if (root.contains(QStringLiteral("readAheadMb"))) {
        profile.readAheadMb = std::clamp(
            root.value(QStringLiteral("readAheadMb")).toInt(profile.readAheadMb), 0, 1024);
    } else {
        // Migrate the legacy "preload into RAM" percentage: any non-zero value
        // meant "whole file in RAM", which maps to read-ahead enabled.
        const int legacyPreload = root.value(QStringLiteral("preloadPercent")).toInt(0);
        profile.readAheadMb = legacyPreload > 0 ? 32 : 0;
    }
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
    root.insert(QStringLiteral("releaseSinkOnPause"), profile.releaseSinkOnPause);
    root.insert(QStringLiteral("readAheadMb"), profile.readAheadMb);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QString mainViewName(MainView view)
{
    switch (view) {
    case MainView::LibraryPanels:
        return QStringLiteral("libraryPanels");
    case MainView::LibraryFileExplorer:
        return QStringLiteral("libraryFileExplorer");
    case MainView::FreeRoamFileExplorer:
        return QStringLiteral("freeRoamFileExplorer");
    case MainView::Search:
        return QStringLiteral("search");
    case MainView::Queue:
        return QStringLiteral("queue");
    }
    return QStringLiteral("libraryPanels");
}

MainView mainViewFromName(const QString &name)
{
    if (name == QStringLiteral("libraryFileExplorer")) {
        return MainView::LibraryFileExplorer;
    }
    if (name == QStringLiteral("freeRoamFileExplorer")) {
        return MainView::FreeRoamFileExplorer;
    }
    if (name == QStringLiteral("search")) {
        return MainView::Search;
    }
    if (name == QStringLiteral("queue")) {
        return MainView::Queue;
    }
    return MainView::LibraryPanels;
}

QString playbackStateName(PlaybackBackend::State state)
{
    switch (state) {
    case PlaybackBackend::State::Playing:
        return QStringLiteral("playing");
    case PlaybackBackend::State::Paused:
        return QStringLiteral("paused");
    case PlaybackBackend::State::Buffering:
        return QStringLiteral("buffering");
    case PlaybackBackend::State::Error:
        return QStringLiteral("error");
    case PlaybackBackend::State::Stopped:
        break;
    }
    return QStringLiteral("stopped");
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

QString cleanDirectoryPath(const QString &path)
{
    const QString cleaned = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (cleaned.size() > 1 && cleaned.endsWith(QLatin1Char('/'))) {
        return cleaned.left(cleaned.size() - 1);
    }
    return cleaned;
}

bool isDirectoryCoveredBy(const QString &child, const QString &parent)
{
    return child != parent && child.startsWith(parent + QLatin1Char('/'));
}

QVector<ScanRoot> deduplicatedScanRoots(QVector<ScanRoot> roots)
{
    for (ScanRoot &root : roots) {
        root.path = cleanDirectoryPath(root.path);
    }
    std::sort(roots.begin(), roots.end(), [](const ScanRoot &left, const ScanRoot &right) {
        if (left.path.size() == right.path.size()) {
            return left.path < right.path;
        }
        return left.path.size() < right.path.size();
    });

    QVector<ScanRoot> deduped;
    for (const ScanRoot &root : roots) {
        if (!root.scanEnabled || root.path.isEmpty()) {
            continue;
        }
        const bool covered = std::any_of(deduped.cbegin(), deduped.cend(), [&root](const ScanRoot &existing) {
            return root.path == existing.path || isDirectoryCoveredBy(root.path, existing.path);
        });
        if (!covered) {
            deduped.push_back(root);
        }
    }
    return deduped;
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

    m_mainStack = new QStackedWidget(central);
    m_queueStore = new QueueStore(this);

    m_rootSplitter = new QSplitter(Qt::Horizontal, m_mainStack);
    m_artistSidebar = new ArtistSidebar(m_rootSplitter);

    m_centerSplitter = new QSplitter(Qt::Vertical, m_rootSplitter);
    m_albumGrid = new AlbumGrid(m_centerSplitter);
    m_trackTable = new TrackTable(m_centerSplitter);
    m_centerSplitter->setStretchFactor(0, 55);
    m_centerSplitter->setStretchFactor(1, 45);

    m_rightSidebar = new RightSidebar(m_rootSplitter);
    m_rightSidebar->setQueueStore(m_queueStore);

    m_rootSplitter->addWidget(m_artistSidebar);
    m_rootSplitter->addWidget(m_centerSplitter);
    m_rootSplitter->addWidget(m_rightSidebar);
    m_rootSplitter->setStretchFactor(0, 0);
    m_rootSplitter->setStretchFactor(1, 1);
    m_rootSplitter->setStretchFactor(2, 0);
    m_rootSplitter->setSizes({260, 900, 300});

    m_libraryFileExplorer = new FileExplorerView(m_mainStack);
    m_libraryFileExplorer->setMode(FileExplorerMode::Library);
    m_libraryFileExplorer->setModeTitle(QStringLiteral("Library Explorer"));
    m_freeRoamFileExplorer = new FileExplorerView(m_mainStack);
    m_freeRoamFileExplorer->setMode(FileExplorerMode::FreeRoam);
    m_freeRoamFileExplorer->setRootPath(QDir::homePath());
    m_freeRoamFileExplorer->setModeTitle(QStringLiteral("File System Explorer"));
    m_searchView = new SearchView(m_mainStack);
    m_queueScreen = new QueueScreen(m_mainStack);
    m_queueScreen->setQueueStore(m_queueStore);
    m_panelSearch = new PanelSearchController(central);

    m_mainStack->addWidget(m_rootSplitter);
    m_mainStack->addWidget(m_libraryFileExplorer);
    m_mainStack->addWidget(m_freeRoamFileExplorer);
    m_mainStack->addWidget(m_searchView);
    m_mainStack->addWidget(m_queueScreen);

    centralLayout->addWidget(m_mainStack, 1);
    centralLayout->addWidget(m_panelSearch, 0);
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
    m_mpris = new MprisService(this);
    m_playbackStateSaveTimer = new QTimer(this);
    m_playbackStateSaveTimer->setSingleShot(true);
    m_playbackStateSaveTimer->setInterval(2000);
    connect(m_playbackStateSaveTimer, &QTimer::timeout, this, [this]() {
        savePlaybackState();
    });

    m_database = std::make_unique<Database>(QStringLiteral("main-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!m_database->open(databasePath())) {
        QMessageBox::warning(this, QStringLiteral("Database"), m_database->lastError());
    }

    // UI/view prefs and session state live in a separate store under XDG_STATE_HOME
    // so they persist independently of the per-build library database.
    m_state = std::make_unique<SettingsStore>(QDir(AppPaths::stateDir()).filePath(QStringLiteral("state.sqlite")));

    // Create a commented config-file template on first run; its [paths] section
    // feeds AppPaths (below CLI/env/--state-root, above the XDG default).
    AppPaths::writeDefaultConfigIfMissing();

    const int artworkSize = std::clamp(m_state->setting(QStringLiteral("artwork.size"), QStringLiteral("1024")).toInt(), 128, 4096);
    m_artworkCache = std::make_unique<ArtworkCache>(QDir(AppPaths::cacheDir()).filePath(QStringLiteral("artwork.sqlite")), artworkSize);
    connect(m_artworkCache.get(), &ArtworkCache::artworkReady, this, &MainWindow::onArtworkReady);
    connect(m_artworkCache.get(), &ArtworkCache::artworkMissing, this, &MainWindow::onArtworkMissing);

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

    m_lastFmThread = new QThread(this);
    m_lastFmScrobbler = new LastFmScrobbler;
    m_lastFmScrobbler->moveToThread(m_lastFmThread);
    connect(m_lastFmThread, &QThread::finished, m_lastFmScrobbler, &QObject::deleteLater);
    connect(m_lastFmScrobbler, &LastFmScrobbler::submissionFailed, this, [this](const QString &message) {
        statusBar()->showMessage(message, 10000);
    });
    connect(m_lastFmScrobbler, &LastFmScrobbler::disabledAfterFailures, this, [this](const QString &message) {
        m_database->setSetting(QStringLiteral("lastfm.enabled"), QStringLiteral("false"));
        m_playerBar->setLastFmEnabled(false);
        statusBar()->showMessage(message, 15000);
        QMessageBox::warning(this, QStringLiteral("Last.fm"), message);
    });
    connect(m_lastFmScrobbler, &LastFmScrobbler::authenticationUrlReady, this, [this](const QUrl &url) {
        QDesktopServices::openUrl(url);
        statusBar()->showMessage(QStringLiteral("Authorize muzaiten in your browser; it will connect automatically."), 15000);
    });
    connect(m_lastFmScrobbler, &LastFmScrobbler::authenticationSucceeded, this, [this](const QString &username, const QString &sessionKey) {
        m_database->setSetting(QStringLiteral("lastfm.username"), username);
        m_database->setSetting(QStringLiteral("lastfm.sessionKey"), sessionKey);
        m_database->setSetting(QStringLiteral("lastfm.enabled"), QStringLiteral("true"));
        configureLastFm();
        statusBar()->showMessage(username.isEmpty() ? QStringLiteral("Last.fm authentication complete")
                                                    : QStringLiteral("Last.fm authenticated as %1").arg(username),
                                 8000);
    });
    connect(m_lastFmScrobbler, &LastFmScrobbler::authenticationFailed, this, [this](const QString &message) {
        statusBar()->showMessage(message, 10000);
        QMessageBox::warning(this, QStringLiteral("Last.fm"), message);
    });
    m_lastFmThread->start();

    connect(m_artistSidebar, &ArtistSidebar::artistSelected, this, &MainWindow::selectArtist);
    connect(m_stopScanButton, &QPushButton::clicked, this, &MainWindow::cancelScan);
    connect(m_artistSidebar, &ArtistSidebar::librarySourceChanged, this, &MainWindow::onLibrarySourceChanged);
    connect(m_trackTable, &TrackTable::trackActivated, this, &MainWindow::appendAndPlayTrack);
    connect(m_trackTable, &TrackTable::playNextRequested, this, &MainWindow::playNextTracks);
    connect(m_trackTable, &TrackTable::addToQueueRequested, this, &MainWindow::addTracksToQueue);
    connect(m_trackTable, &TrackTable::trackRatingChanged, this, &MainWindow::applyTrackRating);
    connect(m_trackTable, &TrackTable::viewSettingsChanged, this, &MainWindow::saveTrackTableViewSettings);
    connect(m_albumGrid, &AlbumGrid::albumSelectionToggled, this, &MainWindow::selectAlbumFilter);
    connect(m_albumGrid, &AlbumGrid::albumSelectionCleared, this, &MainWindow::clearAlbumFilter);
    connect(m_albumGrid, &AlbumGrid::albumSelectionNarrowRequested, this, &MainWindow::narrowAlbumFilters);
    connect(m_albumGrid, &AlbumGrid::albumPlayNextRequested, this, &MainWindow::playNextAlbum);
    connect(m_albumGrid, &AlbumGrid::albumAddToQueueRequested, this, &MainWindow::addAlbumToQueue);
    connect(m_albumGrid, &AlbumGrid::albumRatingChanged, this, &MainWindow::applyAlbumRating);
    connect(m_albumGrid, &AlbumGrid::viewSettingsChanged, this, &MainWindow::saveAlbumGridViewSettings);
    connect(m_artistSidebar, &ArtistSidebar::viewSettingsChanged, this, &MainWindow::saveArtistSidebarViewSettings);
    connect(m_rightSidebar, &RightSidebar::viewSettingsChanged, this, &MainWindow::saveRightSidebarViewSettings);
    m_panelSearch->registerTarget({
        MainPanelId::Queue,
        QStringLiteral("Queue"),
        m_rightSidebar->queueNavigationWidget(),
        [this]() { return m_rightSidebar->queueRowCount(); },
        [this]() { return m_rightSidebar->queueCurrentRow(); },
        [this](int row) { m_rightSidebar->setQueueCurrentRow(row); },
        [this](int row, int direction) { m_rightSidebar->setQueueCurrentRow(row, direction); },
        [this]() { m_rightSidebar->activateCurrentQueueTrack(); },
        {},
        {},
        {},
        {},
        {},
        {},
        {},
        {},
        {},
        {},
        [this]() { return m_rightSidebar->queueSearchDocuments(); },
    });
    m_panelSearch->registerTarget({
        MainPanelId::Artists,
        QStringLiteral("Artists"),
        m_artistSidebar->navigationWidget(),
        [this]() { return m_artistSidebar->rowCount(); },
        [this]() { return m_artistSidebar->currentRow(); },
        [this](int row) { m_artistSidebar->setCurrentRow(row); },
        [this](int row, int direction) { m_artistSidebar->setCurrentRow(row, direction); },
        [this]() { m_artistSidebar->activateCurrentArtist(); },
        {},
        {},
        {},
        {},
        {},
        {},
        {},
        {},
        {},
        {},
        [this]() { return m_artistSidebar->searchDocuments(); },
    });
    m_panelSearch->registerTarget({
        MainPanelId::Albums,
        QStringLiteral("Albums"),
        m_albumGrid,
        [this]() { return m_albumGrid->rowCount(); },
        [this]() { return m_albumGrid->currentRow(); },
        [this](int row) { m_albumGrid->setCurrentRow(row); },
        {},
        [this]() { narrowAlbumFilters(m_albumGrid->albumTitlesForAction()); },
        [this]() { playAlbumsNow(m_albumGrid->albumTitlesForAction()); },
        [this]() { m_albumGrid->addCurrentAlbumToQueue(); },
        [this]() { m_albumGrid->playNextCurrentAlbum(); },
        [this]() { m_albumGrid->markCurrentAlbum(); },
        [this]() { m_albumGrid->markAllAlbums(); },
        [this]() { m_albumGrid->unmarkCurrentAlbum(); },
        [this]() { m_albumGrid->unmarkAllAlbums(); },
        [this]() {
            m_albumGrid->setRememberedOutlineVisible(false);
            m_artistSidebar->activateCurrentArtist();
        },
        [this](int horizontal, int vertical) { m_albumGrid->moveCurrentByGrid(horizontal, vertical); },
        [this]() { clearAlbumFilter(); },
        [this]() { return m_albumGrid->searchDocuments(); },
    });
    m_panelSearch->registerTarget({
        MainPanelId::Tracks,
        QStringLiteral("Tracks"),
        m_trackTable,
        [this]() { return m_trackTable->rowCount(); },
        [this]() { return m_trackTable->currentRow(); },
        [this](int row) { m_trackTable->setCurrentRow(row); },
        [this](int row, int direction) { m_trackTable->setCurrentRow(row, direction); },
        [this]() { m_trackTable->activateCurrentTrack(); },
        [this]() { m_trackTable->activateCurrentTrack(); },
        [this]() { m_trackTable->addCurrentTrackToQueue(); },
        [this]() { m_trackTable->playNextCurrentTrack(); },
        [this]() { m_trackTable->markCurrentTrack(); },
        [this]() { m_trackTable->markAllTracks(); },
        [this]() { m_trackTable->unmarkCurrentTrack(); },
        [this]() { m_trackTable->unmarkAllTracks(); },
        [this]() { m_artistSidebar->activateCurrentArtist(); },
        {},
        [this]() { clearAlbumFilter(); },
        [this]() { return m_trackTable->searchDocuments(); },
    });
    connect(m_panelSearch, &PanelSearchController::statusMessage, this, [this](const QString &message, int timeoutMs) {
        statusBar()->showMessage(message, timeoutMs);
    });
    connect(m_panelSearch, &PanelSearchController::activePanelChanged, this, [this](MainPanelId) {
        saveMainWindowViewSettings();
    });
    connect(m_rootSplitter, &QSplitter::splitterMoved, this, &MainWindow::saveMainWindowViewSettings);
    connect(m_centerSplitter, &QSplitter::splitterMoved, this, &MainWindow::saveMainWindowViewSettings);
    connect(m_rightSidebar, &RightSidebar::queueTrackActivated, this, [this](int index) { playQueueIndex(index); });
    connect(m_rightSidebar, &RightSidebar::queueTrackRatingChanged, this, &MainWindow::applyTrackRating);
    connect(m_rightSidebar, &RightSidebar::queueRowsMoveRequested, this, &MainWindow::moveQueueRows);
    connect(m_rightSidebar, &RightSidebar::queueRowsRemoveRequested, this, &MainWindow::removeQueueRows);
    connect(m_rightSidebar, &RightSidebar::queueClearRequested, this, &MainWindow::clearQueue);
    connect(m_rightSidebar, &RightSidebar::clearPlayNextPriorityRequested, this, &MainWindow::clearPlayNextPriority);
    connect(m_queueScreen, &QueueScreen::queueTrackActivated, this, [this](int index) { playQueueIndex(index); });
    connect(m_queueScreen, &QueueScreen::queueTrackRatingChanged, this, &MainWindow::applyTrackRating);
    connect(m_queueScreen, &QueueScreen::queueRowsMoveRequested, this, &MainWindow::moveQueueRows);
    connect(m_queueScreen, &QueueScreen::queueRowsRemoveRequested, this, &MainWindow::removeQueueRows);
    connect(m_queueScreen, &QueueScreen::queueClearRequested, this, &MainWindow::clearQueue);
    connect(m_queueScreen, &QueueScreen::clearPlayNextPriorityRequested, this, &MainWindow::clearPlayNextPriority);
    connect(m_queueScreen, &QueueScreen::findFileRequested, this, &MainWindow::findTrackFile);
    connect(m_queueScreen, &QueueScreen::trackLibraryRequested, this, &MainWindow::revealTrackInLibrary);
    connect(m_queueScreen, &QueueScreen::viewSettingsChanged, this, &MainWindow::saveQueueScreenViewSettings);
    connect(m_mpris, &MprisService::raiseRequested, this, [this]() {
        show();
        raise();
        activateWindow();
    });
    connect(m_mpris, &MprisService::previousRequested, this, &MainWindow::playPreviousTrack);
    connect(m_mpris, &MprisService::nextRequested, this, &MainWindow::playNextTrack);
    connect(m_mpris, &MprisService::pauseRequested, m_playback, &PlaybackBackend::pause);
    connect(m_mpris, &MprisService::playPauseRequested, this, &MainWindow::togglePlayback);
    connect(m_mpris, &MprisService::stopRequested, m_playback, &PlaybackBackend::stop);
    connect(m_mpris, &MprisService::playRequested, this, &MainWindow::playFromMpris);
    connect(m_mpris, &MprisService::seekRequested, m_playback, &PlaybackBackend::seek);
    connect(m_mpris, &MprisService::relativeSeekRequested, this, &MainWindow::seekRelativeFromMpris);
    connect(m_mpris, &MprisService::volumeRequested, this, &MainWindow::setVolumeFromMpris);
    connect(m_playerBar, &PlayerBar::openLibraryRequested, this, &MainWindow::openLibraryFolder);
    connect(m_playerBar, &PlayerBar::sourceDirectoriesRequested, this, &MainWindow::configureSourceDirectories);
    connect(m_playerBar, &PlayerBar::scanEnabledSourcesRequested, this, &MainWindow::scanEnabledSourceDirectories);
    connect(m_playerBar, &PlayerBar::forceRescanRequested, this, &MainWindow::forceRescanEnabledSourceDirectories);
    connect(m_playerBar, &PlayerBar::removeMissingTracksRequested, this, &MainWindow::removeMissingTracks);
    connect(m_playerBar, &PlayerBar::listUnsupportedFilesChanged, this, [this](bool show) {
        m_freeRoamFileExplorer->setShowUnsupportedFiles(show);
        m_libraryFileExplorer->setShowUnsupportedFiles(show);
        m_state->setSetting(QStringLiteral("fileExplorer.showUnsupported"), show ? QStringLiteral("true") : QStringLiteral("false"));
    });
    connect(m_playerBar, &PlayerBar::syncCurrentTrackRatingTagsRequested, this, &MainWindow::syncCurrentTrackRatingTags);
    connect(m_playerBar, &PlayerBar::syncCurrentArtistRatingTagsRequested, this, &MainWindow::syncCurrentArtistRatingTags);
    connect(m_playerBar, &PlayerBar::syncAllSavedRatingTagsRequested, this, &MainWindow::syncAllSavedRatingTags);
    connect(m_playerBar, &PlayerBar::retryPendingRatingTagsRequested, this, &MainWindow::retryPendingRatingTags);
    connect(m_playerBar, &PlayerBar::currentTrackLibraryRequested, this, &MainWindow::jumpToPlayingSong);
    connect(m_playerBar, &PlayerBar::playbackProfileRequested, this, &MainWindow::configurePlaybackProfile);
    connect(m_playerBar, &PlayerBar::playbackResumeRequested, this, &MainWindow::configurePlaybackResume);
    connect(m_playerBar, &PlayerBar::linkRootsRequested, this, &MainWindow::configureLinkRoots);
    connect(m_playerBar, &PlayerBar::mpdSourceRequested, this, &MainWindow::configureMpdSource);
    connect(m_playerBar, &PlayerBar::mpdImportRequested, this, &MainWindow::importMpdLibraryMetadata);
    connect(m_playerBar, &PlayerBar::compactMenuChanged, this, &MainWindow::applyCompactMenu);
    connect(m_playerBar, &PlayerBar::trackInfoPaneVisibleChanged, this, &MainWindow::applyTrackInfoPaneVisible);
    connect(m_playerBar, &PlayerBar::trackInfoPaneSettingsRequested, this, &MainWindow::configureTrackInfoPanel);
    connect(m_playerBar, &PlayerBar::albumArtResolutionRequested, this, &MainWindow::configureAlbumArtResolution);
    connect(m_playerBar, &PlayerBar::searchRankingRequested, this, &MainWindow::configureSearchRanking);
    connect(m_playerBar, &PlayerBar::keybindingsRequested, this, &MainWindow::configureKeybindings);
    connect(m_playerBar, &PlayerBar::resetViewPreferencesRequested, this, &MainWindow::resetViewPreferences);
    connect(m_playerBar, &PlayerBar::panelOrderRequested, this, &MainWindow::openPanelOrderDialog);
    connect(m_playerBar, &PlayerBar::listenBrainzEnabledChanged, this, &MainWindow::setListenBrainzEnabled);
    connect(m_playerBar, &PlayerBar::listenBrainzTokenRequested, this, &MainWindow::setListenBrainzToken);
    connect(m_playerBar, &PlayerBar::lastFmEnabledChanged, this, &MainWindow::setLastFmEnabled);
    connect(m_playerBar, &PlayerBar::lastFmSettingsRequested, this, &MainWindow::showLastFmSettings);
    connect(m_playerBar, &PlayerBar::previousRequested, this, &MainWindow::playPreviousTrack);
    connect(m_playerBar, &PlayerBar::playPauseRequested, this, &MainWindow::togglePlayback);
    connect(m_playerBar, &PlayerBar::nextRequested, this, &MainWindow::playNextTrack);
    connect(m_playerBar, &PlayerBar::stopRequested, m_playback, &PlaybackBackend::stop);
    connect(m_playerBar, &PlayerBar::seekRequested, m_playback, &PlaybackBackend::seek);
    connect(m_playerBar, &PlayerBar::volumeChanged, this, [this](int volume) {
        const int clamped = std::clamp(volume, 0, 100);
        m_volume = static_cast<double>(clamped) / 100.0;
        m_playback->setVolume(m_volume);
        m_mpris->setVolume(m_volume);
        m_state->setSetting(QStringLiteral("volume"), QString::number(clamped));
    });
    connect(m_playerBar, &PlayerBar::currentTrackRatingChanged, this, [this](int rating) {
        if (!m_currentTrack.path.isEmpty()) {
            applyTrackRating(m_currentTrack, rating);
        }
    });
    connect(m_trackTable, &TrackTable::findFileRequested, this, &MainWindow::findTrackFile);
    connect(m_rightSidebar, &RightSidebar::findFileRequested, this, &MainWindow::findTrackFile);
    connect(m_rightSidebar, &RightSidebar::trackLibraryRequested, this, &MainWindow::revealTrackInLibrary);
    connect(m_rightSidebar, &RightSidebar::artistRequested, this, &MainWindow::jumpToTrackInfoArtist);
    connect(m_rightSidebar, &RightSidebar::albumRequested, this, &MainWindow::jumpToTrackInfoAlbum);
    connect(m_libraryFileExplorer, &FileExplorerView::directoryRequested, this, &MainWindow::setLibraryExplorerDirectory);
    connect(m_libraryFileExplorer, &FileExplorerView::trackActivated, this, &MainWindow::appendAndPlayTrack);
    connect(m_libraryFileExplorer, &FileExplorerView::playNextRequested, this, &MainWindow::playNextTracks);
    connect(m_libraryFileExplorer, &FileExplorerView::addToQueueRequested, this, &MainWindow::addTracksToQueue);
    connect(m_libraryFileExplorer, &FileExplorerView::importDirectoryRequested, this, [this](const QString &path) {
        startScan(path);
    });
    connect(m_libraryFileExplorer, &FileExplorerView::findFileRequested, this, &MainWindow::findTrackFile);
    connect(m_freeRoamFileExplorer, &FileExplorerView::directoryRequested, this, &MainWindow::setFreeRoamDirectory);
    connect(m_freeRoamFileExplorer, &FileExplorerView::trackActivated, this, &MainWindow::appendAndPlayTrack);
    connect(m_freeRoamFileExplorer, &FileExplorerView::playNextRequested, this, &MainWindow::playNextTracks);
    connect(m_freeRoamFileExplorer, &FileExplorerView::addToQueueRequested, this, &MainWindow::addTracksToQueue);
    connect(m_freeRoamFileExplorer, &FileExplorerView::importDirectoryRequested, this, [this](const QString &path) {
        startScan(path);
    });
    connect(m_freeRoamFileExplorer, &FileExplorerView::findFileRequested, this, &MainWindow::findTrackFile);
    connect(m_libraryFileExplorer, &FileExplorerView::trackRatingChangeRequested, this, &MainWindow::applyTrackRating);
    connect(m_freeRoamFileExplorer, &FileExplorerView::trackRatingChangeRequested, this, &MainWindow::applyTrackRating);

    // Search view
    connect(m_searchView, &SearchView::addToQueueRequested, this, &MainWindow::addTracksToQueue);
    connect(m_searchView, &SearchView::playNextRequested, this, &MainWindow::playNextTracks);
    connect(m_searchView, &SearchView::playNowRequested, this, [this](const QVector<Track> &tracks) {
        if (tracks.isEmpty()) return;
        addTracksToQueue(tracks);
        playQueueIndex(static_cast<int>(m_queue.size()) - static_cast<int>(tracks.size()));
    });
    connect(m_searchView, &SearchView::findInLibraryRequested, this, &MainWindow::revealTrackInLibrary);
    connect(m_searchView, &SearchView::findFileRequested, this, &MainWindow::findTrackFile);
    const auto trackResolver = [this](const QString &path) { return m_database->trackForPath(path); };
    m_libraryFileExplorer->setTrackResolver(trackResolver);
    m_freeRoamFileExplorer->setTrackResolver(trackResolver);
    connect(m_freeRoamFileExplorer, &FileExplorerView::startDirectoryChanged, this, [this](const QString &path) {
        m_state->setSetting(QStringLiteral("fileExplorer.startDirectory"), path);
    });
    connect(m_libraryFileExplorer, &FileExplorerView::rowHeightChanged, this, [this](int height) {
        m_freeRoamFileExplorer->setRowHeight(height);
        m_state->setSetting(QStringLiteral("fileExplorer.rowHeight"), QString::number(height));
    });
    connect(m_freeRoamFileExplorer, &FileExplorerView::rowHeightChanged, this, [this](int height) {
        m_libraryFileExplorer->setRowHeight(height);
        m_state->setSetting(QStringLiteral("fileExplorer.rowHeight"), QString::number(height));
    });
    connect(m_libraryFileExplorer, &FileExplorerView::keyBindingProfileChanged, this, [this](const QString &name) {
        m_freeRoamFileExplorer->setKeyBindingProfileName(name);
        m_state->setSetting(QStringLiteral("fileExplorer.keyBindingProfile"), name);
    });
    connect(m_freeRoamFileExplorer, &FileExplorerView::keyBindingProfileChanged, this, [this](const QString &name) {
        m_libraryFileExplorer->setKeyBindingProfileName(name);
        m_state->setSetting(QStringLiteral("fileExplorer.keyBindingProfile"), name);
    });
    connect(m_libraryFileExplorer, &FileExplorerView::keyHintVisibilityChanged, this, [this](bool visible) {
        m_freeRoamFileExplorer->setKeyHintBarVisible(visible);
        m_state->setSetting(QStringLiteral("fileExplorer.showKeyHints"), visible ? QStringLiteral("true") : QStringLiteral("false"));
    });
    connect(m_freeRoamFileExplorer, &FileExplorerView::keyHintVisibilityChanged, this, [this](bool visible) {
        m_libraryFileExplorer->setKeyHintBarVisible(visible);
        m_state->setSetting(QStringLiteral("fileExplorer.showKeyHints"), visible ? QStringLiteral("true") : QStringLiteral("false"));
    });
    auto persistExplorerSort = [this](const QString &field, bool descending, bool reverseGroups) {
        m_state->setSetting(QStringLiteral("fileExplorer.sortField"), field);
        m_state->setSetting(QStringLiteral("fileExplorer.sortDescending"), descending ? QStringLiteral("true") : QStringLiteral("false"));
        m_state->setSetting(QStringLiteral("fileExplorer.sortReverseGroups"), reverseGroups ? QStringLiteral("true") : QStringLiteral("false"));
    };
    connect(m_libraryFileExplorer, &FileExplorerView::sortChanged, this, [this, persistExplorerSort](const QString &field, bool descending, bool reverseGroups) {
        m_freeRoamFileExplorer->setSort(MusicSort::sortFieldFromString(field, MusicSort::SortField::FileName), descending, reverseGroups);
        persistExplorerSort(field, descending, reverseGroups);
    });
    connect(m_freeRoamFileExplorer, &FileExplorerView::sortChanged, this, [this, persistExplorerSort](const QString &field, bool descending, bool reverseGroups) {
        m_libraryFileExplorer->setSort(MusicSort::sortFieldFromString(field, MusicSort::SortField::FileName), descending, reverseGroups);
        persistExplorerSort(field, descending, reverseGroups);
    });
    connect(m_playback, &PlaybackBackend::positionChanged, this, &MainWindow::updatePlaybackPosition);
    connect(m_playback, &PlaybackBackend::durationChanged, this, &MainWindow::updatePlaybackPosition);
    connect(m_playback, &PlaybackBackend::preparedTrackStarted, this, &MainWindow::advanceAfterPreparedTransition);
    connect(m_playback, &PlaybackBackend::stateChanged, this, [this](PlaybackBackend::State state) {
        const bool playing = state == PlaybackBackend::State::Playing;
        m_playerBar->setPlaying(playing);
        m_mpris->setPlaybackState(state);
        QMetaObject::invokeMethod(m_listenBrainzScrobbler, "playbackStateChanged", Qt::QueuedConnection, Q_ARG(bool, playing));
        QMetaObject::invokeMethod(m_lastFmScrobbler, "playbackStateChanged", Qt::QueuedConnection, Q_ARG(bool, playing));
        schedulePlaybackStateSave(state != PlaybackBackend::State::Playing);
    });
    connect(m_playback, &PlaybackBackend::finished, this, [this]() {
        if (m_queueIndex + 1 < m_queue.size()) {
            playQueueIndex(m_queueIndex + 1);
        } else {
            // End of queue: tear the pipeline down so hasSource() is false and
            // the output device is freed. A later Play will restart cleanly.
            m_playback->stop();
            updateMprisCapabilities();
        }
    });
    connect(m_playback, &PlaybackBackend::errorOccurred, this, [this](const QString &errorString) {
        if (!errorString.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("Playback error: %1").arg(errorString), 10000);
        }
    });

    auto *queueShortcut = new QShortcut(QKeySequence(QStringLiteral("1")), this);
    connect(queueShortcut, &QShortcut::activated, this, [this]() {
        if (qobject_cast<QLineEdit *>(QApplication::focusWidget()) != nullptr) {
            return;
        }
        if (m_mainView == MainView::Queue) {
            m_queueScreen->revealCurrentPlaying();
        } else {
            switchMainView(MainView::Queue);
        }
    });
    auto *libraryPanelsShortcut = new QShortcut(QKeySequence(QStringLiteral("2")), this);
    connect(libraryPanelsShortcut, &QShortcut::activated, this, [this]() {
        if (qobject_cast<QLineEdit *>(QApplication::focusWidget()) != nullptr) {
            return;
        }
        switchMainView(MainView::LibraryPanels);
    });
    auto *fileExplorerShortcut = new QShortcut(QKeySequence(QStringLiteral("3")), this);
    connect(fileExplorerShortcut, &QShortcut::activated, this, [this]() {
        if (qobject_cast<QLineEdit *>(QApplication::focusWidget()) != nullptr) {
            return;
        }
        toggleFileExplorerView();
    });
    auto *searchShortcut = new QShortcut(QKeySequence(QStringLiteral("4")), this);
    connect(searchShortcut, &QShortcut::activated, this, [this]() {
        // While a text field has focus (including the search box), let '4' type.
        if (qobject_cast<QLineEdit *>(QApplication::focusWidget()) != nullptr) {
            return;
        }
        if (m_mainView == MainView::Search) {
            m_searchView->forceRefresh();  // re-press 4 in browse mode forces a refresh
        } else {
            switchMainView(MainView::Search);
        }
    });
    auto *jumpToPlayingShortcut = new QShortcut(QKeySequence(QStringLiteral("o")), this);
    connect(jumpToPlayingShortcut, &QShortcut::activated, this, [this]() {
        if (qobject_cast<QLineEdit *>(QApplication::focusWidget()) != nullptr) {
            return;
        }
        if (m_mainView == MainView::Queue) {
            m_queueScreen->revealCurrentPlaying();
            return;
        }
        jumpToPlayingSong();
    });

    m_albumGrid->setArtworkCache(m_artworkCache.get());
    loadPlaybackProfile();
    loadPlaybackResumeSettings();
    loadViewSettings();
    loadSearchRankingConfig();
    loadQueueState();
    loadExplorerState();
    configureListenBrainz();
    configureLastFm();
    loadExistingLibrary();
    restoreSavedPlaybackState();
}

MainWindow::~MainWindow()
{
    if (m_scanPipeline != nullptr) {
        m_scanPipeline->cancel();
    }
    if (m_scanThread != nullptr) {
        m_scanThread->quit();
        m_scanThread->wait(3000);
    }
    if (m_listenBrainzThread != nullptr) {
        m_listenBrainzThread->quit();
        m_listenBrainzThread->wait(3000);
    }
    if (m_lastFmThread != nullptr) {
        m_lastFmThread->quit();
        m_lastFmThread->wait(3000);
    }
    if (m_mpdImportThread != nullptr) {
        // run() can be blocked on network I/O; cancel() (atomic) makes it and the
        // MpdClient waits return promptly. Then join unconditionally so the worker
        // never outlives the window and touches its DB connection during teardown.
        if (m_mpdImportWorker != nullptr) {
            m_mpdImportWorker->cancel();
        }
        m_mpdImportThread->quit();
        if (!m_mpdImportThread->wait(5000)) {
            m_mpdImportThread->wait();
        }
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
    savePlaybackState(true);
    saveQueueState();
    saveExplorerState();
    saveAllViewSettings();
    QMainWindow::closeEvent(event);
}

void MainWindow::startScan(const QString &rootPath)
{
    startScan(rootPath, 0);
}

void MainWindow::startScan(const QString &rootPath, int scanRootId)
{
    if (m_scanThread != nullptr) {
        statusBar()->showMessage(QStringLiteral("A scan is already running"), 5000);
        return;
    }

    qCInfo(uiLog) << "starting scan" << rootPath;
    m_activeScanRootId = scanRootId;
    m_activeScanRootPath = cleanDirectoryPath(rootPath);
    statusBar()->showMessage(QStringLiteral("Scanning %1").arg(m_activeScanRootPath));
    m_scanProgress->setVisible(true);
    m_stopScanButton->setEnabled(true);
    m_stopScanButton->setVisible(true);
    m_lastUiRefreshIndexedTracks = 0;
    m_database->beginScanSession();

    ScanPipeline::Options options;
    options.forceFullRescan = m_forceFullRescan;

    m_scanThread = new QThread(this);
    m_scanPipeline = new ScanPipeline(m_activeScanRootPath, scanRootId,
                                      m_database->trackFingerprints(m_activeScanRootPath), options);
    m_scanPipeline->moveToThread(m_scanThread);

    connect(m_scanThread, &QThread::started, m_scanPipeline, &ScanPipeline::run);
    connect(m_scanPipeline, &ScanPipeline::batchReady, this, &MainWindow::ingestScanBatch);
    connect(m_scanPipeline, &ScanPipeline::progress, this,
            [this](qint64 enumerated, qint64 toProcess, qint64 processed, const QString &phase) {
                if (phase == QStringLiteral("enumerating")) {
                    statusBar()->showMessage(QStringLiteral("Scanning: enumerating files..."));
                } else {
                    statusBar()->showMessage(QStringLiteral("Scanning: %1 of %2 read (%3 found)")
                                                 .arg(processed).arg(toProcess).arg(enumerated));
                }
            });
    connect(m_scanPipeline, &ScanPipeline::missingReady, this, &MainWindow::markScannedTracksMissing);
    connect(m_scanPipeline, &ScanPipeline::finished, this, &MainWindow::finishScan);
    connect(m_scanPipeline, &ScanPipeline::finished, m_scanThread, &QThread::quit);
    connect(m_scanThread, &QThread::finished, m_scanPipeline, &QObject::deleteLater);
    connect(m_scanThread, &QThread::finished, m_scanThread, &QObject::deleteLater);
    connect(m_scanThread, &QThread::finished, this, [this]() {
        m_scanThread = nullptr;
        m_scanPipeline = nullptr;
        if (!m_pendingScanRoots.isEmpty()) {
            startNextQueuedSourceScan();
        } else {
            m_forceFullRescan = false;
        }
    });

    m_scanThread->start();
}

void MainWindow::scanEnabledSourceDirectories()
{
    scanSourceRoots(m_database->enabledScanRoots());
}

void MainWindow::forceRescanEnabledSourceDirectories()
{
    if (m_scanThread != nullptr) {
        statusBar()->showMessage(QStringLiteral("A scan is already running"), 5000);
        return;
    }
    m_forceFullRescan = true;
    scanSourceRoots(m_database->enabledScanRoots());
}

void MainWindow::scanSourceRoots(const QVector<ScanRoot> &roots)
{
    if (m_scanThread != nullptr) {
        statusBar()->showMessage(QStringLiteral("A scan is already running"), 5000);
        return;
    }

    m_pendingScanRoots = deduplicatedScanRoots(roots);
    if (m_pendingScanRoots.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("No scan-enabled source directories"), 5000);
        return;
    }
    startNextQueuedSourceScan();
}

void MainWindow::startNextQueuedSourceScan()
{
    if (m_scanThread != nullptr || m_pendingScanRoots.isEmpty()) {
        return;
    }

    const ScanRoot root = m_pendingScanRoots.takeFirst();
    startScan(root.path, root.id);
}

void MainWindow::cancelScan()
{
    if (m_scanPipeline == nullptr) {
        return;
    }

    m_scanPipeline->cancel();
    m_pendingScanRoots.clear();
    m_forceFullRescan = false;
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
        refreshLibraryFileExplorer();
    }
}

void MainWindow::finishScan(qint64 enumerated, qint64 indexed, qint64 skipped, bool canceled)
{
    qCInfo(uiLog) << "scan finished" << enumerated << indexed << "skipped" << skipped << "canceled" << canceled;
    m_database->endScanSession();
    const bool sourceScan = m_activeScanRootId > 0;
    const QString finishedRootPath = m_activeScanRootPath;
    if (sourceScan) {
        m_database->setScanRootLastScanned(m_activeScanRootId, canceled ? QStringLiteral("Canceled") : QString());
    }
    m_activeScanRootId = 0;
    m_activeScanRootPath.clear();
    m_scanProgress->setVisible(false);
    m_stopScanButton->setVisible(false);
    m_stopScanButton->setEnabled(false);
    statusBar()->showMessage(
        canceled
            ? QStringLiteral("Scan canceled: %1 read, %2 unchanged").arg(indexed).arg(skipped)
            : QStringLiteral("Scan complete: %1 found, %2 read, %3 unchanged").arg(enumerated).arg(indexed).arg(skipped),
        10000);
    refreshArtists();
    refreshLibraryFileExplorer();
    // Rebuild the search index with fresh library data
    if (!canceled) {
        m_searchView->invalidateIndex(databasePath());
    }
    if (!canceled && !m_pendingScanRoots.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Source scan complete: %1").arg(finishedRootPath), 3000);
    } else if (sourceScan && !canceled) {
        statusBar()->showMessage(QStringLiteral("Source scans complete"), 10000);
    }
}

void MainWindow::markScannedTracksMissing(const QStringList &paths)
{
    if (paths.isEmpty()) {
        return;
    }
    if (!m_database->beginTransaction()) {
        return;
    }
    const int marked = m_database->markTracksMissing(paths);
    m_database->commitTransaction();
    if (marked > 0) {
        qCInfo(uiLog) << "marked" << marked << "tracks missing";
    }
}

void MainWindow::removeMissingTracks()
{
    const int count = m_database->missingTrackCount();
    if (count == 0) {
        statusBar()->showMessage(QStringLiteral("No missing tracks to remove"), 5000);
        return;
    }
    const auto choice = QMessageBox::question(
        this, QStringLiteral("Remove missing tracks"),
        QStringLiteral("Permanently remove %1 track(s) whose files are gone from the library?").arg(count));
    if (choice != QMessageBox::Yes) {
        return;
    }
    const int removed = m_database->removeMissingTracks();
    refreshArtists();
    refreshLibraryFileExplorer();
    statusBar()->showMessage(QStringLiteral("Removed %1 missing track(s)").arg(removed), 5000);
}


void MainWindow::onArtworkReady(const QString &token, const QImage &image, quint64 generation)
{
    if (token == QStringLiteral("current") && generation == m_currentArtGeneration && !image.isNull()) {
        m_rightSidebar->setAlbumArt(image);
        m_playerBar->setAlbumArt(image);
    }
}

void MainWindow::onArtworkMissing(const QString &token, quint64 generation)
{
    if (token == QStringLiteral("current") && generation == m_currentArtGeneration) {
        m_rightSidebar->setAlbumArt(QString());
        m_playerBar->setAlbumArt(QString());
    }
}

void MainWindow::loadExistingLibrary()
{
    const qint64 mpdSourceId = m_database->mpdSourceId();
    m_artistSidebar->setMpdAvailable(mpdSourceId > 0);
    m_artistSidebar->setLibrarySourceIndex(m_librarySource == LibrarySource::Mpd ? 1 : 0);
    if (m_librarySource == LibrarySource::Mpd && mpdSourceId <= 0) {
        m_librarySource = LibrarySource::Local;
        restoreCurrentSourceSelection();
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
        if (m_panelSearch != nullptr) {
            m_panelSearch->refreshPanel(MainPanelId::Artists);
        }
        if (!m_currentArtist.isEmpty() && m_artistSidebar->selectArtist(m_currentArtist)) {
            showArtist(m_currentArtist, true, false);
            return;
        }
        if (!m_currentArtist.isEmpty()) {
            m_currentArtist.clear();
            m_selectedAlbumTitles.clear();
            m_selectedAlbumTitle.clear();
            m_loadedPanelArtist.clear();
            m_loadedPanelAlbumFilter.clear();
            m_loadedPanelSource = m_librarySource;
        }
        if (!artists.isEmpty() && m_currentArtist.isEmpty()) {
            m_artistSidebar->selectArtist(artists.first().name);
            showArtist(artists.first().name, true, true);
            return;
        }
        if (artists.isEmpty()) {
            m_currentArtist.clear();
            m_selectedAlbumTitles.clear();
            m_selectedAlbumTitle.clear();
            m_loadedPanelArtist.clear();
            m_loadedPanelAlbumFilter.clear();
            m_loadedPanelSource = m_librarySource;
            rememberCurrentSourceSelection();
        }
        return;
    }

    const QVector<Artist> artists = m_database->albumArtists();
    m_artistSidebar->setArtists(artists);
    if (m_panelSearch != nullptr) {
        m_panelSearch->refreshPanel(MainPanelId::Artists);
    }

    if (!m_currentArtist.isEmpty() && m_artistSidebar->selectArtist(m_currentArtist)) {
        showArtist(m_currentArtist, true, false);
        return;
    }

    if (!m_currentArtist.isEmpty()) {
        m_currentArtist.clear();
        m_selectedAlbumTitles.clear();
        m_selectedAlbumTitle.clear();
        m_loadedPanelArtist.clear();
        m_loadedPanelAlbumFilter.clear();
        m_loadedPanelSource = m_librarySource;
    }

    if (!artists.isEmpty() && m_currentArtist.isEmpty()) {
        m_artistSidebar->selectArtist(artists.first().name);
        showArtist(artists.first().name, true, true);
        return;
    }

    if (artists.isEmpty()) {
        m_currentArtist.clear();
        m_selectedAlbumTitles.clear();
        m_selectedAlbumTitle.clear();
        m_loadedPanelArtist.clear();
        m_loadedPanelAlbumFilter.clear();
        m_loadedPanelSource = m_librarySource;
        rememberCurrentSourceSelection();
    }
}

void MainWindow::selectArtist(const QString &artistName)
{
    showArtist(artistName, false, true);
}

void MainWindow::showArtist(const QString &artistName, bool forceReload, bool clearAlbumSelectionOnArtistChange)
{
    if (artistName.isEmpty()) {
        return;
    }

    const bool artistChanged = m_currentArtist != artistName;
    const QStringList nextAlbumFilters = (artistChanged && clearAlbumSelectionOnArtistChange)
        ? QStringList()
        : m_selectedAlbumTitles;
    const QString nextAlbumFilter = albumFilterKey(nextAlbumFilters);
    const bool sourceChanged = m_loadedPanelSource != m_librarySource;
    const bool albumFilterChanged = m_loadedPanelAlbumFilter != nextAlbumFilter;
    const bool shouldReload = forceReload
        || artistChanged
        || m_loadedPanelArtist != artistName
        || sourceChanged
        || albumFilterChanged
        || m_loadedPanelArtist.isEmpty();

    if (!shouldReload) {
        return;
    }

    rememberTrackTableViewState();
    m_currentArtist = artistName;
    m_selectedAlbumTitles = nextAlbumFilters;
    m_selectedAlbumTitle = m_selectedAlbumTitles.size() == 1 ? m_selectedAlbumTitles.first() : QString();
    rememberCurrentSourceSelection();
    refreshAlbumGrid(forceReload || artistChanged);
    refreshTrackTable();
    restoreTrackTableViewState();
    m_loadedPanelArtist = m_currentArtist;
    m_loadedPanelAlbumFilter = albumFilterKey(m_selectedAlbumTitles);
    m_loadedPanelSource = m_librarySource;
    saveExplorerState();
}

void MainWindow::selectAlbumFilter(const QString &albumTitle)
{
    rememberTrackTableViewState();
    m_selectedAlbumTitles = (m_selectedAlbumTitles.size() == 1 && m_selectedAlbumTitles.first() == albumTitle)
        ? QStringList()
        : QStringList{albumTitle};
    m_selectedAlbumTitle = m_selectedAlbumTitles.size() == 1 ? m_selectedAlbumTitles.first() : QString();
    rememberCurrentSourceSelection();
    refreshAlbumGrid();
    refreshTrackTable();
    restoreTrackTableViewState();
    m_loadedPanelArtist = m_currentArtist;
    m_loadedPanelAlbumFilter = albumFilterKey(m_selectedAlbumTitles);
    m_loadedPanelSource = m_librarySource;
    saveExplorerState();
}

void MainWindow::narrowAlbumFilters(const QStringList &albumTitles)
{
    const QStringList nextAlbumTitles = normalizedAlbumTitles(albumTitles);
    if (nextAlbumTitles.isEmpty() || nextAlbumTitles == m_selectedAlbumTitles) {
        return;
    }
    rememberTrackTableViewState();
    m_selectedAlbumTitles = nextAlbumTitles;
    m_selectedAlbumTitle = m_selectedAlbumTitles.size() == 1 ? m_selectedAlbumTitles.first() : QString();
    rememberCurrentSourceSelection();
    refreshAlbumGrid();
    refreshTrackTable();
    restoreTrackTableViewState();
    m_loadedPanelArtist = m_currentArtist;
    m_loadedPanelAlbumFilter = albumFilterKey(m_selectedAlbumTitles);
    m_loadedPanelSource = m_librarySource;
    saveExplorerState();
}

void MainWindow::clearAlbumFilter()
{
    if (m_selectedAlbumTitles.isEmpty()) {
        return;
    }
    rememberTrackTableViewState();
    m_selectedAlbumTitles.clear();
    m_selectedAlbumTitle.clear();
    rememberCurrentSourceSelection();
    refreshAlbumGrid();
    refreshTrackTable();
    restoreTrackTableViewState();
    m_loadedPanelArtist = m_currentArtist;
    m_loadedPanelAlbumFilter = albumFilterKey(m_selectedAlbumTitles);
    m_loadedPanelSource = m_librarySource;
    saveExplorerState();
}

void MainWindow::refreshAlbumGrid(bool freshLoad)
{
    if (m_currentArtist.isEmpty()) {
        return;
    }
    if (m_librarySource == LibrarySource::Mpd) {
        m_albumGrid->setAlbums(m_database->mpdAlbumsForArtist(m_currentArtist, mpdMusicDirectory()), freshLoad);
    } else {
        m_albumGrid->setAlbums(m_database->albumsForArtist(m_currentArtist), freshLoad);
    }
    m_albumGrid->setSelectedAlbumTitle(m_selectedAlbumTitle);
    if (m_panelSearch != nullptr) {
        m_panelSearch->refreshPanel(MainPanelId::Albums);
    }
}

void MainWindow::refreshTrackTable()
{
    if (m_currentArtist.isEmpty()) {
        return;
    }
    // One query for the whole selection (empty = whole artist), ordered by the
    // normal album sort instead of selection order.
    if (m_librarySource == LibrarySource::Mpd) {
        m_trackTable->setTracks(m_database->mpdTracksForArtist(m_currentArtist, mpdMusicDirectory(), m_selectedAlbumTitles));
    } else {
        m_trackTable->setTracks(m_database->tracksForArtist(m_currentArtist, m_selectedAlbumTitles));
    }
    if (m_panelSearch != nullptr) {
        m_panelSearch->refreshPanel(MainPanelId::Tracks);
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
    // Patch the rated row in place instead of rebuilding the whole track table
    // (a full reload also dropped scroll/selection, hence the old remember/restore
    // dance). The album grid still refreshes because its star reflects the album's
    // average track rating, which this edit can shift. track.rating0To100 already
    // carries the scanned file rating (or unset), so it is the right fallback when
    // a user rating is cleared.
    const bool nowHasUserRating = rating0To100 >= 0;
    m_trackTable->updateTrackRating(track.path, nowHasUserRating ? rating0To100 : track.rating0To100, nowHasUserRating);
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
        m_mpris->setTrack(m_currentTrack);
    }
    m_queueStore->setSnapshot(m_queue, m_queueIndex, m_queueIndex + 1, m_playNextInsertIndex);
    refreshPlayNextRange();

    if (rating0To100 >= 0 && m_librarySource == LibrarySource::Local) {
        schedulePendingRatingTagSync();
    }
}

void MainWindow::startRatingTagSync(const QVector<Track> &tracks, int scope)
{
    if (tracks.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("No rating tags to sync"), 5000);
        return;
    }
    if (m_ratingTagSyncRunning) {
        m_ratingTagSyncPending = true;
        statusBar()->showMessage(QStringLiteral("Rating tag sync already running; queued latest pending writes"), 5000);
        return;
    }

    RatingTagSyncRequest request;
    request.scope = static_cast<RatingTagSyncRequest::Scope>(scope);
    request.tracks = tracks;
    request.linkRoots = m_database->linkRoots();

    auto *thread = new QThread(this);
    auto *worker = new RatingTagSyncWorker(databasePath(), request);
    m_ratingTagSyncRunning = true;
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, &RatingTagSyncWorker::run);
    connect(worker, &RatingTagSyncWorker::progress, this, [this](int checked, int total, const QString &) {
        statusBar()->showMessage(QStringLiteral("Rating tag sync: %1 / %2 checked").arg(checked).arg(total));
    });
    connect(worker, &RatingTagSyncWorker::finished, this, [this, thread, worker](const RatingTagSyncSummary &summary, const QString &error) {
        if (!error.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("Rating tag sync"), error);
        } else {
            statusBar()->showMessage(QStringLiteral("Rating tag sync complete: %1 written, %2 no writable path, %3 failed")
                                         .arg(summary.written)
                                         .arg(summary.noWritablePath)
                                         .arg(summary.failed),
                                     10000);
        }
        // Patch only the rows the worker actually wrote, in place — no full table
        // reload and no per-queued-track DB requery (the old N+1 main-thread freeze
        // the user felt "when the tag is written"). The DB is already reconciled by
        // the worker; the effective rating equals the just-written value.
        bool currentTrackChanged = false;
        for (const RatingTagSyncUpdate &update : summary.updates) {
            const int effective = update.effectiveRating0To100;
            const bool hasUserRating = effective >= 0;
            m_trackTable->updateTrackRating(update.path, effective, hasUserRating);
            for (Track &queuedTrack : m_queue) {
                if (queuedTrack.path != update.path) {
                    continue;
                }
                queuedTrack.rating0To100 = effective;
                queuedTrack.hasUserRating = hasUserRating;
                queuedTrack.effectiveRating0To100 = effective;
            }
            if (m_currentTrack.path == update.path) {
                m_currentTrack.rating0To100 = effective;
                m_currentTrack.hasUserRating = hasUserRating;
                m_currentTrack.effectiveRating0To100 = effective;
                currentTrackChanged = true;
            }
        }
        if (currentTrackChanged) {
            const QString title = m_currentTrack.title.isEmpty() ? m_currentTrack.filename : m_currentTrack.title;
            QString subtitle = QStringLiteral("%1 - %2").arg(m_currentTrack.artistName, m_currentTrack.albumTitle);
            if (!m_currentTrack.date.isEmpty()) {
                subtitle += QStringLiteral(" (%1)").arg(m_currentTrack.date.left(4));
            }
            m_playerBar->setTrackInfo(title, subtitle, m_currentTrack.effectiveRating0To100);
            m_rightSidebar->setTrackInfo(m_currentTrack);
            m_mpris->setTrack(m_currentTrack);
        }
        if (!summary.updates.isEmpty()) {
            m_queueStore->setSnapshot(m_queue, m_queueIndex, m_queueIndex + 1, m_playNextInsertIndex);
            refreshPlayNextRange();
            saveQueueState();
        }
        m_ratingTagSyncRunning = false;
        const bool runPendingAgain = m_ratingTagSyncPending;
        m_ratingTagSyncPending = false;
        worker->deleteLater();
        thread->quit();
        if (runPendingAgain) {
            QTimer::singleShot(0, this, [this]() {
                startRatingTagSync(m_database->tracksWithPendingRatingWrites(), static_cast<int>(RatingTagSyncRequest::Scope::PendingWrites));
            });
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MainWindow::schedulePendingRatingTagSync()
{
    m_ratingTagSyncPending = true;
    statusBar()->showMessage(QStringLiteral("Queued rating tag write"), 3000);
    QTimer::singleShot(0, this, [this]() {
        if (m_ratingTagSyncRunning || !m_ratingTagSyncPending) {
            return;
        }
        m_ratingTagSyncPending = false;
        startRatingTagSync(m_database->tracksWithPendingRatingWrites(), static_cast<int>(RatingTagSyncRequest::Scope::PendingWrites));
    });
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
    m_trackTable->applyViewSettingsJson(m_state->setting(QStringLiteral("trackTable.view")));
    const QString rightSidebarSettings = m_state->setting(QStringLiteral("rightSidebar.view"));
    m_rightSidebar->applyViewSettingsJson(rightSidebarSettings);
    m_queueScreen->applyViewSettingsJson(m_state->setting(QStringLiteral("queueScreen.view")));
    m_playerBar->setTrackInfoPaneVisible(QJsonDocument::fromJson(rightSidebarSettings.toUtf8()).object().value(QStringLiteral("showTrackInfo")).toBool(true));
    const QJsonObject playerBar = QJsonDocument::fromJson(m_state->setting(QStringLiteral("playerBar.view")).toUtf8()).object();
    m_playerBar->setCompactMenu(playerBar.value(QStringLiteral("compactMenu")).toBool(false));

    const int volume = std::clamp(m_state->setting(QStringLiteral("volume"), QStringLiteral("100")).toInt(), 0, 100);
    m_volume = static_cast<double>(volume) / 100.0;
    m_playback->setVolume(m_volume);
    m_mpris->setVolume(m_volume);
    m_playerBar->setVolume(volume);
    m_albumGrid->applyViewSettingsJson(m_state->setting(QStringLiteral("albumGrid.view")));
    m_artistSidebar->applyViewSettingsJson(m_state->setting(QStringLiteral("artistSidebar.view")));
    const QJsonObject mainWindow = QJsonDocument::fromJson(m_state->setting(QStringLiteral("mainWindow.view")).toUtf8()).object();
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
    m_mainView = mainViewFromName(mainWindow.value(QStringLiteral("mainView")).toString());
    m_libraryExplorerDirectory = mainWindow.value(QStringLiteral("libraryExplorerDirectory")).toString();
    m_freeRoamDirectory = mainWindow.value(QStringLiteral("freeRoamDirectory")).toString(QDir::homePath());
    m_freeRoamFileExplorer->setRootPath(m_freeRoamDirectory);
    refreshLibraryFileExplorer();

    const QString keyProfile = m_state->setting(QStringLiteral("fileExplorer.keyBindingProfile"));
    if (!keyProfile.isEmpty()) {
        m_libraryFileExplorer->setKeyBindingProfileName(keyProfile);
        m_freeRoamFileExplorer->setKeyBindingProfileName(keyProfile);
    }
    const QString showHints = m_state->setting(QStringLiteral("fileExplorer.showKeyHints"));
    const bool hintsVisible = showHints == QStringLiteral("true");
    m_libraryFileExplorer->setKeyHintBarVisible(hintsVisible);
    m_freeRoamFileExplorer->setKeyHintBarVisible(hintsVisible);

    const bool showUnsupported = m_state->setting(QStringLiteral("fileExplorer.showUnsupported")) == QStringLiteral("true");
    m_playerBar->setListUnsupportedFiles(showUnsupported);
    m_libraryFileExplorer->setShowUnsupportedFiles(showUnsupported);
    m_freeRoamFileExplorer->setShowUnsupportedFiles(showUnsupported);

    m_freeRoamFileExplorer->setStartDirectory(m_state->setting(QStringLiteral("fileExplorer.startDirectory")));
    m_queueScreen->setKeyBindingProfileName(m_state->setting(QStringLiteral("queueScreen.keyBindingProfile")));

    const int explorerRowHeight = m_state->setting(QStringLiteral("fileExplorer.rowHeight")).toInt();
    if (explorerRowHeight > 0) {
        m_libraryFileExplorer->setRowHeight(explorerRowHeight);
        m_freeRoamFileExplorer->setRowHeight(explorerRowHeight);
    }

    const MusicSort::SortField explorerSortField = MusicSort::sortFieldFromString(
        m_state->setting(QStringLiteral("fileExplorer.sortField")), MusicSort::SortField::FileName);
    const bool explorerSortDesc = m_state->setting(QStringLiteral("fileExplorer.sortDescending")) == QStringLiteral("true");
    const bool explorerSortReverseGroups = m_state->setting(QStringLiteral("fileExplorer.sortReverseGroups")) == QStringLiteral("true");
    m_libraryFileExplorer->setSort(explorerSortField, explorerSortDesc, explorerSortReverseGroups);
    m_freeRoamFileExplorer->setSort(explorerSortField, explorerSortDesc, explorerSortReverseGroups);

    if (m_panelSearch != nullptr) {
        m_panelSearch->setKeyBindingProfileName(m_state->setting(QStringLiteral("mainPanel.keyBindingProfile"),
                                                                 defaultMainPanelKeyBindingProfileName()));
        const QJsonArray focusOrder = QJsonDocument::fromJson(m_state->setting(QStringLiteral("mainPanel.focusOrder")).toUtf8()).array();
        m_panelSearch->setFocusOrder(mainPanelFocusOrderFromJson(focusOrder));
        m_panelSearch->setActivePanelFromString(m_state->setting(QStringLiteral("mainPanel.activePanel")));
    }
    const int mainPanelScrollPadding = std::clamp(m_state->setting(QStringLiteral("mainPanel.scrollPadding"),
                                                                   QString::number(TableNavigationScroll::kDefaultPaddingRows)).toInt(),
                                                  0, 20);
    m_rightSidebar->setNavigationScrollPadding(mainPanelScrollPadding);
    m_queueScreen->setNavigationScrollPadding(mainPanelScrollPadding);
    m_artistSidebar->setNavigationScrollPadding(mainPanelScrollPadding);
    m_trackTable->setNavigationScrollPadding(mainPanelScrollPadding);

    switchMainView(m_mainView);
    applySharedTableSettings();
}

void MainWindow::saveTrackTableViewSettings()
{
    m_state->setSetting(QStringLiteral("trackTable.view"), m_trackTable->viewSettingsJson());
    applySharedTableSettings();
}

void MainWindow::saveAlbumGridViewSettings()
{
    m_state->setSetting(QStringLiteral("albumGrid.view"), m_albumGrid->viewSettingsJson());
}

void MainWindow::saveArtistSidebarViewSettings()
{
    m_state->setSetting(QStringLiteral("artistSidebar.view"), m_artistSidebar->viewSettingsJson());
}

void MainWindow::saveRightSidebarViewSettings()
{
    m_state->setSetting(QStringLiteral("rightSidebar.view"), m_rightSidebar->viewSettingsJson());
    applySharedTableSettings();
}

void MainWindow::saveQueueScreenViewSettings()
{
    m_state->setSetting(QStringLiteral("queueScreen.view"), m_queueScreen->viewSettingsJson());
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
    root.insert(QStringLiteral("mainView"), mainViewName(m_mainView));
    root.insert(QStringLiteral("libraryExplorerDirectory"), m_libraryExplorerDirectory);
    root.insert(QStringLiteral("freeRoamDirectory"), m_freeRoamDirectory);
    if (m_panelSearch != nullptr) {
        root.insert(QStringLiteral("activePanel"), mainPanelIdToString(m_panelSearch->activePanel()));
    }
    m_state->setSetting(QStringLiteral("mainWindow.view"), QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    if (m_panelSearch != nullptr) {
        m_state->setSetting(QStringLiteral("mainPanel.keyBindingProfile"), m_panelSearch->keyBindingProfileName());
        m_state->setSetting(QStringLiteral("mainPanel.focusOrder"),
                            QString::fromUtf8(QJsonDocument(mainPanelFocusOrderToJson(m_panelSearch->focusOrder())).toJson(QJsonDocument::Compact)));
        m_state->setSetting(QStringLiteral("mainPanel.activePanel"), mainPanelIdToString(m_panelSearch->activePanel()));
    }
}

void MainWindow::saveAllViewSettings()
{
    saveTrackTableViewSettings();
    saveAlbumGridViewSettings();
    saveArtistSidebarViewSettings();
    saveRightSidebarViewSettings();
    saveQueueScreenViewSettings();
    saveMainWindowViewSettings();
}

void MainWindow::resetViewPreferences()
{
    const QStringList keys = {
        QStringLiteral("trackTable.view"),
        QStringLiteral("rightSidebar.view"),
        QStringLiteral("queueScreen.view"),
        QStringLiteral("albumGrid.view"),
        QStringLiteral("artistSidebar.view"),
        QStringLiteral("mainWindow.view"),
        QStringLiteral("tables.view"),
        QStringLiteral("playerBar.view"),
        QStringLiteral("fileExplorer.keyBindingProfile"),
        QStringLiteral("fileExplorer.showKeyHints"),
        QStringLiteral("fileExplorer.showUnsupported"),
        QStringLiteral("fileExplorer.startDirectory"),
        QStringLiteral("fileExplorer.rowHeight"),
        QStringLiteral("fileExplorer.sortField"),
        QStringLiteral("fileExplorer.sortDescending"),
        QStringLiteral("fileExplorer.sortReverseGroups"),
        QStringLiteral("queueScreen.keyBindingProfile"),
        QStringLiteral("mainPanel.keyBindingProfile"),
        QStringLiteral("mainPanel.focusOrder"),
        QStringLiteral("mainPanel.activePanel"),
        QStringLiteral("mainPanel.scrollPadding"),
    };
    for (const QString &key : keys) {
        m_state->removeSetting(key);
    }

    m_trackTable->resetViewSettings();
    m_albumGrid->resetViewSettings();
    m_artistSidebar->resetViewSettings();
    m_rightSidebar->resetViewSettings();
    m_queueScreen->resetViewSettings();

    m_rootSplitter->setSizes({260, 900, 300});
    m_centerSplitter->setSizes({500, 400});
    m_playerBar->setCompactMenu(false);
    m_playerBar->setTrackInfoPaneVisible(true);
    m_playerBar->setListUnsupportedFiles(false);

    const QString explorerProfile = defaultKeyBindingProfiles().isEmpty()
        ? QStringLiteral("vim")
        : defaultKeyBindingProfiles().first().name;
    m_libraryFileExplorer->setKeyBindingProfileName(explorerProfile);
    m_freeRoamFileExplorer->setKeyBindingProfileName(explorerProfile);
    m_libraryFileExplorer->setKeyHintBarVisible(false);
    m_freeRoamFileExplorer->setKeyHintBarVisible(false);
    m_libraryFileExplorer->setShowUnsupportedFiles(false);
    m_freeRoamFileExplorer->setShowUnsupportedFiles(false);
    m_freeRoamFileExplorer->setStartDirectory(QString());
    m_libraryFileExplorer->setRowHeight(18);
    m_freeRoamFileExplorer->setRowHeight(18);
    m_libraryFileExplorer->setSort(MusicSort::SortField::FileName, false, false);
    m_freeRoamFileExplorer->setSort(MusicSort::SortField::FileName, false, false);

    m_queueScreen->setKeyBindingProfileName(defaultQueueKeyBindingProfileName());
    if (m_panelSearch != nullptr) {
        m_panelSearch->setKeyBindingProfileName(defaultMainPanelKeyBindingProfileName());
        m_panelSearch->setFocusOrder(defaultMainPanelFocusOrder());
        m_panelSearch->setActivePanel(MainPanelId::Artists, false);
    }
    const int defaultScrollPadding = TableNavigationScroll::kDefaultPaddingRows;
    m_rightSidebar->setNavigationScrollPadding(defaultScrollPadding);
    m_queueScreen->setNavigationScrollPadding(defaultScrollPadding);
    m_artistSidebar->setNavigationScrollPadding(defaultScrollPadding);
    m_trackTable->setNavigationScrollPadding(defaultScrollPadding);

    saveAllViewSettings();
    statusBar()->showMessage(QStringLiteral("View preferences reset to defaults"), 4000);
}

void MainWindow::openPanelOrderDialog()
{
    if (m_panelSearch == nullptr) {
        return;
    }
    PanelOrderDialog dialog(m_panelSearch->focusOrder(), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    m_panelSearch->setFocusOrder(dialog.resultOrder());
    m_state->setSetting(QStringLiteral("mainPanel.focusOrder"),
                        QString::fromUtf8(QJsonDocument(mainPanelFocusOrderToJson(m_panelSearch->focusOrder())).toJson(QJsonDocument::Compact)));
}

void MainWindow::switchMainView(MainView view)
{
    m_mainView = view;
    m_playerBar->setExplorerOptionsVisible(view == MainView::LibraryFileExplorer || view == MainView::FreeRoamFileExplorer);
    m_playerBar->setQueueViewLayoutActive(view == MainView::Queue);
    if (view == MainView::LibraryPanels) {
        m_mainStack->setCurrentWidget(m_rootSplitter);
        if (m_panelSearch != nullptr) {
            m_panelSearch->activateForMainView();
        }
    } else if (view == MainView::LibraryFileExplorer) {
        if (m_panelSearch != nullptr) {
            m_panelSearch->deactivateForNonMainView();
        }
        refreshLibraryFileExplorer();
        m_mainStack->setCurrentWidget(m_libraryFileExplorer);
    } else if (view == MainView::Search) {
        if (m_panelSearch != nullptr) {
            m_panelSearch->deactivateForNonMainView();
        }
        m_mainStack->setCurrentWidget(m_searchView);
        m_searchView->ensureIndexLoaded(databasePath());
        m_searchView->focusSearchBox();
    } else if (view == MainView::Queue) {
        if (m_panelSearch != nullptr) {
            m_panelSearch->deactivateForNonMainView();
        }
        m_mainStack->setCurrentWidget(m_queueScreen);
        m_queueScreen->focusQueue();
    } else {
        if (m_panelSearch != nullptr) {
            m_panelSearch->deactivateForNonMainView();
        }
        m_freeRoamFileExplorer->setRootPath(m_freeRoamDirectory.isEmpty() ? QDir::homePath() : m_freeRoamDirectory);
        m_mainStack->setCurrentWidget(m_freeRoamFileExplorer);
    }
    saveMainWindowViewSettings();
}

void MainWindow::toggleFileExplorerView()
{
    if (m_mainView == MainView::LibraryFileExplorer) {
        switchMainView(MainView::FreeRoamFileExplorer);
    } else {
        switchMainView(MainView::LibraryFileExplorer);
    }
}

void MainWindow::jumpToPlayingSong()
{
    if (m_currentTrack.path.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Nothing is playing"), 3000);
        return;
    }
    revealTrackInLibrary(m_currentTrack);
}

void MainWindow::revealTrackInLibrary(const Track &track)
{
    if (track.path.isEmpty()) {
        return;
    }

    switch (m_mainView) {
    case MainView::LibraryPanels: {
        const QString artist = !track.albumArtistName.trimmed().isEmpty()
            ? track.albumArtistName
            : track.artistName;
        if (!artist.isEmpty()) {
            // Narrow to the album as part of the reveal. h from Tracks clears
            // that narrowing, so jumping back to the full artist remains cheap.
            m_selectedAlbumTitles = track.albumTitle.isEmpty() ? QStringList() : QStringList{track.albumTitle};
            m_selectedAlbumTitle = track.albumTitle;
            m_artistSidebar->selectArtist(artist);
            showArtist(artist, false, false);
        }
        m_trackTable->selectTrackByPath(track.path);
        if (m_panelSearch != nullptr) {
            m_panelSearch->setActivePanel(MainPanelId::Tracks, true);
        }
        break;
    }
    case MainView::LibraryFileExplorer:
        m_libraryFileExplorer->revealFile(track.path);
        break;
    case MainView::FreeRoamFileExplorer: {
        const QString resolved = resolvedReadPathForTrack(track);
        m_freeRoamFileExplorer->revealFile(resolved.isEmpty() ? track.path : resolved);
        break;
    }
    case MainView::Search:
        // Search view has no reveal concept; switch to library panels and reveal there
        switchMainView(MainView::LibraryPanels);
        revealTrackInLibrary(track);
        break;
    case MainView::Queue:
        switchMainView(MainView::LibraryPanels);
        revealTrackInLibrary(track);
        break;
    }
}

void MainWindow::setLibraryExplorerDirectory(const QString &path)
{
    m_libraryExplorerDirectory = cleanDirectoryPath(path);
    refreshLibraryFileExplorer();
    saveMainWindowViewSettings();
}

void MainWindow::setFreeRoamDirectory(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir()) {
        return;
    }
    m_freeRoamDirectory = cleanDirectoryPath(path);
    m_freeRoamFileExplorer->setRootPath(m_freeRoamDirectory);
    saveMainWindowViewSettings();
}

void MainWindow::refreshLibraryFileExplorer()
{
    const QStringList directories = m_database->localLibraryDirectories(m_libraryExplorerDirectory);
    const QVector<Track> tracks = m_libraryExplorerDirectory.isEmpty() ? QVector<Track>() : m_database->tracksForDirectory(m_libraryExplorerDirectory);
    if (!m_libraryExplorerDirectory.isEmpty() && directories.isEmpty() && tracks.isEmpty()) {
        m_libraryExplorerDirectory.clear();
        m_libraryFileExplorer->setRootPath(QString());
        m_libraryFileExplorer->setLibraryEntries(m_database->localLibraryDirectories(), {});
        return;
    }
    m_libraryFileExplorer->setRootPath(m_libraryExplorerDirectory);
    m_libraryFileExplorer->setLibraryEntries(directories, tracks);
}

void MainWindow::loadQueueState()
{
    const QJsonObject root = QJsonDocument::fromJson(m_state->setting(QStringLiteral("queue.state")).toUtf8()).object();
    const QJsonArray tracks = root.value(QStringLiteral("tracks")).toArray();
    m_queue.clear();
    m_queue.reserve(tracks.size());
    for (const QJsonValue &value : tracks) {
        Track track = trackFromJson(value.toObject());
        if (!track.path.isEmpty()) {
            const Track refreshed = m_database->trackForPath(track.path);
            if (!refreshed.path.isEmpty()) {
                track = refreshed;
            }
            m_queue.push_back(track);
        }
    }

    m_queueIndex = std::clamp(root.value(QStringLiteral("index")).toInt(-1), -1, static_cast<int>(m_queue.size()) - 1);
    m_playNextInsertIndex = std::clamp(root.value(QStringLiteral("playNextInsertIndex")).toInt(m_queueIndex + 1), 0, static_cast<int>(m_queue.size()));
    m_queueStore->setSnapshot(m_queue, m_queueIndex, m_queueIndex + 1, m_playNextInsertIndex);
    m_rightSidebar->setCurrentIndex(m_queueIndex, /*reveal=*/true);
    refreshPlayNextRange();
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
    m_state->setSetting(QStringLiteral("queue.state"), QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    updateMprisCapabilities();
}

void MainWindow::loadExplorerState()
{
    const QJsonObject root = QJsonDocument::fromJson(m_state->setting(QStringLiteral("libraryExplorer.state")).toUtf8()).object();
    m_librarySource = root.value(QStringLiteral("source")).toString() == QStringLiteral("mpd") ? LibrarySource::Mpd : LibrarySource::Local;
    m_localArtist = root.value(QStringLiteral("localArtist")).toString(root.value(QStringLiteral("artist")).toString());
    m_localAlbumTitle = root.value(QStringLiteral("localAlbum")).toString(root.value(QStringLiteral("album")).toString());
    m_mpdArtist = root.value(QStringLiteral("mpdArtist")).toString();
    m_mpdAlbumTitle = root.value(QStringLiteral("mpdAlbum")).toString();
    restoreCurrentSourceSelection();
}

void MainWindow::saveExplorerState()
{
    rememberCurrentSourceSelection();
    QJsonObject root;
    root.insert(QStringLiteral("source"), m_librarySource == LibrarySource::Mpd ? QStringLiteral("mpd") : QStringLiteral("local"));
    root.insert(QStringLiteral("localArtist"), m_localArtist);
    root.insert(QStringLiteral("localAlbum"), m_localAlbumTitle);
    root.insert(QStringLiteral("mpdArtist"), m_mpdArtist);
    root.insert(QStringLiteral("mpdAlbum"), m_mpdAlbumTitle);
    m_state->setSetting(QStringLiteral("libraryExplorer.state"), QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

void MainWindow::applySharedTableSettings()
{
    const QJsonObject sharedSettings = QJsonDocument::fromJson(m_state->setting(QStringLiteral("tables.view")).toUtf8()).object();
    const QJsonObject trackSettings = QJsonDocument::fromJson(m_state->setting(QStringLiteral("trackTable.view")).toUtf8()).object();
    const QJsonObject sidebarSettings = QJsonDocument::fromJson(m_state->setting(QStringLiteral("rightSidebar.view")).toUtf8()).object();
    const int headerHeight = sharedSettings.value(QStringLiteral("headerHeight")).toInt(trackSettings.value(QStringLiteral("headerHeight")).toInt(sidebarSettings.value(QStringLiteral("headerHeight")).toInt(20)));
    m_trackTable->setHeaderHeight(headerHeight);
    m_rightSidebar->setHeaderHeight(headerHeight);

    QJsonObject shared;
    shared.insert(QStringLiteral("headerHeight"), headerHeight);
    m_state->setSetting(QStringLiteral("tables.view"), QString::fromUtf8(QJsonDocument(shared).toJson(QJsonDocument::Compact)));
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
    m_state->setSetting(QStringLiteral("playerBar.view"), QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

void MainWindow::rememberCurrentSourceSelection()
{
    if (m_librarySource == LibrarySource::Mpd) {
        m_mpdArtist = m_currentArtist;
        m_mpdAlbumTitle = albumFilterKey(m_selectedAlbumTitles);
    } else {
        m_localArtist = m_currentArtist;
        m_localAlbumTitle = albumFilterKey(m_selectedAlbumTitles);
    }
}

void MainWindow::restoreCurrentSourceSelection()
{
    if (m_librarySource == LibrarySource::Mpd) {
        m_currentArtist = m_mpdArtist;
        m_selectedAlbumTitles = normalizedAlbumTitles(m_mpdAlbumTitle.split(QChar(kAlbumFilterSeparator)));
    } else {
        m_currentArtist = m_localArtist;
        m_selectedAlbumTitles = normalizedAlbumTitles(m_localAlbumTitle.split(QChar(kAlbumFilterSeparator)));
    }
    m_selectedAlbumTitle = m_selectedAlbumTitles.size() == 1 ? m_selectedAlbumTitles.first() : QString();
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

void MainWindow::loadPlaybackResumeSettings()
{
    const QJsonObject root = QJsonDocument::fromJson(m_database->setting(QStringLiteral("playback.resumeSettings")).toUtf8()).object();
    m_savePlaybackPositionEnabled = root.value(QStringLiteral("savePosition")).toBool(true);
    m_restorePlaybackStateEnabled = root.value(QStringLiteral("restorePlaybackState")).toBool(true);
}

void MainWindow::savePlaybackResumeSettings()
{
    QJsonObject root;
    root.insert(QStringLiteral("savePosition"), m_savePlaybackPositionEnabled);
    root.insert(QStringLiteral("restorePlaybackState"), m_restorePlaybackStateEnabled);
    m_database->setSetting(QStringLiteral("playback.resumeSettings"), QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

void MainWindow::schedulePlaybackStateSave(bool immediate)
{
    if (!m_savePlaybackPositionEnabled && !m_restorePlaybackStateEnabled) {
        return;
    }
    if (immediate) {
        savePlaybackState();
        return;
    }
    if (m_playbackStateSaveTimer != nullptr) {
        m_playbackStateSaveTimer->start();
    }
}

void MainWindow::savePlaybackState(bool force)
{
    if (!m_savePlaybackPositionEnabled && !m_restorePlaybackStateEnabled) {
        return;
    }

    const qint64 positionMs = m_savePlaybackPositionEnabled ? std::max<qint64>(0, m_playback->position()) : 0;
    const PlaybackBackend::State state = m_playback->state();
    const QString stateName = playbackStateName(state);
    const bool meaningfulPositionChange = std::llabs(positionMs - m_lastSavedPlaybackPositionMs) >= 5000;
    const bool stateChanged = stateName != m_lastSavedPlaybackState;
    const bool trackChanged = m_currentTrack.path != m_lastSavedPlaybackTrackPath;
    if (!force && !meaningfulPositionChange && !stateChanged && !trackChanged) {
        return;
    }

    QJsonObject root;
    root.insert(QStringLiteral("queueIndex"), m_queueIndex);
    root.insert(QStringLiteral("trackPath"), m_currentTrack.path);
    root.insert(QStringLiteral("positionMs"), QString::number(positionMs));
    root.insert(QStringLiteral("state"), stateName);
    m_state->setSetting(QStringLiteral("playback.state"), QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    m_lastSavedPlaybackPositionMs = positionMs;
    m_lastSavedPlaybackTrackPath = m_currentTrack.path;
    m_lastSavedPlaybackState = stateName;
}

void MainWindow::restoreSavedPlaybackState()
{
    if (!m_restorePlaybackStateEnabled) {
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(m_state->setting(QStringLiteral("playback.state")).toUtf8()).object();
    const int queueIndex = root.value(QStringLiteral("queueIndex")).toInt(-1);
    const QString trackPath = root.value(QStringLiteral("trackPath")).toString();
    const qint64 positionMs = root.value(QStringLiteral("positionMs")).toString().toLongLong();
    const QString state = root.value(QStringLiteral("state")).toString(QStringLiteral("stopped"));
    if (queueIndex < 0 || queueIndex >= m_queue.size() || trackPath.isEmpty() || m_queue.at(queueIndex).path != trackPath) {
        return;
    }
    if (state != QStringLiteral("playing") && state != QStringLiteral("paused")) {
        return;
    }

    // For a paused restore, load the source directly into PAUSED state so the
    // audio device is never opened and no audio blip occurs. For a playing
    // restore, start normally; the backend will be in Playing state by the
    // time the settle timer fires.
    // Either way, skip scrobbler notification here — the backend transiently
    // reports states while starting up. Notify only when the session is
    // genuinely playing (checked in the settle timer below).
    const bool restoringPaused = (state == QStringLiteral("paused"));
    playQueueIndex(queueIndex, /*notifyScrobbler=*/false, /*startPaused=*/restoringPaused);
    QTimer::singleShot(250, this, [this, positionMs, state]() {
        if (positionMs > 0) {
            m_playback->seek(positionMs);
            m_playerBar->setPosition(positionMs, std::max<qint64>(m_playback->duration(), m_currentTrack.durationMs));
            m_mpris->setPositionMs(positionMs);
        }
        // Resume the scrobble session at the restored position rather than
        // starting a fresh one (which would lose already-elapsed time and
        // re-scrobble tracks that crossed the threshold before the restart).
        // For a paused restore, set up the session now so that when the user
        // later hits play, playbackStateChanged() continues the session
        // (including sending a rate-limited "now playing") without waiting
        // for the next track.
        const bool playing = state != QStringLiteral("paused")
            && m_playback->state() == PlaybackBackend::State::Playing;
        resumeScrobblers(m_currentTrack, std::max<qint64>(0, positionMs), playing);
        savePlaybackState(true);
    });
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

void MainWindow::configurePlaybackResume()
{
    PlaybackResumeDialog dialog(this);
    dialog.setSavePositionEnabled(m_savePlaybackPositionEnabled);
    dialog.setRestorePlaybackStateEnabled(m_restorePlaybackStateEnabled);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    m_savePlaybackPositionEnabled = dialog.savePositionEnabled();
    m_restorePlaybackStateEnabled = dialog.restorePlaybackStateEnabled();
    savePlaybackResumeSettings();
    savePlaybackState(true);
    statusBar()->showMessage(QStringLiteral("Playback resume settings updated"), 3000);
}

void MainWindow::loadSearchRankingConfig()
{
    const QString json = m_database->setting(QStringLiteral("search.ranking"));
    m_searchView->setRankConfig(Search::RankConfig::fromJsonString(json));
}

void MainWindow::configureSearchRanking()
{
    const QString json = m_database->setting(QStringLiteral("search.ranking"));
    const Search::RankConfig current = Search::RankConfig::fromJsonString(json);

    RankingDialog dialog(this);
    dialog.setConfig(current);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const Search::RankConfig updated = dialog.config();
    m_database->setSetting(QStringLiteral("search.ranking"), updated.toJsonString());
    m_searchView->setRankConfig(updated);
    statusBar()->showMessage(QStringLiteral("Search ranking updated"), 3000);
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

void MainWindow::configureSourceDirectories()
{
    SourceDirectoriesDialog dialog(this);
    dialog.setScanRoots(m_database->scanRoots());
    connect(&dialog, &SourceDirectoriesDialog::scanRootsRequested, this, [this](const QVector<ScanRoot> &roots) {
        scanSourceRoots(roots);
    });
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QVector<ScanRoot> updated = dialog.scanRoots();
    const QVector<ScanRoot> existing = m_database->scanRoots();
    for (const ScanRoot &root : existing) {
        const bool stillPresent = std::any_of(updated.cbegin(), updated.cend(), [&root](const ScanRoot &candidate) {
            return candidate.id == root.id;
        });
        if (stillPresent) {
            continue;
        }
        if (!m_database->removeScanRoot(root.id)) {
            QMessageBox::warning(this, QStringLiteral("Source directories"), m_database->lastError());
            return;
        }
    }

    for (const ScanRoot &root : updated) {
        if (!m_database->saveScanRoot(root)) {
            QMessageBox::warning(this, QStringLiteral("Source directories"), m_database->lastError());
            return;
        }
    }

    rememberTrackTableViewState();
    refreshArtists();
    refreshLibraryFileExplorer();
    restoreTrackTableViewState();
    statusBar()->showMessage(QStringLiteral("Source directories updated"), 3000);
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
                             QStringLiteral("Open containing directory"),
                             QStringLiteral("%1\n\nCandidates:\n%2")
                                 .arg(resolution.failureReason, resolution.candidates.join(QLatin1Char('\n'))));
        return;
    }

    // Try the FreeDesktop ShowItems D-Bus API first; it pre-selects the file
    // in the running file manager (Nautilus, Dolphin, Nemo, Thunar 3+, etc.).
    // Fall back to opening the parent directory via QDesktopServices when no
    // file manager is registered on the session bus.
    const QString fileUrl = QUrl::fromLocalFile(resolution.preferredPath).toString();
    const auto session = QDBusConnection::sessionBus();
    const auto *iface = session.interface();
    if (iface && iface->isServiceRegistered(QStringLiteral("org.freedesktop.FileManager1"))) {
        auto msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.FileManager1"),
                                                  QStringLiteral("/org/freedesktop/FileManager1"),
                                                  QStringLiteral("org.freedesktop.FileManager1"),
                                                  QStringLiteral("ShowItems"));
        msg << QStringList{fileUrl} << QString{};
        session.asyncCall(msg);
    } else {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(resolution.preferredPath).absolutePath()));
    }
    statusBar()->showMessage(QStringLiteral("Resolved %1").arg(resolution.preferredPath), 5000);
}

void MainWindow::configureTrackInfoPanel()
{
    m_rightSidebar->configureTrackInfoPanel(this);
}

void MainWindow::configureAlbumArtResolution()
{
    const int current = std::clamp(m_state->setting(QStringLiteral("artwork.size"), QStringLiteral("1024")).toInt(), 128, 4096);
    bool ok = false;
    const int size = QInputDialog::getInt(this,
                                          QStringLiteral("Album art resolution"),
                                          QStringLiteral("Cached cover size (pixels, square).\nHigher is sharper but uses more cache space."),
                                          current, 128, 4096, 64, &ok);
    if (!ok || size == current) {
        return;
    }

    m_state->setSetting(QStringLiteral("artwork.size"), QString::number(size));
    if (m_artworkCache != nullptr) {
        m_artworkCache->setArtSize(size);
    }
    // Re-render visible art at the new resolution (new size -> new cache keys).
    updateCurrentAlbumArt();
    if (!m_currentArtist.isEmpty()) {
        refreshAlbumGrid(true);
    }
}

void MainWindow::configureKeybindings()
{
    KeybindingsDialog dialog(this);
    if (m_panelSearch != nullptr) {
        dialog.setMainPanelProfileName(m_panelSearch->keyBindingProfileName());
    }
    dialog.setFileExplorerProfileName(m_libraryFileExplorer->keyBindingProfileName());
    dialog.setQueueProfileName(m_queueScreen->keyBindingProfileName());
    dialog.setFileExplorerKeyHintsVisible(m_libraryFileExplorer->isKeyHintBarVisible());

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    if (m_panelSearch != nullptr) {
        m_panelSearch->setKeyBindingProfileName(dialog.mainPanelProfileName());
        saveMainWindowViewSettings();
    }
    m_libraryFileExplorer->setKeyBindingProfileName(dialog.fileExplorerProfileName());
    m_freeRoamFileExplorer->setKeyBindingProfileName(dialog.fileExplorerProfileName());
    m_queueScreen->setKeyBindingProfileName(dialog.queueProfileName());
    m_libraryFileExplorer->setKeyHintBarVisible(dialog.fileExplorerKeyHintsVisible());
    m_freeRoamFileExplorer->setKeyHintBarVisible(dialog.fileExplorerKeyHintsVisible());
    m_state->setSetting(QStringLiteral("fileExplorer.keyBindingProfile"), dialog.fileExplorerProfileName());
    m_state->setSetting(QStringLiteral("queueScreen.keyBindingProfile"), dialog.queueProfileName());
    m_state->setSetting(QStringLiteral("fileExplorer.showKeyHints"),
                        dialog.fileExplorerKeyHintsVisible() ? QStringLiteral("true") : QStringLiteral("false"));
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
            // Rebuild the search index to include the newly imported MPD tracks
            m_searchView->invalidateIndex(databasePath());
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
    rememberCurrentSourceSelection();
    m_librarySource = (index == 1) ? LibrarySource::Mpd : LibrarySource::Local;

    if (m_librarySource == LibrarySource::Mpd && m_database->mpdSourceId() <= 0) {
        m_artistSidebar->setMpdAvailable(false);
        m_currentArtist.clear();
        m_selectedAlbumTitles.clear();
        m_selectedAlbumTitle.clear();
        m_loadedPanelArtist.clear();
        m_loadedPanelAlbumFilter.clear();
        m_loadedPanelSource = m_librarySource;
        rememberCurrentSourceSelection();
        m_albumGrid->setAlbums({});
        m_trackTable->setTracks({});
        saveExplorerState();
        statusBar()->showMessage(QStringLiteral("No MPD source configured. Use the menu to configure and import."), 5000);
        return;
    }

    restoreCurrentSourceSelection();
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
                              Q_ARG(QString, QDir(AppPaths::stateDir()).filePath(QStringLiteral("listenbrainz-pending.json"))));
}

void MainWindow::setListenBrainzEnabled(bool enabled)
{
    m_database->setSetting(QStringLiteral("listenbrainz.enabled"), enabled ? QStringLiteral("true") : QStringLiteral("false"));
    configureListenBrainz();
    // If a track is already playing when the scrobbler is toggled on, catch up
    // so it doesn't miss the current track.  The queued configure() runs first
    // (Qt::QueuedConnection), so credentials are set before resumeTrack fires.
    if (enabled && !m_currentTrack.path.isEmpty() && m_playback->state() != PlaybackBackend::State::Stopped) {
        const qint64 elapsedMs = std::max<qint64>(0, m_playback->position());
        const bool playing = m_playback->state() == PlaybackBackend::State::Playing;
        QMetaObject::invokeMethod(m_listenBrainzScrobbler, "resumeTrack", Qt::QueuedConnection,
                                  Q_ARG(Track, m_currentTrack), Q_ARG(qint64, elapsedMs), Q_ARG(bool, playing));
    }
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

QString MainWindow::lastFmApiKey() const
{
    QString apiKey = m_database->setting(QStringLiteral("lastfm.apiKey")).trimmed();
    if (apiKey.isEmpty()) {
        apiKey = QString::fromLocal8Bit(qgetenv("LASTFM_API_KEY")).trimmed();
    }
    if (apiKey.isEmpty()) {
        apiKey = QString::fromStdString(LastFmCredentials::defaultApiKey()).trimmed();
    }
    return apiKey;
}

QString MainWindow::lastFmSharedSecret() const
{
    QString secret = m_database->setting(QStringLiteral("lastfm.sharedSecret")).trimmed();
    if (secret.isEmpty()) {
        secret = QString::fromLocal8Bit(qgetenv("LASTFM_SHARED_SECRET")).trimmed();
    }
    if (secret.isEmpty()) {
        secret = QString::fromStdString(LastFmCredentials::defaultSharedSecret()).trimmed();
    }
    return secret;
}

bool MainWindow::hasDefaultLastFmCredentials() const
{
    // Returns true when env vars or build-time defaults supply both key and secret
    // (without consulting the DB — those are user overrides, not "defaults").
    const auto fromEnvOrDefault = [](const char *envVar, const std::string &buildDefault) {
        QString v = QString::fromLocal8Bit(qgetenv(envVar)).trimmed();
        if (v.isEmpty()) {
            v = QString::fromStdString(buildDefault).trimmed();
        }
        return v;
    };
    return !fromEnvOrDefault("LASTFM_API_KEY", LastFmCredentials::defaultApiKey()).isEmpty()
        && !fromEnvOrDefault("LASTFM_SHARED_SECRET", LastFmCredentials::defaultSharedSecret()).isEmpty();
}

void MainWindow::configureLastFm()
{
    const bool enabled = m_database->setting(QStringLiteral("lastfm.enabled"), QStringLiteral("false")) == QStringLiteral("true");
    const QString sessionKey = m_database->setting(QStringLiteral("lastfm.sessionKey"));

    m_playerBar->setLastFmEnabled(enabled);
    QMetaObject::invokeMethod(m_lastFmScrobbler,
                              "configure",
                              Qt::QueuedConnection,
                              Q_ARG(bool, enabled),
                              Q_ARG(QString, lastFmApiKey()),
                              Q_ARG(QString, lastFmSharedSecret()),
                              Q_ARG(QString, sessionKey),
                              Q_ARG(QString, QDir(AppPaths::stateDir()).filePath(QStringLiteral("lastfm-pending.json"))));
}

void MainWindow::setLastFmEnabled(bool enabled)
{
    m_database->setSetting(QStringLiteral("lastfm.enabled"), enabled ? QStringLiteral("true") : QStringLiteral("false"));
    configureLastFm();
    // If a track is already playing when the scrobbler is toggled on, catch up
    // so it doesn't miss the current track.  The queued configure() runs first
    // (Qt::QueuedConnection), so credentials are set before resumeTrack fires.
    if (enabled && !m_currentTrack.path.isEmpty() && m_playback->state() != PlaybackBackend::State::Stopped) {
        const qint64 elapsedMs = std::max<qint64>(0, m_playback->position());
        const bool playing = m_playback->state() == PlaybackBackend::State::Playing;
        QMetaObject::invokeMethod(m_lastFmScrobbler, "resumeTrack", Qt::QueuedConnection,
                                  Q_ARG(Track, m_currentTrack), Q_ARG(qint64, elapsedMs), Q_ARG(bool, playing));
    }
    statusBar()->showMessage(enabled ? QStringLiteral("Last.fm scrobbling enabled") : QStringLiteral("Last.fm scrobbling disabled"), 3000);
}

void MainWindow::showLastFmSettings()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Last.fm settings"));

    auto *layout = new QVBoxLayout(&dialog);

    // --- Status label ---
    auto *status = new QLabel(&dialog);
    layout->addWidget(status);

    // --- Single login/logout button ---
    // State machine: Disconnected → Login flow, Connected → Confirm logout.
    // A small shared struct keeps the confirm-timer and flag alive across lambdas.
    struct ButtonState {
        QTimer *confirmTimer = nullptr;
        bool confirming = false;
    };
    auto state = std::make_shared<ButtonState>();

    auto *actionButton = new QPushButton(&dialog);
    auto *buttonRow = new QHBoxLayout;
    buttonRow->addWidget(actionButton);
    buttonRow->addStretch();
    layout->addLayout(buttonRow);

    state->confirmTimer = new QTimer(&dialog);
    state->confirmTimer->setSingleShot(true);
    state->confirmTimer->setInterval(3000);

    // --- Credentials drawer ---
    // Collapsed by default when build/env defaults supply both key and secret;
    // expanded otherwise so users who must supply their own see the fields.
    const bool hasDefaults = hasDefaultLastFmCredentials();
    const QString keyPlaceholder = hasDefaults
        ? QStringLiteral("Using built-in default (leave blank)")
        : QStringLiteral("Required — your Last.fm API key");
    const QString secretPlaceholder = hasDefaults
        ? QStringLiteral("Using built-in default (leave blank)")
        : QStringLiteral("Required — your Last.fm shared secret");

    auto *drawerToggle = new QToolButton(&dialog);
    drawerToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    drawerToggle->setText(QStringLiteral("API credentials"));
    drawerToggle->setCheckable(true);
    drawerToggle->setChecked(!hasDefaults);
    drawerToggle->setArrowType(drawerToggle->isChecked() ? Qt::DownArrow : Qt::RightArrow);
    layout->addWidget(drawerToggle);

    auto *credWidget = new QWidget(&dialog);
    auto *form = new QFormLayout(credWidget);
    form->setContentsMargins(16, 0, 0, 0);
    auto *apiKey = new QLineEdit(m_database->setting(QStringLiteral("lastfm.apiKey")), credWidget);
    auto *sharedSecret = new QLineEdit(m_database->setting(QStringLiteral("lastfm.sharedSecret")), credWidget);
    apiKey->setPlaceholderText(keyPlaceholder);
    sharedSecret->setPlaceholderText(secretPlaceholder);
    sharedSecret->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("API key"), apiKey);
    form->addRow(QStringLiteral("Shared secret"), sharedSecret);
    credWidget->setVisible(!hasDefaults);
    layout->addWidget(credWidget);

    connect(drawerToggle, &QToolButton::toggled, credWidget, [drawerToggle, credWidget](bool open) {
        credWidget->setVisible(open);
        drawerToggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
        if (auto *w = drawerToggle->window()) {
            w->adjustSize();
        }
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    // --- refreshStatus: updates label and button label based on current DB state ---
    const auto refreshStatus = [this, status, actionButton, state]() {
        const QString username = m_database->setting(QStringLiteral("lastfm.username"));
        const bool connected = !m_database->setting(QStringLiteral("lastfm.sessionKey")).isEmpty();
        state->confirming = false;
        state->confirmTimer->stop();
        if (connected) {
            const QString text = username.isEmpty()
                ? QStringLiteral("Connected to Last.fm")
                : QStringLiteral("Connected as %1").arg(username);
            status->setText(text);
            status->setStyleSheet(QStringLiteral("font-weight: 600; color: #2e9e3f;"));
            actionButton->setText(QStringLiteral("Log out"));
            actionButton->setEnabled(true);
        } else {
            status->setText(QStringLiteral("Not connected"));
            status->setStyleSheet(QString());
            actionButton->setText(QStringLiteral("Log in to Last.fm"));
            actionButton->setEnabled(true);
        }
    };
    refreshStatus();

    // --- Persist credentials from the drawer fields ---
    const auto persistCredentials = [this, apiKey, sharedSecret]() {
        m_database->setSetting(QStringLiteral("lastfm.apiKey"), apiKey->text().trimmed());
        m_database->setSetting(QStringLiteral("lastfm.sharedSecret"), sharedSecret->text().trimmed());
        configureLastFm();
    };

    // --- Logout helper (shared by button click and confirm-timer path) ---
    const auto doLogout = [this, refreshStatus]() {
        QMetaObject::invokeMethod(m_lastFmScrobbler, "cancelAuthentication", Qt::QueuedConnection);
        m_database->removeSetting(QStringLiteral("lastfm.sessionKey"));
        m_database->removeSetting(QStringLiteral("lastfm.username"));
        m_database->setSetting(QStringLiteral("lastfm.enabled"), QStringLiteral("false"));
        configureLastFm();
        refreshStatus();
    };

    // Confirm-timeout: revert "Click again to log out" back to "Log out"
    connect(state->confirmTimer, &QTimer::timeout, &dialog, [actionButton, state]() {
        state->confirming = false;
        actionButton->setText(QStringLiteral("Log out"));
    });

    // --- Single button handler ---
    connect(actionButton, &QPushButton::clicked, &dialog,
            [this, &dialog, persistCredentials, doLogout, actionButton, status, state]() {
        const bool connected = !m_database->setting(QStringLiteral("lastfm.sessionKey")).isEmpty();

        if (!connected) {
            // Login path
            persistCredentials();
            const QString key = lastFmApiKey();
            const QString secret = lastFmSharedSecret();
            if (key.isEmpty() || secret.isEmpty()) {
                QMessageBox::warning(&dialog, QStringLiteral("Last.fm"),
                                     QStringLiteral("Enter a Last.fm API key and shared secret first."));
                return;
            }
            actionButton->setEnabled(false);
            status->setText(QStringLiteral("Waiting for browser authorization…"));
            status->setStyleSheet(QString());
            QMetaObject::invokeMethod(m_lastFmScrobbler,
                                      "startAuthentication",
                                      Qt::QueuedConnection,
                                      Q_ARG(QString, key),
                                      Q_ARG(QString, secret));
        } else if (!state->confirming) {
            // First logout click → enter confirm state
            state->confirming = true;
            actionButton->setText(QStringLiteral("Click again to log out"));
            state->confirmTimer->start();
        } else {
            // Second click within window → commit logout
            state->confirmTimer->stop();
            doLogout();
        }
    });

    // Auth runs on a worker thread; exec() keeps the event loop running, so
    // these queued signals update the open dialog live. Scoped to the dialog
    // so the connections drop when it closes.
    connect(m_lastFmScrobbler, &LastFmScrobbler::authenticationSucceeded, &dialog,
            [refreshStatus](const QString &, const QString &) {
                refreshStatus();
            });
    connect(m_lastFmScrobbler, &LastFmScrobbler::authenticationFailed, &dialog,
            [status, actionButton](const QString &message) {
                status->setText(QStringLiteral("Not connected — %1").arg(message));
                status->setStyleSheet(QString());
                actionButton->setEnabled(true);
                actionButton->setText(QStringLiteral("Log in to Last.fm"));
            });

    dialog.exec();
    persistCredentials();
    QMetaObject::invokeMethod(m_lastFmScrobbler, "cancelAuthentication", Qt::QueuedConnection);
}

void MainWindow::playTrack(const Track &track, bool notifyScrobbler, bool startPaused)
{
    if (track.path.isEmpty()) {
        return;
    }

    const QString playbackPath = resolvedReadPathForTrack(track);
    if (playbackPath.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("Playback"), QStringLiteral("Could not resolve a readable file for %1").arg(track.path));
        return;
    }

    presentTrack(track, notifyScrobbler);
    if (startPaused) {
        m_playback->loadPaused(QUrl::fromLocalFile(playbackPath));
    } else {
        m_playback->play(QUrl::fromLocalFile(playbackPath));
    }
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
    m_mpris->setTrack(track);
    m_mpris->setDurationMs(track.durationMs);
    m_mpris->setPositionMs(0);
    updateMprisCapabilities();
    if (notifyScrobbler) {
        notifyScrobblersTrackStarted(track);
        statusBar()->showMessage(QStringLiteral("Playing %1").arg(title), 3000);
    }
}

void MainWindow::notifyScrobblersTrackStarted(const Track &track)
{
    QMetaObject::invokeMethod(m_listenBrainzScrobbler, "trackStarted", Qt::QueuedConnection, Q_ARG(Track, track));
    QMetaObject::invokeMethod(m_lastFmScrobbler, "trackStarted", Qt::QueuedConnection, Q_ARG(Track, track));
}

void MainWindow::resumeScrobblers(const Track &track, qint64 elapsedMs, bool playing)
{
    QMetaObject::invokeMethod(m_listenBrainzScrobbler, "resumeTrack", Qt::QueuedConnection,
                              Q_ARG(Track, track), Q_ARG(qint64, elapsedMs), Q_ARG(bool, playing));
    QMetaObject::invokeMethod(m_lastFmScrobbler, "resumeTrack", Qt::QueuedConnection,
                              Q_ARG(Track, track), Q_ARG(qint64, elapsedMs), Q_ARG(bool, playing));
}

void MainWindow::appendAndPlayTrack(const Track &track)
{
    if (track.path.isEmpty()) {
        return;
    }

    for (int index = 0; index < m_queue.size(); ++index) {
        if (m_queue.at(index).path == track.path) {
            // Reset before jumping so playQueueIndex's guard always clears any
            // stale play-next boundary. Without this, a boundary that coincides
            // with m_queue.size() (left over from playing the last track) would
            // survive the guard check and spuriously badge all trailing entries.
            m_playNextInsertIndex = -1;
            playQueueIndex(index);
            return;
        }
    }

    m_queue.push_back(track);
    m_queueStore->setTracks(m_queue);
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
        if (m_queueIndex < 0 && start < m_queue.size()) {
            // Show the new rows, then start playback (playQueueIndex prepares next).
            m_queueStore->setTracks(m_queue);
            playQueueIndex(start);
        } else {
            syncQueueState();
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
    syncQueueState();
}

void MainWindow::addTracksToQueue(const QVector<Track> &tracks)
{
    for (const Track &track : tracks) {
        if (!track.path.isEmpty()) {
            m_queue.push_back(track);
        }
    }
    syncQueueState();
}

void MainWindow::moveQueueRows(const QVector<int> &rows, int destinationRow)
{
    if (rows.isEmpty() || m_queue.isEmpty()) {
        return;
    }

    QVector<int> sortedRows = rows;
    std::sort(sortedRows.begin(), sortedRows.end());
    sortedRows.erase(std::unique(sortedRows.begin(), sortedRows.end()), sortedRows.end());
    sortedRows.erase(std::remove_if(sortedRows.begin(), sortedRows.end(), [this](int row) {
                         return row < 0 || row >= m_queue.size();
                     }),
                     sortedRows.end());
    if (sortedRows.isEmpty()) {
        return;
    }

    // Allow dropping anywhere, including above the current track: newQueueIndex
    // below tracks where the playing track lands, so playback stays correct.
    destinationRow = std::clamp(destinationRow, 0, static_cast<int>(m_queue.size()));
    QVector<Track> moving;
    moving.reserve(sortedRows.size());
    int removedBeforeDestination = 0;
    for (int row : sortedRows) {
        moving.push_back(m_queue.at(row));
        if (row < destinationRow) {
            ++removedBeforeDestination;
        }
    }
    const int adjustedDestination = destinationRow - removedBeforeDestination;

    QVector<Track> remaining;
    remaining.reserve(m_queue.size() - moving.size());
    int removeCursor = 0;
    for (int row = 0; row < m_queue.size(); ++row) {
        if (removeCursor < sortedRows.size() && sortedRows.at(removeCursor) == row) {
            ++removeCursor;
            continue;
        }
        remaining.push_back(m_queue.at(row));
    }

    m_queue = remaining;
    for (int offset = 0; offset < moving.size(); ++offset) {
        m_queue.insert(adjustedDestination + offset, moving.at(offset));
    }

    int newQueueIndex = -1;
    if (m_queueIndex >= 0) {
        const auto movedCurrent = std::find(sortedRows.cbegin(), sortedRows.cend(), m_queueIndex);
        if (movedCurrent != sortedRows.cend()) {
            newQueueIndex = adjustedDestination + static_cast<int>(std::distance(sortedRows.cbegin(), movedCurrent));
        } else {
            const int removedBeforeCurrent = static_cast<int>(std::count_if(sortedRows.cbegin(), sortedRows.cend(), [this](int row) {
                return row < m_queueIndex;
            }));
            newQueueIndex = m_queueIndex - removedBeforeCurrent;
            if (adjustedDestination <= newQueueIndex) {
                newQueueIndex += static_cast<int>(moving.size());
            }
        }
    }
    m_queueIndex = newQueueIndex;
    // Manually reordering the upcoming tracks collapses the play-next priority
    // block (its ordinals no longer reflect a single "play next" batch).
    m_playNextInsertIndex = std::clamp(m_queueIndex + 1, 0, static_cast<int>(m_queue.size()));
    syncQueueState();
}

void MainWindow::removeQueueRows(const QVector<int> &rows)
{
    if (rows.isEmpty() || m_queue.isEmpty()) {
        return;
    }

    QVector<int> sortedRows = rows;
    std::sort(sortedRows.begin(), sortedRows.end());
    sortedRows.erase(std::unique(sortedRows.begin(), sortedRows.end()), sortedRows.end());

    QVector<Track> remaining;
    remaining.reserve(m_queue.size());
    int newQueueIndex = -1;
    int removedBeforeCurrent = 0;
    bool removedCurrent = false;
    int removeCursor = 0;
    for (int row = 0; row < m_queue.size(); ++row) {
        const bool remove = removeCursor < sortedRows.size() && sortedRows.at(removeCursor) == row;
        if (remove) {
            if (row < m_queueIndex) {
                ++removedBeforeCurrent;
            } else if (row == m_queueIndex) {
                removedCurrent = true;
            }
            ++removeCursor;
            continue;
        }
        remaining.push_back(m_queue.at(row));
    }

    if (!removedCurrent && m_queueIndex >= 0) {
        newQueueIndex = m_queueIndex - removedBeforeCurrent;
    } else if (!remaining.isEmpty()) {
        newQueueIndex = std::min(m_queueIndex - removedBeforeCurrent, static_cast<int>(remaining.size()) - 1);
    }

    m_queue = remaining;
    m_queueIndex = std::clamp(newQueueIndex, -1, static_cast<int>(m_queue.size()) - 1);
    m_playNextInsertIndex = std::clamp(m_queueIndex + 1, 0, static_cast<int>(m_queue.size()));
    syncQueueState();
    if (m_queueIndex >= 0 && m_queueIndex < m_queue.size()) {
        presentTrack(m_queue.at(m_queueIndex), false);
    } else {
        m_currentTrack = {};
        m_playerBar->setTrackText({});
        m_playerBar->setAlbumArt(QString());
        m_rightSidebar->setTrackInfo({});
        m_mpris->setTrack({});
        savePlaybackState(true);
    }
}

void MainWindow::clearQueue()
{
    m_queue.clear();
    m_queueIndex = -1;
    m_playNextInsertIndex = -1;
    m_currentTrack = {};
    syncQueueState();
    m_playerBar->setTrackText({});
    m_playerBar->setAlbumArt(QString());
    m_rightSidebar->setTrackInfo({});
    m_mpris->setTrack({});
    savePlaybackState(true);
}

void MainWindow::clearPlayNextPriority()
{
    if (m_queueIndex >= 0) {
        m_playNextInsertIndex = m_queueIndex + 1;
    } else {
        m_playNextInsertIndex = -1;
    }
    refreshPlayNextRange();
    saveQueueState();
}

void MainWindow::refreshPlayNextRange()
{
    if (m_queueStore != nullptr) {
        m_queueStore->setPlayNextRange(m_queueIndex + 1, m_playNextInsertIndex);
    }
}

void MainWindow::syncQueueState()
{
    // Defensively clamp the canonical indices so callers can mutate the queue
    // freely and rely on this one place to keep everything in range.
    if (m_queue.isEmpty()) {
        m_queueIndex = -1;
    } else {
        m_queueIndex = std::clamp(m_queueIndex, -1, static_cast<int>(m_queue.size()) - 1);
    }
    if (m_queueIndex < 0) {
        m_playNextInsertIndex = -1;
    } else if (m_playNextInsertIndex <= m_queueIndex || m_playNextInsertIndex > m_queue.size()) {
        m_playNextInsertIndex = m_queueIndex + 1;
    }

    // Push every derived view of the queue in lock-step. Crucially this includes
    // re-preparing the gapless "next" track: the playback backend pre-buffers
    // m_queue[m_queueIndex + 1], so reordering/inserting/removing without this
    // would let a stale track play next while the UI shows a different order.
    m_queueStore->setSnapshot(m_queue, m_queueIndex, m_queueIndex + 1, m_playNextInsertIndex);
    refreshPlayNextRange();
    if (m_panelSearch != nullptr) {
        m_panelSearch->refreshPanel(MainPanelId::Queue);
    }
    prepareNextQueueTrack();
    saveQueueState();
}

void MainWindow::playAlbumNow(const QString &albumTitle)
{
    playAlbumsNow(albumTitle.isEmpty() ? QStringList() : QStringList{albumTitle});
}

void MainWindow::playAlbumsNow(const QStringList &albumTitles)
{
    if (m_currentArtist.isEmpty() || albumTitles.isEmpty()) {
        return;
    }

    QVector<Track> tracks;
    for (const QString &albumTitle : albumTitles) {
        if (albumTitle.isEmpty()) {
            continue;
        }
        const QVector<Track> albumTracks = m_librarySource == LibrarySource::Mpd
            ? m_database->mpdTracksForArtist(m_currentArtist, mpdMusicDirectory(), albumTitle)
            : m_database->tracksForArtist(m_currentArtist, albumTitle);
        tracks += albumTracks;
    }
    if (tracks.isEmpty()) {
        return;
    }

    const int startIndex = static_cast<int>(m_queue.size());
    addTracksToQueue(tracks);
    playQueueIndex(startIndex);
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

void MainWindow::playQueueIndex(int index, bool notifyScrobbler, bool startPaused)
{
    if (index < 0 || index >= m_queue.size()) {
        return;
    }

    m_queueIndex = index;
    if (m_playNextInsertIndex <= m_queueIndex || m_playNextInsertIndex > m_queue.size()) {
        m_playNextInsertIndex = m_queueIndex + 1;
    }
    m_queueStore->setCurrentIndex(m_queueIndex);
    m_rightSidebar->setCurrentIndex(m_queueIndex, /*reveal=*/true);
    if (m_mainView == MainView::Queue) {
        m_queueScreen->revealCurrentPlaying();
    }
    refreshPlayNextRange();
    saveQueueState();
    playTrack(m_queue.at(m_queueIndex), notifyScrobbler, startPaused);
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
        if (!m_queue.isEmpty()) {
            playQueueIndex(m_queueIndex >= 0 ? m_queueIndex : 0);
        }
        return;
    }

    if (m_playback->state() == PlaybackBackend::State::Playing) {
        m_playback->pause();
    } else {
        m_playback->resume();
    }
}

void MainWindow::playFromMpris()
{
    if (m_playback->hasSource()) {
        m_playback->resume();
        return;
    }
    if (!m_queue.isEmpty()) {
        playQueueIndex(m_queueIndex >= 0 ? m_queueIndex : 0);
    }
}

void MainWindow::setVolumeFromMpris(double volume0To1)
{
    m_volume = std::clamp(volume0To1, 0.0, 1.0);
    m_playback->setVolume(m_volume);
    m_mpris->setVolume(m_volume);
}

void MainWindow::seekRelativeFromMpris(qint64 offsetMs)
{
    const qint64 duration = std::max<qint64>(0, m_playback->duration());
    const qint64 requested = m_playback->position() + offsetMs;
    m_playback->seek(duration > 0 ? std::clamp<qint64>(requested, 0, duration) : std::max<qint64>(0, requested));
}

void MainWindow::updateMprisCapabilities()
{
    m_mpris->setQueueCapabilities(m_queueIndex > 0,
                                  m_queueIndex >= 0 && m_queueIndex + 1 < m_queue.size(),
                                  m_playback->hasSource() || !m_queue.isEmpty());
}

void MainWindow::updatePlaybackPosition()
{
    m_playerBar->setPosition(m_playback->position(), m_playback->duration());
    m_mpris->setPositionMs(m_playback->position());
    m_mpris->setDurationMs(m_playback->duration());
    if (m_playback->state() == PlaybackBackend::State::Playing || m_playback->state() == PlaybackBackend::State::Paused) {
        schedulePlaybackStateSave(false);
    }
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

    m_playback->onGaplessTrackAdvanced();
    ++m_queueIndex;
    if (m_playNextInsertIndex <= m_queueIndex || m_playNextInsertIndex > m_queue.size()) {
        m_playNextInsertIndex = m_queueIndex + 1;
    }
    m_queueStore->setCurrentIndex(m_queueIndex);
    refreshPlayNextRange();
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
    ++m_currentArtGeneration;
    // Show the fallback immediately; the cache replies asynchronously and the
    // generation guard drops stale results from fast track changes.
    m_rightSidebar->setAlbumArt(QString());
    m_playerBar->setAlbumArt(QString());

    const QString resolvedPath = resolvedReadPathForTrack(m_currentTrack);
    const QString directory = resolvedPath.isEmpty() ? m_currentTrack.parentDir : QFileInfo(resolvedPath).absolutePath();
    if (m_artworkCache != nullptr) {
        m_artworkCache->requestArtwork(QStringLiteral("current"), directory, resolvedPath, m_currentArtGeneration);
    }
}

QString MainWindow::databasePath() const
{
    return QDir(AppPaths::dataDir()).filePath(QStringLiteral("library.sqlite"));
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
