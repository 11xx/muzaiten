#pragma once

#include "core/Track.h"
#include "scrobble/ScrobbleBackfill.h"

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
class RadioSession;
class QSystemTrayIcon;
class QThread;
class SettingsStore;

class AppCore final : public QObject {
    Q_OBJECT

public:
    // Main-thread mirror of the backfill worker's state, refreshed from its
    // progress/finished/failed signals. Consumed by the Scrobblers menu (see
    // MainWindow::updateBackfillStatusDisplay) and the IPC "status" reply.
    struct BackfillStatus {
        QString service;          // ListenHistoryStore::ListenBrainz/LastFm, empty when never run
        bool running = false;
        int processed = 0;
        int inserted = 0;
        qint64 reachedTs = 0;      // ListenBrainz cursor reached; 0 for Last.fm/unknown
        qint64 totalListens = 0;   // ListenBrainz listen-count total; 0 when unknown/Last.fm
        QString lastMessage;       // outcome of the most recent finished/failed run
    };

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

    // Start a rule-based radio session seeded from a library track: clears the
    // queue, plays the seed, and installs a scored provider that extends the
    // queue with recommendations. Returns false (no state change) when the seed
    // path is not a known library track.
    bool startRadio(const QString &seedPath);
    // End the session: deactivates radio and tears down the provider/session.
    // Queue contents stay as they are.
    void stopRadio();

    QString databasePath() const;
    QString playlistDatabasePath() const;
    QString listenHistoryPath() const;
    bool scrobbleOffline() const;
    bool trayAvailable() const;
    bool isQuitting() const;

    void setTrayAlwaysVisible(bool visible);

    // Shared start path for the scrobbler backfill, used by both the IPC
    // handler and the Scrobblers menu. Returns "started" | "already-running" |
    // "missing-credentials" | "unknown-service". Starting ListenBrainz clears
    // any stale canceled flag so a later interruption can auto-resume again.
    QString startBackfill(const QString &service);
    // Explicit cancel: the ONLY thing that stops eager auto-resume. Marks the
    // ListenBrainz canceled flag (via the main-thread store, so it survives
    // even if the app quits before the worker acknowledges) then asks the
    // worker to abort.
    void cancelBackfill();
    BackfillStatus backfillStatus() const { return m_backfillStatus; }

signals:
    // Emitted on every backfill progress/finished/failed update, so the
    // Scrobblers menu (which stays open while browsing) can refresh live.
    void backfillStatusChanged();

public slots:
    void showWindow();
    void releaseWindow();
    void quit();

    // Called by MainWindow's resumePlaybackAt to resume scrobble state
    // after restoring a saved playback position.
    void resumeScrobblers(const Track &track, qint64 elapsedMs, bool playing);

private:
    // ~20s after construction, eagerly resume an interrupted ListenBrainz
    // import if one is pending (see startBackfill's cancel-vs-interrupt rule).
    void maybeAutoResumeListenBrainzBackfill();
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
    // Build the scrobbler-backfill match index from the library DB (folded
    // artist+title and recording MBID -> track path).
    ScrobbleBackfill::LibraryIndex buildLibraryIndex() const;

    std::unique_ptr<Database>          m_database;
    std::unique_ptr<PlaylistDatabase>  m_playlistDb;
    std::unique_ptr<SettingsStore>     m_state;
    std::unique_ptr<ArtworkCache>      m_artworkCache;
    std::unique_ptr<ListenHistoryStore> m_listenHistory;
    std::unique_ptr<RadioSession>      m_radioSession;
    PlayerCore       *m_player = nullptr;
    PlaybackBackend  *m_playback = nullptr;
    ListenTracker    *m_listenTracker = nullptr;
    PlayEventRecorder *m_playEventRecorder = nullptr;
    QThread          *m_listenBrainzThread = nullptr;
    ListenBrainzScrobbler *m_listenBrainzScrobbler = nullptr;
    QThread          *m_lastFmThread = nullptr;
    LastFmScrobbler  *m_lastFmScrobbler = nullptr;
    QThread          *m_scrobbleBackfillThread = nullptr;
    ScrobbleBackfill *m_scrobbleBackfill = nullptr;
    // Main-thread mirror of the worker's busy state, so the IPC trigger can
    // report "already running" without a cross-thread query.
    bool              m_backfillRunning = false;
    BackfillStatus    m_backfillStatus;
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
