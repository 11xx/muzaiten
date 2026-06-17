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
#include <QThread>
#include <QUuid>

#include <algorithm>

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

void AppCore::showWindow()
{
    if (!m_window) {
        m_window = new MainWindow(this);
    }
    m_window->show();
    m_window->raise();
    m_window->activateWindow();
}

void AppCore::releaseWindow()
{
    if (!m_window) return;
    m_window->deleteLater();
    m_window = nullptr;
}

void AppCore::quit()
{
    if (m_window) {
        m_window->deleteLater();
        m_window = nullptr;
    }
    QApplication::quit();
}
