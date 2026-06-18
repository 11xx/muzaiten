#include "app/AppCore.h"

#include "app/AppPaths.h"
#include "db/Database.h"
#include "db/PlaylistDatabase.h"
#include "db/SettingsStore.h"
#include "fs/LinkRoot.h"
#include "ipc/IpcServer.h"
#include "mpris/MprisService.h"
#include "playback/GStreamerPlaybackBackend.h"
#include "playback/PlaybackBackend.h"
#include "player/PlayerCore.h"
#include "scanner/ArtworkCache.h"
#include "scrobble/LastFmScrobbler.h"
#include "scrobble/ListenBrainzScrobbler.h"
#include "scrobble/ListenHistoryStore.h"
#include "scrobble/ListenTracker.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cmath>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace {

QString repeatModeToString(RepeatMode mode)
{
    switch (mode) {
    case RepeatMode::All:
        return QStringLiteral("all");
    case RepeatMode::One:
        return QStringLiteral("one");
    case RepeatMode::Off:
        break;
    }
    return QStringLiteral("off");
}

QString shuffleModeToString(ShuffleMode mode)
{
    switch (mode) {
    case ShuffleMode::Queue:
        return QStringLiteral("queue");
    case ShuffleMode::Library:
        return QStringLiteral("library");
    case ShuffleMode::Off:
        break;
    }
    return QStringLiteral("off");
}

QJsonObject trackJson(const Track &track, int index = -1)
{
    QJsonObject json{
        {QStringLiteral("path"), track.path},
        {QStringLiteral("title"), track.title.isEmpty() ? track.filename : track.title},
        {QStringLiteral("artist"), track.artistName},
        {QStringLiteral("album"), track.albumTitle},
        {QStringLiteral("duration"), std::round(static_cast<double>(track.durationMs) / 10.0) / 100.0},
    };
    if (index >= 0) {
        json.insert(QStringLiteral("index"), index);
    }
    if (track.effectiveRating0To100 >= 0) {
        json.insert(QStringLiteral("rating"), track.effectiveRating0To100);
    }
    return json;
}

} // namespace

AppCore::AppCore(QObject *parent)
    : QObject(parent)
{
    m_database = std::make_unique<Database>(QStringLiteral("main-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!m_database->open(databasePath())) {
        qWarning("AppCore: failed to open database: %s", qPrintable(m_database->lastError()));
    }

    m_playlistDb = std::make_unique<PlaylistDatabase>(QStringLiteral("playlists-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!m_playlistDb->open(playlistDatabasePath())) {
        qWarning("AppCore: failed to open playlist database: %s", qPrintable(m_playlistDb->lastError()));
    }

    m_state = std::make_unique<SettingsStore>(QDir(AppPaths::stateDir()).filePath(QStringLiteral("state.sqlite")));
    AppPaths::writeDefaultConfigIfMissing();

    const int artworkSize = std::clamp(m_state->setting(QStringLiteral("artwork.size"), QStringLiteral("1024")).toInt(), 128, 4096);
    m_artworkCache = std::make_unique<ArtworkCache>(QDir(AppPaths::cacheDir()).filePath(QStringLiteral("artwork.sqlite")), artworkSize);

    m_listenHistory = std::make_unique<ListenHistoryStore>(listenHistoryPath());

    m_player = new PlayerCore(new GStreamerPlaybackBackend(), this);
    m_playback = m_player->backend();
    m_player->setPathResolver([this](const Track &track) {
        if (track.path.isEmpty()) {
            return QString();
        }
        const PathResolver resolver(m_database->linkRoots());
        const PathResolution resolution = resolver.resolveLocalPath(track.path, PathUse::Read);
        return resolution.preferredPath;
    });
    m_player->setRandomTrackProvider([this](int count, const QSet<QString> &excludePaths) {
        return m_database ? m_database->randomTracks(count, excludePaths) : QVector<Track>{};
    });

    m_mpris = new MprisService(this);
    m_mpris->setDatabase(m_database.get());

    m_listenTracker = new ListenTracker(this);
    connect(m_listenTracker, &ListenTracker::listenReached, this, [this](const Track &track, qint64 startedAtSecs) {
        const bool oweListenBrainz = m_database->setting(QStringLiteral("listenbrainz.enabled"), QStringLiteral("false")) == QStringLiteral("true");
        const bool oweLastFm = m_database->setting(QStringLiteral("lastfm.enabled"), QStringLiteral("false")) == QStringLiteral("true");
        m_listenHistory->recordListen(track, startedAtSecs, oweLastFm, oweListenBrainz);
        if (oweListenBrainz) {
            QMetaObject::invokeMethod(m_listenBrainzScrobbler, "uploadBacklog", Qt::QueuedConnection);
        }
        if (oweLastFm) {
            QMetaObject::invokeMethod(m_lastFmScrobbler, "uploadBacklog", Qt::QueuedConnection);
        }
    });

    m_listenBrainzThread = new QThread(this);
    m_listenBrainzScrobbler = new ListenBrainzScrobbler;
    m_listenBrainzScrobbler->moveToThread(m_listenBrainzThread);
    connect(m_listenBrainzThread, &QThread::finished, m_listenBrainzScrobbler, &QObject::deleteLater);
    m_listenBrainzThread->start();

    m_lastFmThread = new QThread(this);
    m_lastFmScrobbler = new LastFmScrobbler;
    m_lastFmScrobbler->moveToThread(m_lastFmThread);
    connect(m_lastFmThread, &QThread::finished, m_lastFmScrobbler, &QObject::deleteLater);
    m_lastFmThread->start();

    m_ipc = new IpcServer(this);

    setupMprisWiring();
    setupScrobbleWiring();
    setupIpcHandler();
    setupTrayIcon();
    // Playback resume is deferred to the first showWindow(): the saved queue is
    // loaded by MainWindow's constructor (loadQueueState), so the player's queue
    // is empty here. restoreSavedPlayback() is guarded to run once per process.
}

AppCore::~AppCore()
{
    if (m_listenBrainzThread != nullptr) {
        m_listenBrainzThread->quit();
        m_listenBrainzThread->wait(3000);
    }
    if (m_lastFmThread != nullptr) {
        m_lastFmThread->quit();
        m_lastFmThread->wait(3000);
    }
}

PlayerCore *AppCore::player() const { return m_player; }
PlaybackBackend *AppCore::backend() const { return m_playback; }
Database *AppCore::database() const { return m_database.get(); }
PlaylistDatabase *AppCore::playlistDatabase() const { return m_playlistDb.get(); }
SettingsStore *AppCore::settings() const { return m_state.get(); }
ArtworkCache *AppCore::artworkCache() const { return m_artworkCache.get(); }
ListenHistoryStore *AppCore::listenHistory() const { return m_listenHistory.get(); }
ListenTracker *AppCore::listenTracker() const { return m_listenTracker; }
MprisService *AppCore::mpris() const { return m_mpris; }
IpcServer *AppCore::ipc() const { return m_ipc; }
MainWindow *AppCore::window() const { return m_window; }
ListenBrainzScrobbler *AppCore::listenBrainzScrobbler() const { return m_listenBrainzScrobbler; }
LastFmScrobbler *AppCore::lastFmScrobbler() const { return m_lastFmScrobbler; }
QThread *AppCore::listenBrainzThread() const { return m_listenBrainzThread; }
QThread *AppCore::lastFmThread() const { return m_lastFmThread; }

QString AppCore::databasePath() const
{
    return QDir(AppPaths::dataDir()).filePath(QStringLiteral("library.sqlite"));
}

QString AppCore::playlistDatabasePath() const
{
    return QDir(AppPaths::dataDir()).filePath(QStringLiteral("playlists.sqlite"));
}

QString AppCore::listenHistoryPath() const
{
    return QDir(AppPaths::dataDir()).filePath(QStringLiteral("history.sqlite"));
}

bool AppCore::scrobbleOffline() const
{
    return m_database->setting(QStringLiteral("scrobble.offline"), QStringLiteral("false")) == QStringLiteral("true");
}

bool AppCore::trayAvailable() const
{
    return m_tray != nullptr;
}

bool AppCore::isQuitting() const
{
    return m_quitting;
}

void AppCore::setTrayAlwaysVisible(bool visible)
{
    m_trayAlwaysVisible = visible;
    m_state->setSetting(QStringLiteral("tray.alwaysVisible"), visible ? QStringLiteral("true") : QStringLiteral("false"));
    if (m_tray) {
        m_tray->setVisible(visible || m_window == nullptr);
    }
}

void AppCore::showWindow()
{
    if (!m_window) {
        restoreInteractiveMemory();
        m_window = new MainWindow(this);
        // First window of the process: its constructor loaded the saved queue,
        // so the player now has tracks to resume into. Guarded to run once.
        restoreSavedPlayback();
    }
    m_window->show();
    m_window->raise();
    m_window->activateWindow();
    if (m_tray && !m_trayAlwaysVisible) {
        m_tray->hide();
    }
}

void AppCore::releaseWindow()
{
    if (!m_window) return;
    m_window->persistViewState();
    connect(m_window, &QObject::destroyed, this, [this]() {
        if (m_window == nullptr) {
            releaseIdleMemory();
        }
    }, Qt::SingleShotConnection);
    m_window->deleteLater();
    m_window = nullptr;
    if (m_tray && !m_trayAlwaysVisible) {
        m_tray->show();
    }
}

void AppCore::releaseIdleMemory()
{
    if (m_database) {
        m_database->releaseCacheMemory();
    }
    if (m_playlistDb) {
        m_playlistDb->releaseCacheMemory();
    }
    if (m_state) {
        m_state->releaseCacheMemory();
    }
    if (m_listenHistory) {
        m_listenHistory->releaseCacheMemory();
    }
    if (m_artworkCache) {
        QMetaObject::invokeMethod(m_artworkCache.get(), "releaseCacheMemory", Qt::QueuedConnection);
    }

#if defined(__GLIBC__)
    malloc_trim(0);
#endif
}

void AppCore::restoreInteractiveMemory()
{
    if (m_database) {
        m_database->restoreCacheMemory();
    }
}

void AppCore::quit()
{
    m_quitting = true;
    // close() runs MainWindow::closeEvent, which force-saves playback, queue,
    // explorer and view state. With m_quitting set, closeEvent accepts instead
    // of hiding/releasing. (If the window was already released to tray, its
    // state was saved when it closed, so there is nothing to do here.)
    if (m_window) {
        m_window->close();
    }
    QApplication::quit();
}

void AppCore::setupMprisWiring()
{
    connect(m_mpris, &MprisService::raiseRequested, this, &AppCore::showWindow);
    connect(m_mpris, &MprisService::previousRequested, m_player, &PlayerCore::previous);
    connect(m_mpris, &MprisService::nextRequested, m_player, &PlayerCore::next);
    connect(m_mpris, &MprisService::repeatModeRequested, m_player, &PlayerCore::setRepeatMode);
    connect(m_mpris, &MprisService::shuffleModeRequested, m_player, &PlayerCore::setShuffleMode);
    connect(m_mpris, &MprisService::pauseRequested, m_playback, &PlaybackBackend::pause);
    connect(m_mpris, &MprisService::playPauseRequested, m_player, &PlayerCore::togglePlayPause);
    connect(m_mpris, &MprisService::stopRequested, m_playback, &PlaybackBackend::stop);
    connect(m_mpris, &MprisService::playRequested, m_player, &PlayerCore::play);
    connect(m_mpris, &MprisService::seekRequested, m_playback, &PlaybackBackend::seek);
    connect(m_mpris, &MprisService::relativeSeekRequested, m_player, &PlayerCore::seekRelative);
    connect(m_mpris, &MprisService::volumeRequested, m_player, &PlayerCore::setVolume);

    connect(m_player, &PlayerCore::repeatModeChanged, this, [this](RepeatMode mode) {
        m_mpris->setRepeatMode(mode);
        m_state->setSetting(QStringLiteral("playback.repeatMode"), repeatModeToString(mode));
    });
    connect(m_player, &PlayerCore::shuffleModeChanged, this, [this](ShuffleMode mode) {
        m_mpris->setShuffleMode(mode);
        m_state->setSetting(QStringLiteral("playback.shuffleMode"), shuffleModeToString(mode));
    });
    connect(m_player, &PlayerCore::libraryShufflePercentChanged, this, [this](int percent) {
        m_state->setSetting(QStringLiteral("playback.libraryShufflePercent"), QString::number(percent));
    });
    connect(m_player, &PlayerCore::currentTrackChanged, this, [this](const Track &track, bool) {
        m_mpris->setTrack(track);
        m_mpris->setDurationMs(track.durationMs);
        m_mpris->setPositionMs(0);
        updateMprisCapabilities();
    });
    connect(m_player, &PlayerCore::playbackCleared, this, [this]() {
        m_mpris->setTrack({});
    });
    connect(m_player, &PlayerCore::volumeChanged, this, [this](double volume0To1) {
        const int percent = std::clamp(static_cast<int>(std::lround(volume0To1 * 100.0)), 0, 100);
        m_mpris->setVolume(static_cast<double>(percent) / 100.0);
        m_state->setSetting(QStringLiteral("volume"), QString::number(percent));
    });
    connect(m_playback, &PlaybackBackend::stateChanged, this, [this](PlaybackBackend::State state) {
        const bool playing = state == PlaybackBackend::State::Playing;
        m_mpris->setPlaybackState(state);
        m_listenTracker->playbackStateChanged(playing);
        QMetaObject::invokeMethod(m_listenBrainzScrobbler, "playbackStateChanged", Qt::QueuedConnection, Q_ARG(bool, playing));
        QMetaObject::invokeMethod(m_lastFmScrobbler, "playbackStateChanged", Qt::QueuedConnection, Q_ARG(bool, playing));
        updateMprisCapabilities();
    });
    connect(m_playback, &PlaybackBackend::positionChanged, this, [this]() {
        m_mpris->setPositionMs(std::max<qint64>(0, m_playback->position()));
    });
    connect(m_playback, &PlaybackBackend::durationChanged, this, [this]() {
        m_mpris->setDurationMs(m_playback->duration());
    });
}

void AppCore::setupScrobbleWiring()
{
    connect(m_player, &PlayerCore::currentTrackChanged, this, [this](const Track &track, bool notifyScrobbler) {
        if (notifyScrobbler) {
            notifyScrobblersTrackStarted(track);
        }
    });
}

void AppCore::setupIpcHandler()
{
    m_ipc->setHandler([this](const QString &command, const QJsonObject &args) {
        return handleIpcCommand(command, args);
    });
    if (!m_ipc->listen()) {
        qWarning("muzaiten: IPC socket unavailable: %s", qPrintable(m_ipc->lastError()));
    }
}

void AppCore::setupTrayIcon()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }
    QIcon icon = QApplication::windowIcon();
    if (icon.isNull()) {
        icon = QIcon(QStringLiteral(":/icons/muzaiten.svg"));
    }
    m_tray = new QSystemTrayIcon(icon, this);
    m_tray->setToolTip(QStringLiteral("muzaiten"));
    QApplication::setQuitOnLastWindowClosed(false);

    auto *menu = new QMenu;
    menu->addAction(QStringLiteral("Unhide"), this, &AppCore::showWindow);
    menu->addSeparator();
    menu->addAction(QStringLiteral("Play/Pause"), m_player, &PlayerCore::togglePlayPause);
    menu->addAction(QStringLiteral("Next"), m_player, &PlayerCore::next);
    menu->addAction(QStringLiteral("Previous"), m_player, &PlayerCore::previous);
    menu->addAction(QStringLiteral("Stop"), m_playback, &PlaybackBackend::stop);
    menu->addSeparator();
    menu->addAction(QStringLiteral("Quit"), this, &AppCore::quit);
    m_tray->setContextMenu(menu);
    m_trayAlwaysVisible = m_state->setting(QStringLiteral("tray.alwaysVisible"), QStringLiteral("false")) == QStringLiteral("true");
    // Default: tray visible only while the window is hidden.  The window is
    // shown on startup, so start hidden unless the user prefers always-visible.
    m_tray->setVisible(m_trayAlwaysVisible);

    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger) {
            showWindow();
        } else if (reason == QSystemTrayIcon::MiddleClick) {
            m_player->togglePlayPause();
        }
    });
    connect(m_player, &PlayerCore::currentTrackChanged, this, [this](const Track &track, bool) {
        const QString title = track.title.isEmpty() ? track.filename : track.title;
        m_tray->setToolTip(track.path.isEmpty()
                               ? QStringLiteral("muzaiten")
                               : QStringLiteral("%1 - %2").arg(track.artistName, title));
    });
    connect(m_player, &PlayerCore::playbackCleared, this, [this]() {
        m_tray->setToolTip(QStringLiteral("muzaiten"));
    });
}

void AppCore::restoreSavedPlayback()
{
    if (m_resumeDone) return;
    m_resumeDone = true;

    const bool enabled = m_state->setting(QStringLiteral("playback.restoreStateEnabled"), QStringLiteral("true")) == QStringLiteral("true");
    if (!enabled) return;

    const QJsonObject root = QJsonDocument::fromJson(m_state->setting(QStringLiteral("playback.state")).toUtf8()).object();
    const int queueIndex = root.value(QStringLiteral("queueIndex")).toInt(-1);
    const QString trackPath = root.value(QStringLiteral("trackPath")).toString();
    const qint64 positionMs = root.value(QStringLiteral("positionMs")).toString().toLongLong();
    const QString state = root.value(QStringLiteral("state")).toString(QStringLiteral("stopped"));
    if (queueIndex < 0 || queueIndex >= m_player->queue().size() || trackPath.isEmpty()
        || m_player->queue().at(queueIndex).path != trackPath) {
        return;
    }
    if (state != QStringLiteral("playing") && state != QStringLiteral("paused")) {
        return;
    }
    m_player->playAt(queueIndex, false, state != QStringLiteral("playing"), false);
    if (positionMs > 0) {
        QTimer::singleShot(250, this, [this, positionMs]() {
            m_playback->seek(positionMs);
        });
    }
}

void AppCore::updateMprisCapabilities()
{
    m_mpris->setQueueCapabilities(m_player->queueIndex() > 0,
                                  m_player->queueIndex() >= 0 && m_player->queueIndex() + 1 < m_player->queue().size(),
                                  m_playback->hasSource() || !m_player->queue().isEmpty());
}

void AppCore::notifyScrobblersTrackStarted(const Track &track)
{
    m_listenTracker->trackStarted(track);
    QMetaObject::invokeMethod(m_listenBrainzScrobbler, "trackStarted", Qt::QueuedConnection, Q_ARG(Track, track));
    QMetaObject::invokeMethod(m_lastFmScrobbler, "trackStarted", Qt::QueuedConnection, Q_ARG(Track, track));
}

void AppCore::resumeScrobblers(const Track &track, qint64 elapsedMs, bool playing)
{
    m_listenTracker->resumeTrack(track, elapsedMs, playing);
    QMetaObject::invokeMethod(m_listenBrainzScrobbler, "resumeTrack", Qt::QueuedConnection,
                              Q_ARG(Track, track), Q_ARG(qint64, elapsedMs), Q_ARG(bool, playing));
    QMetaObject::invokeMethod(m_lastFmScrobbler, "resumeTrack", Qt::QueuedConnection,
                              Q_ARG(Track, track), Q_ARG(qint64, elapsedMs), Q_ARG(bool, playing));
}

QJsonObject AppCore::ipcStatus() const
{
    return QJsonDocument::fromJson(m_mpris->currentTrackJson().toUtf8()).object();
}

QJsonObject AppCore::handleIpcCommand(const QString &command, const QJsonObject &args)
{
    const auto error = [](const QString &message) {
        return QJsonObject{{QStringLiteral("error"), message}};
    };
    const auto status = [this] {
        return QJsonObject{{QStringLiteral("status"), ipcStatus()}};
    };

    if (command == QLatin1String("status")) {
        return ipcStatus();
    }
    if (command == QLatin1String("raise")) {
        showWindow();
        return status();
    }
    if (command == QLatin1String("play")) {
        m_player->play();
        return status();
    }
    if (command == QLatin1String("pause")) {
        m_playback->pause();
        return status();
    }
    if (command == QLatin1String("play-pause")) {
        m_player->togglePlayPause();
        return status();
    }
    if (command == QLatin1String("stop")) {
        m_playback->stop();
        return status();
    }
    if (command == QLatin1String("next")) {
        m_player->next();
        return status();
    }
    if (command == QLatin1String("prev")) {
        m_player->previous();
        return status();
    }
    if (command == QLatin1String("seek")) {
        if (args.contains(QStringLiteral("offset_ms")) || args.contains(QStringLiteral("offsetMs"))) {
            const QJsonValue offset = args.contains(QStringLiteral("offset_ms"))
                ? args.value(QStringLiteral("offset_ms"))
                : args.value(QStringLiteral("offsetMs"));
            m_player->seekRelative(static_cast<qint64>(offset.toDouble()));
        } else if (args.contains(QStringLiteral("ms"))) {
            m_playback->seek(std::max<qint64>(0, static_cast<qint64>(args.value(QStringLiteral("ms")).toDouble())));
        } else {
            return error(QStringLiteral("seek needs \"ms\" or \"offset_ms\""));
        }
        return status();
    }
    if (command == QLatin1String("volume")) {
        double percent = 0.0;
        if (args.contains(QStringLiteral("percent"))) {
            percent = args.value(QStringLiteral("percent")).toDouble();
        } else if (args.contains(QStringLiteral("delta_percent")) || args.contains(QStringLiteral("deltaPercent"))) {
            const QJsonValue delta = args.contains(QStringLiteral("delta_percent"))
                ? args.value(QStringLiteral("delta_percent"))
                : args.value(QStringLiteral("deltaPercent"));
            percent = m_player->volume() * 100.0 + delta.toDouble();
        } else {
            return error(QStringLiteral("volume needs \"percent\" or \"delta_percent\""));
        }
        m_player->setVolume(percent / 100.0);
        return status();
    }
    if (command == QLatin1String("queue")) {
        QJsonArray tracks;
        for (int i = 0; i < m_player->queue().size(); ++i) {
            tracks.append(trackJson(m_player->queue().at(i), i));
        }
        return QJsonObject{{QStringLiteral("index"), m_player->queueIndex()},
                           {QStringLiteral("tracks"), tracks}};
    }
    if (command == QLatin1String("queue-jump")) {
        const int index = args.value(QStringLiteral("index")).toInt(-1);
        if (index < 0 || index >= m_player->queue().size()) {
            return error(QStringLiteral("queue-jump needs \"index\" in 0..%1").arg(m_player->queue().size() - 1));
        }
        m_player->playAt(index, true, false, /*explicitJump=*/true);
        return status();
    }
    if (command == QLatin1String("search")) {
        const QString text = args.value(QStringLiteral("query")).toString().trimmed();
        if (text.isEmpty()) {
            return error(QStringLiteral("search needs a non-empty \"query\""));
        }
        const int limit = std::clamp(args.value(QStringLiteral("limit")).toInt(50), 1, 500);
        QJsonArray results;
        for (const Track &track : m_database->searchTracksLike(text, limit)) {
            results.append(trackJson(track));
        }
        return QJsonObject{{QStringLiteral("results"), results}};
    }
    if (command == QLatin1String("play-file")) {
        const QString path = QFileInfo(args.value(QStringLiteral("path")).toString()).absoluteFilePath();
        if (path.isEmpty() || !QFileInfo::exists(path)) {
            return error(QStringLiteral("play-file needs an existing \"path\""));
        }
        Track track = m_database->trackForPath(path);
        if (track.path.isEmpty()) {
            const QFileInfo info(path);
            track.path = path;
            track.parentDir = info.absolutePath();
            track.filename = info.fileName();
            track.title = info.completeBaseName();
        }
        m_player->appendAndPlay(track);
        return status();
    }
    if (command == QLatin1String("enqueue")) {
        const QJsonArray pathsJson = args.value(QStringLiteral("paths")).toArray();
        if (pathsJson.isEmpty()) {
            return error(QStringLiteral("enqueue needs a non-empty \"paths\" array"));
        }
        QVector<Track> tracks;
        tracks.reserve(static_cast<int>(pathsJson.size()));
        for (const QJsonValue &value : pathsJson) {
            const QString path = QFileInfo(value.toString()).absoluteFilePath();
            if (path.isEmpty() || !QFileInfo::exists(path)) {
                continue;
            }
            Track track = m_database->trackForPath(path);
            if (track.path.isEmpty()) {
                const QFileInfo info(path);
                track.path = path;
                track.parentDir = info.absolutePath();
                track.filename = info.fileName();
                track.title = info.completeBaseName();
            }
            tracks.push_back(track);
        }
        if (tracks.isEmpty()) {
            return error(QStringLiteral("enqueue: none of the given paths exist"));
        }
        const bool play = args.value(QStringLiteral("play")).toBool();
        const bool next = args.value(QStringLiteral("next")).toBool();
        const int startIndex = next ? m_player->queueIndex() + 1 : static_cast<int>(m_player->queue().size());
        if (next) {
            m_player->playTracksNext(tracks);
        } else {
            m_player->appendTracks(tracks);
        }
        if (play) {
            m_player->playAt(startIndex, true, false, /*explicitJump=*/true);
        }
        QJsonObject reply = status();
        reply.insert(QStringLiteral("enqueued"), static_cast<int>(tracks.size()));
        return reply;
    }
    if (command == QLatin1String("rate")) {
        if (m_player->currentTrack().path.isEmpty()) {
            return error(QStringLiteral("no current track to rate"));
        }
        int rating = -1;
        if (!args.value(QStringLiteral("clear")).toBool()) {
            rating = args.contains(QStringLiteral("rating"))
                ? args.value(QStringLiteral("rating")).toInt(-1)
                : args.value(QStringLiteral("rating0To100")).toInt(-1);
            if (rating < 0 || rating > 100) {
                return error(QStringLiteral("rate needs \"rating\" in 0..100 or \"clear\": true"));
            }
        }
        const Track rated = m_player->currentTrack();
        if (rating >= 0) {
            m_database->setUserTrackRating(rated.path, rating);
            m_database->setPendingTrackRatingWrite(rated.path, rating, QStringLiteral("pending"));
        } else {
            m_database->clearUserTrackRating(rated.path);
            m_database->clearPendingTrackRatingWrite(rated.path);
        }
        m_player->updateTrackRating(rated.path, rating >= 0 ? rating : rated.rating0To100, rating >= 0);
        return status();
    }
    return error(QStringLiteral("unknown command \"%1\"").arg(command));
}
