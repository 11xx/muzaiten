#pragma once

#include "core/Track.h"

#include <QJsonObject>
#include <QObject>
#include <memory>

class ArtworkCache;
class Database;
class IpcServer;
class LastFmScrobbler;
class ListenBrainzScrobbler;
class ListenHistoryStore;
class ListenTracker;
class MainWindow;
class PlayEventRecorder;
class MprisService;
class PlaybackBackend;
class PlayerCore;
class PlaylistDatabase;
class QSystemTrayIcon;
class QThread;
class SettingsStore;

class AppCore final : public QObject {
    Q_OBJECT

public:
    explicit AppCore(QObject *parent = nullptr);
    ~AppCore() override;

    PlayerCore          *player() const;
    PlaybackBackend     *backend() const;
    Database            *database() const;
    PlaylistDatabase    *playlistDatabase() const;
    SettingsStore       *settings() const;
    ArtworkCache        *artworkCache() const;
    ListenHistoryStore  *listenHistory() const;
    ListenTracker       *listenTracker() const;
    PlayEventRecorder   *playEventRecorder() const;
    MprisService        *mpris() const;
    IpcServer           *ipc() const;
    MainWindow          *window() const;

    ListenBrainzScrobbler *listenBrainzScrobbler() const;
    LastFmScrobbler       *lastFmScrobbler() const;
    QThread               *listenBrainzThread() const;
    QThread               *lastFmThread() const;

    QString databasePath() const;
    QString playlistDatabasePath() const;
    QString listenHistoryPath() const;
    bool scrobbleOffline() const;
    bool trayAvailable() const;
    bool isQuitting() const;

    void setTrayAlwaysVisible(bool visible);

public slots:
    void showWindow();
    void releaseWindow();
    void quit();

    // Called by MainWindow's resumePlaybackAt to resume scrobble state
    // after restoring a saved playback position.
    void resumeScrobblers(const Track &track, qint64 elapsedMs, bool playing);

private:
    void setupMprisWiring();
    void setupScrobbleWiring();
    void setupIpcHandler();
    void setupTrayIcon();
    void restoreSavedPlayback();
    void updateMprisCapabilities();
    void releaseIdleMemory();
    void restoreInteractiveMemory();
    void notifyScrobblersTrackStarted(const Track &track);
    QJsonObject handleIpcCommand(const QString &command, const QJsonObject &args);
    QJsonObject ipcStatus() const;

    std::unique_ptr<Database>          m_database;
    std::unique_ptr<PlaylistDatabase>  m_playlistDb;
    std::unique_ptr<SettingsStore>     m_state;
    std::unique_ptr<ArtworkCache>      m_artworkCache;
    std::unique_ptr<ListenHistoryStore> m_listenHistory;
    PlayerCore       *m_player = nullptr;
    PlaybackBackend  *m_playback = nullptr;
    ListenTracker    *m_listenTracker = nullptr;
    PlayEventRecorder *m_playEventRecorder = nullptr;
    QThread          *m_listenBrainzThread = nullptr;
    ListenBrainzScrobbler *m_listenBrainzScrobbler = nullptr;
    QThread          *m_lastFmThread = nullptr;
    LastFmScrobbler  *m_lastFmScrobbler = nullptr;
    MprisService     *m_mpris = nullptr;
    IpcServer        *m_ipc = nullptr;
    QSystemTrayIcon  *m_tray = nullptr;
    MainWindow       *m_window = nullptr;
    bool              m_quitting = false;
    bool              m_resumeDone = false;
    bool              m_trayAlwaysVisible = false;
    // Track-start attribution for play events. currentIndexChanged(index,
    // userInitiated) always precedes the matching currentTrackChanged, and
    // aboutToInjectLibraryTrack precedes it for library-shuffle injections, so
    // these carry the attribution forward to the currentTrackChanged handler.
    bool              m_nextStartUserInitiated = false;
    bool              m_nextStartInjected = false;
};
