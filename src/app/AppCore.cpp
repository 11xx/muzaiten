#include "app/AppCore.h"

#include "app/AppPaths.h"
#include "core/FoldKey.h"
#include "core/GenreTags.h"
#include "db/Database.h"
#include "db/PlaylistDatabase.h"
#include "db/SettingsStore.h"
#include "fs/LinkRoot.h"
#include "ipc/IpcServer.h"
#include "mpris/MprisService.h"
#include "playback/GStreamerPlaybackBackend.h"
#include "playback/PlaybackBackend.h"
#include "player/PlayerCore.h"
#include "reco/AffinityPool.h"
#include "reco/RadioFilters.h"
#include "reco/RadioMix.h"
#include "reco/RadioSession.h"
#include "reco/ReasonText.h"
#include "reco/TrackScorer.h"
#include "scanner/ArtworkCache.h"
#include "scrobble/LastFmCredentials.h"
#include "scrobble/LastFmScrobbler.h"
#include "scrobble/ListenBrainzScrobbler.h"
#include "scrobble/ListenHistoryStore.h"
#include "scrobble/ListenTracker.h"
#include "scrobble/PlayEventRecorder.h"
#include "scrobble/ScrobbleBackfill.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <optional>

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
    case ShuffleMode::Radio:
        return QStringLiteral("radio");
    case ShuffleMode::Off:
        break;
    }
    return QStringLiteral("off");
}

std::optional<Database::TrackFlag> trackFlagFromName(const QString &flag)
{
    if (flag == QStringLiteral("never_radio")) {
        return Database::TrackFlag::NeverRadio;
    }
    if (flag == QStringLiteral("no_learn")) {
        return Database::TrackFlag::NoLearn;
    }
    return std::nullopt;
}

// Leading 4-digit release year from a track's date fields (original date wins);
// 0 when neither carries a parseable year.
int trackYear(const Track &track)
{
    const QString date = !track.originalDate.isEmpty() ? track.originalDate : track.date;
    if (date.size() < 4) {
        return 0;
    }
    bool ok = false;
    const int year = QStringView(date).left(4).toInt(&ok);
    return ok ? year : 0;
}

QStringList canonicalRadioGenres(const QStringList &foldedGenres, const QHash<QString, QString> &aliases,
                                 const QSet<QString> &ignored)
{
    QStringList genres;
    QSet<QString> seen;
    genres.reserve(foldedGenres.size());
    for (const QString &genre : foldedGenres) {
        const QString canonical = GenreTags::canonical(genre, aliases);
        if (canonical.isEmpty() || GenreTags::isNonGenre(canonical) || ignored.contains(canonical)
            || seen.contains(canonical)) {
            continue;
        }
        seen.insert(canonical);
        genres.push_back(canonical);
    }
    return genres;
}

TrackScorer::Candidate candidateFromRow(const RadioCandidateRow &row, const QHash<QString, QString> &genreAliases,
                                        const QSet<QString> &ignoredRadioGenres)
{
    TrackScorer::Candidate candidate;
    candidate.path = row.path;
    candidate.songKey = FoldKey::songKey(row.mbRecordingId, row.artistName, row.title);
    candidate.artistFolded = FoldKey::fold(row.artistName);
    candidate.albumKey = FoldKey::albumGroupKey(row.releaseGroupId, row.albumArtistName, row.albumTitle);
    candidate.genresFolded = canonicalRadioGenres(row.genresFolded, genreAliases, ignoredRadioGenres);
    candidate.year = row.year;
    candidate.effectiveRating0To100 = row.effectiveRating0To100;
    candidate.hasUserRating = row.hasUserRating;
    return candidate;
}

// Radio settings defaults/limits (plans/music-recommendation-plan.md, "Batch
// radio queue"). batchSize == 1 must reproduce the pre-batch, pure-JIT
// behaviour exactly -- see AppCore::startRadio/appendRadioBatch.
constexpr int kDefaultRadioExploration = 30;
constexpr int kDefaultRadioBatchSize = 15;
constexpr int kAdventurousExploration = 85;
// Keep at least this many un-played rows queued ahead of the current index
// while radio is batching, so the recommendation stream never visibly empties.
constexpr int kRadioTopUpThreshold = 5;
// A run of this many consecutive early radio skips means generation-time
// context (mood) has likely drifted; see AppCore::rerollRadioQueue.
constexpr int kRerollAfterConsecutiveSkips = 3;

QSet<QString> stringSetFromJson(const QJsonValue &value)
{
    QSet<QString> strings;
    const QJsonArray array = value.toArray();
    strings.reserve(array.size());
    for (const QJsonValue &item : array) {
        const QString text = item.toString();
        if (!text.isEmpty()) {
            strings.insert(text);
        }
    }
    return strings;
}

TrackScorer::Affinity affinityFromRow(const ListenHistoryStore::TrackAffinityRow &row)
{
    TrackScorer::Affinity affinity;
    affinity.playEvents = row.playEvents;
    affinity.finished = row.finished;
    affinity.skipped = row.skipped;
    affinity.lastPlayedAtSecs = row.lastPlayedAtSecs;
    affinity.listenCount = row.listenCount;
    affinity.baselineMax = row.baselineMax;
    return affinity;
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

QJsonArray reasonComponentsJson(const QList<TrackScorer::Component> &components)
{
    QJsonArray array;
    for (const TrackScorer::Component &component : components) {
        array.append(QJsonObject{
            {QStringLiteral("name"), component.name},
            {QStringLiteral("value"), component.value},
        });
    }
    return array;
}

} // namespace

AppCore::AppCore(QObject *parent)
    : QObject(parent)
{
    m_database = std::make_unique<Database>(QStringLiteral("main-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!m_database->open(databasePath())) {
        qWarning("AppCore: failed to open database: %s", qPrintable(m_database->lastError()));
    }
    m_radioBatchSize = std::clamp(
        m_database->setting(QStringLiteral("radio.batchSize"), QString::number(kDefaultRadioBatchSize)).toInt(),
        1, 100);

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

    m_playEventRecorder = new PlayEventRecorder(this);
    connect(m_playEventRecorder, &PlayEventRecorder::playEventReady, this,
            [this](ListenHistoryStore::PlayEvent event) {
                m_listenHistory->recordPlayEvent(event);
                handleRadioPlayEvent(event.source, event.outcome, event.playedMs, event.durationMs);
            });
    // Seed the recorder with the current shuffle mode and keep it in sync so the
    // value at each track start is stamped into that track's play event.
    m_playEventRecorder->setShuffleMode(shuffleModeToString(m_player->shuffleMode()));
    connect(m_player, &PlayerCore::shuffleModeChanged, this, [this](ShuffleMode mode) {
        m_playEventRecorder->setShuffleMode(shuffleModeToString(mode));
        syncRadioShuffleSession();
    });
    connect(m_player, &PlayerCore::currentIndexChanged, this, [this](int, bool userInitiated) {
        m_nextStartUserInitiated = userInitiated;
        maybeTopUpRadioQueue();
    });
    connect(m_player, &PlayerCore::aboutToInjectLibraryTrack, this, [this](const Track &) {
        m_nextStartInjected = true;
    });
    connect(m_player, &PlayerCore::trackFinished, m_playEventRecorder,
            &PlayEventRecorder::trackFinishedNaturally);
    connect(m_player, &PlayerCore::playbackCleared, m_playEventRecorder,
            &PlayEventRecorder::playbackCleared);
    connect(m_player, &PlayerCore::playbackCleared, this, [this]() {
        m_currentPlayingSource.clear();
    });
    connect(qApp, &QCoreApplication::aboutToQuit, m_playEventRecorder,
            &PlayEventRecorder::flushSessionEnd);
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        saveRadioSessionState();
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

    // LibraryIndex crosses the thread boundary via QueuedConnection.
    qRegisterMetaType<ScrobbleBackfill::LibraryIndex>();
    m_scrobbleBackfillThread = new QThread(this);
    m_scrobbleBackfill = new ScrobbleBackfill;
    m_scrobbleBackfill->moveToThread(m_scrobbleBackfillThread);
    connect(m_scrobbleBackfillThread, &QThread::finished, m_scrobbleBackfill, &QObject::deleteLater);
    connect(m_scrobbleBackfill, &ScrobbleBackfill::progress, this,
            [this](const QString &source, int processed, int inserted, qint64 reachedTs, qint64 total) {
                m_backfillStatus.service = source;
                m_backfillStatus.running = true;
                m_backfillStatus.processed = processed;
                m_backfillStatus.inserted = inserted;
                m_backfillStatus.reachedTs = reachedTs;
                m_backfillStatus.totalListens = total;
                // Per-run counters restart on every resume; the cumulative DB
                // count is what users track across interruptions.
                m_backfillStatus.storedTotal =
                    (m_listenHistory != nullptr && m_listenHistory->isOpen())
                        ? m_listenHistory->importedListenCount(source)
                        : 0;
                qInfo("scrobble-backfill[%s]: processed %d, stored %d this run (%d total)",
                      qPrintable(source), processed, inserted, m_backfillStatus.storedTotal);
                emit backfillStatusChanged();
            });
    connect(m_scrobbleBackfill, &ScrobbleBackfill::finished, this,
            [this](const QString &source, int processed, int inserted, qint64 total, const QString &message) {
                m_backfillRunning = false;
                m_backfillStatus.service = source;
                m_backfillStatus.running = false;
                m_backfillStatus.processed = processed;
                m_backfillStatus.inserted = inserted;
                m_backfillStatus.totalListens = total;
                m_backfillStatus.lastMessage = message;
                qInfo("scrobble-backfill[%s]: done — %s (processed %d, stored %d)",
                      qPrintable(source), qPrintable(message), processed, inserted);
                emit backfillStatusChanged();
            });
    connect(m_scrobbleBackfill, &ScrobbleBackfill::failed, this,
            [this](const QString &source, const QString &message) {
                m_backfillRunning = false;
                m_backfillStatus.service = source;
                m_backfillStatus.running = false;
                m_backfillStatus.lastMessage = message;
                qWarning("scrobble-backfill[%s]: failed — %s", qPrintable(source), qPrintable(message));
                emit backfillStatusChanged();
                // Transient service trouble (ListenBrainz's deep-history pages
                // are flaky) heals itself: try again later, exactly like the
                // startup auto-resume. Explicit cancel is respected because
                // maybeAutoResume re-checks the canceled flag when it fires.
                if (source == ListenHistoryStore::ListenBrainz
                    && message != QLatin1String("aborted")) {
                    qInfo("scrobble-backfill[listenbrainz]: will retry in 10 minutes");
                    QTimer::singleShot(10 * 60 * 1000, this,
                                       [this]() { maybeAutoResumeListenBrainzBackfill(); });
                }
            });
    m_scrobbleBackfillThread->start();

    m_ipc = new IpcServer(this);

    setupMprisWiring();
    setupScrobbleWiring();
    setupIpcHandler();
    setupTrayIcon();
    // Playback resume is deferred to the first showWindow(): the saved queue is
    // loaded by MainWindow's constructor (loadQueueState), so the player's queue
    // is empty here. restoreSavedPlayback() is guarded to run once per process.

    // Eagerly resume an interrupted ListenBrainz backfill, but not immediately:
    // give startup I/O (library/db opens, scan resume, tray/mpris wiring) a
    // window to settle first so the import doesn't compete with it for disk
    // and CPU right as the app comes up.
    QTimer::singleShot(20000, this, [this]() { maybeAutoResumeListenBrainzBackfill(); });
}

AppCore::~AppCore()
{
    saveRadioSessionState();
    // Finalize any in-flight play event before teardown, defensively: the
    // aboutToQuit signal may not fire on every exit path.
    if (m_playEventRecorder != nullptr) {
        m_playEventRecorder->flushSessionEnd();
    }
    if (m_listenBrainzThread != nullptr) {
        m_listenBrainzThread->quit();
        m_listenBrainzThread->wait(3000);
    }
    if (m_lastFmThread != nullptr) {
        m_lastFmThread->quit();
        m_lastFmThread->wait(3000);
    }
    if (m_scrobbleBackfillThread != nullptr) {
        m_scrobbleBackfillThread->quit();
        m_scrobbleBackfillThread->wait(3000);
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
PlayEventRecorder *AppCore::playEventRecorder() const { return m_playEventRecorder; }
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
        maybeRestoreRadioSession();
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
    // Native DSD may have switched the card's PipeWire profile off. Keep its
    // small UI controller alive while tray-hidden so its pause/end timers can
    // return the card; destroying it would either leak the takeover or interrupt
    // active headless playback.
    if (m_window->hasTakenOverDsdDevice()) {
        m_window->hide();
        if (m_tray && !m_trayAlwaysVisible) {
            m_tray->show();
        }
        return;
    }
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
        MainWindow *window = m_window;
        window->close();
        delete window;
        m_window = nullptr;
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
    connect(m_player, &PlayerCore::radioShufflePercentChanged, this, [this](int percent) {
        m_state->setSetting(QStringLiteral("playback.radioShufflePercent"), QString::number(percent));
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
        m_playEventRecorder->playbackStateChanged(playing);
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
        // Consume the attribution set by the preceding currentIndexChanged /
        // aboutToInjectLibraryTrack regardless of notifyScrobbler, so a silent
        // present/restore does not leave stale flags for the next real start.
        const bool userInitiated = m_nextStartUserInitiated;
        const bool injected = m_nextStartInjected;
        m_nextStartInjected = false;
        // A silent present/restore (notifyScrobbler == false) must not open a
        // play event; only real track starts do.
        if (!notifyScrobbler) {
            return;
        }
        notifyScrobblersTrackStarted(track);
        // A radio pick's path is tracked in m_radioPickPaths regardless of how it
        // entered the queue: a JIT pick's start rides aboutToInjectLibraryTrack
        // (injected == true) but a batch-appended pick's start is a PLAIN
        // "advance to the next queued row" (the track was already in the queue),
        // so `injected` alone cannot distinguish "radio" from "queue_auto" here.
        const QString source = m_radioPickPaths.contains(track.path)
            ? QStringLiteral("radio")
            : (injected
                ? QStringLiteral("library_shuffle")
                : (userInitiated ? QStringLiteral("queue_manual") : QStringLiteral("queue_auto")));
        m_currentPlayingSource = source;
        m_playEventRecorder->trackStarted(track, userInitiated, source);
        // Advance the radio rolling context with every real track start while a
        // session is active — the seed, radio picks, and user-queued
        // interruptions alike (they all shape mood continuity).
        if (m_radioSession && (m_player->radioActive() || m_radioShuffleSessionActive)) {
            m_radioSession->notePlayed(track);
        }
        // A non-radio track breaks the consecutive-early-skip streak the moment
        // it starts (a user-queued interruption, not a skip outcome, is still a
        // mood break); handleRadioPlayEvent maintains the streak for radio spins.
        if (source != QStringLiteral("radio")) {
            m_radioConsecutiveEarlySkips = 0;
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

bool AppCore::recordRatingEvent(const Track &track,
                                bool hadOldUserRating,
                                int oldUserRating0To100,
                                int oldEffectiveRating0To100,
                                int newRating0To100,
                                const QString &sourceSurface)
{
    if (m_listenHistory == nullptr) {
        return false;
    }

    ListenHistoryStore::RatingEvent event;
    event.occurredAtSecs = QDateTime::currentSecsSinceEpoch();
    event.track = track;
    event.hasOldUserRating = hadOldUserRating;
    event.oldUserRating0To100 = oldUserRating0To100;
    event.oldEffectiveRating0To100 = oldEffectiveRating0To100;
    event.newRating0To100 = newRating0To100;
    event.sourceSurface = sourceSurface;
    if (m_player != nullptr) {
        event.radioActive = m_player->radioActive();
        const bool activeSource = m_playback != nullptr
            && m_playback->hasSource()
            && m_playback->state() != PlaybackBackend::State::Stopped
            && m_playback->state() != PlaybackBackend::State::Error;
        if (activeSource) {
            const Track playing = m_player->currentTrack();
            event.playingTrackPath = playing.path;
            event.playingSource = playing.path.isEmpty() ? QString() : m_currentPlayingSource;
        }
    }
    return m_listenHistory->recordRatingEvent(event);
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
    m_playEventRecorder->resumeTrack(track, elapsedMs, playing, QStringLiteral("resume"));
    QMetaObject::invokeMethod(m_listenBrainzScrobbler, "resumeTrack", Qt::QueuedConnection,
                              Q_ARG(Track, track), Q_ARG(qint64, elapsedMs), Q_ARG(bool, playing));
    QMetaObject::invokeMethod(m_lastFmScrobbler, "resumeTrack", Qt::QueuedConnection,
                              Q_ARG(Track, track), Q_ARG(qint64, elapsedMs), Q_ARG(bool, playing));
}

QJsonObject AppCore::ipcStatus() const
{
    QJsonObject status = QJsonDocument::fromJson(m_mpris->currentTrackJson().toUtf8()).object();
    status.insert(QStringLiteral("radio"),
                  (m_player != nullptr && m_player->radioActive()) ? QStringLiteral("active")
                                                                   : QStringLiteral("inactive"));
    return status;
}

QStringList AppCore::radioFoldedGenresForTrack(const QString &path,
                                               const QHash<QString, QString> &genreAliases,
                                               const QSet<QString> &ignoredRadioGenres) const
{
    QStringList genresFolded;
    if (m_database == nullptr) {
        return genresFolded;
    }
    for (const QString &genre : m_database->genresForTrack(path)) {
        genresFolded.push_back(GenreTags::folded(genre));
    }
    return canonicalRadioGenres(genresFolded, genreAliases, ignoredRadioGenres);
}

QStringList AppCore::pathsForSongKeyOfTrack(const QString &trackPath) const
{
    if (m_database == nullptr || trackPath.isEmpty()) {
        return {};
    }

    const auto rows = m_database->trackMatchRows();
    QString targetSongKey;
    QList<std::tuple<QString, QString, QString, QString>> cachedRows;
    cachedRows.reserve(rows.size());
    for (const auto &[path, artist, title, recordingMbid] : rows) {
        cachedRows.push_back({path, artist, title, recordingMbid});
        if (path == trackPath) {
            targetSongKey = FoldKey::songKey(recordingMbid, artist, title);
        }
    }
    if (targetSongKey.isEmpty()) {
        return {trackPath};
    }

    QStringList paths;
    for (const auto &[path, artist, title, recordingMbid] : cachedRows) {
        if (FoldKey::songKey(recordingMbid, artist, title) == targetSongKey) {
            paths.push_back(path);
        }
    }
    if (paths.isEmpty()) {
        paths.push_back(trackPath);
    }
    paths.removeDuplicates();
    return paths;
}

TrackScorer::Candidate AppCore::buildRadioSeedCandidate(const Track &seed,
                                                        const QStringList &seedGenresFolded) const
{
    TrackScorer::Candidate seedCandidate;
    seedCandidate.path = seed.path;
    seedCandidate.songKey = FoldKey::songKey(seed.musicBrainz.recordingId, seed.artistName, seed.title);
    seedCandidate.artistFolded = FoldKey::fold(seed.artistName);
    seedCandidate.albumKey = FoldKey::albumKey(seed.albumArtistName, seed.albumTitle);
    seedCandidate.genresFolded = seedGenresFolded;
    seedCandidate.year = trackYear(seed);
    seedCandidate.effectiveRating0To100 = seed.effectiveRating0To100;
    seedCandidate.hasUserRating = seed.hasUserRating;
    return seedCandidate;
}

QVector<TrackScorer::Candidate> AppCore::buildRadioCandidatePool(const QStringList &informativeGenres,
                                                                 const QHash<QString, QString> &genreAliases,
                                                                 const QSet<QString> &ignoredRadioGenres) const
{
    if (m_database == nullptr) {
        return {};
    }

    QVector<RadioCandidateRow> rows;
    if (informativeGenres.isEmpty()) {
        // No informative genre to match on (no genres at all, only junk
        // placeholders, or anchorless Radio shuffle): fall back to a random
        // slice of the whole library.
        rows = m_database->radioFallbackCandidates(2000);
    } else {
        // Genre-matched candidates, plus a random slice of the library blended
        // in unconditionally: no session may be trapped inside one genre
        // cohort. The random slice gives novelty and the rolling context an
        // escape route; IDF-weighted scoring (TrackScorer) keeps it honest by
        // still favoring genuine genre matches over the noise.
        rows = m_database->radioCandidates(informativeGenres, 2000);
        const QVector<RadioCandidateRow> randomSlice = m_database->radioFallbackCandidates(500);
        QSet<QString> seenPaths;
        seenPaths.reserve(rows.size());
        for (const RadioCandidateRow &row : rows) {
            seenPaths.insert(row.path);
        }
        for (const RadioCandidateRow &row : randomSlice) {
            if (!seenPaths.contains(row.path)) {
                seenPaths.insert(row.path);
                rows.push_back(row);
            }
        }
    }

    QVector<TrackScorer::Candidate> pool;
    pool.reserve(rows.size());
    for (const RadioCandidateRow &row : rows) {
        pool.push_back(candidateFromRow(row, genreAliases, ignoredRadioGenres));
    }
    // User taste flags are path-scoped storage but AppCore applies them at the
    // recommender boundary, after every SQL candidate source has been merged.
    return RadioFilters::excludeFlaggedCandidates(
        pool, m_database->flaggedPaths(Database::TrackFlag::NeverRadio));
}

QVector<TrackScorer::Candidate> AppCore::buildRadioFallbackPool(int limit,
                                                                const QHash<QString, QString> &genreAliases,
                                                                const QSet<QString> &ignoredRadioGenres) const
{
    QVector<TrackScorer::Candidate> pool;
    if (m_database == nullptr) {
        return pool;
    }

    const QVector<RadioCandidateRow> rows = m_database->radioFallbackCandidates(limit);
    pool.reserve(rows.size());
    for (const RadioCandidateRow &row : rows) {
        pool.push_back(candidateFromRow(row, genreAliases, ignoredRadioGenres));
    }
    return RadioFilters::excludeFlaggedCandidates(
        pool, m_database->flaggedPaths(Database::TrackFlag::NeverRadio));
}

QHash<QString, double> AppCore::buildRadioGenreIdf(const QHash<QString, QString> &genreAliases,
                                                   const QSet<QString> &ignoredRadioGenres) const
{
    QHash<QString, double> genreIdf;
    if (m_database == nullptr) {
        return genreIdf;
    }

    // Library-wide genre IDF map: broad/junk genres self-discount, rare genres
    // carry real weight. Built over the FULL genre vocabulary (not just the
    // seed's genres) because the rolling context drifts to played tracks'
    // genres as the session goes on, and those need lookups too.
    int taggedTrackTotal = 0;
    const QHash<QString, int> genreDf = m_database->genreTrackCounts(&taggedTrackTotal);
    QHash<QString, int> canonicalDf;
    for (auto it = genreDf.cbegin(); it != genreDf.cend(); ++it) {
        const QString canonical = GenreTags::canonical(it.key(), genreAliases);
        if (canonical.isEmpty() || GenreTags::isNonGenre(canonical) || ignoredRadioGenres.contains(canonical)) {
            continue;
        }
        canonicalDf[canonical] += it.value();
    }
    genreIdf.reserve(canonicalDf.size());
    for (auto it = canonicalDf.cbegin(); it != canonicalDf.cend(); ++it) {
        const int df = std::max(1, it.value());
        genreIdf.insert(it.key(), std::log(std::max(2.0, static_cast<double>(taggedTrackTotal) / df)));
    }
    return genreIdf;
}

TrackScorer::Weights AppCore::radioScoringWeights() const
{
    if (m_database == nullptr) {
        return TrackScorer::defaultWeights();
    }

    QString error;
    const TrackScorer::Weights weights =
        TrackScorer::weightsFromJson(m_database->setting(QStringLiteral("radio.scoringWeights")).toUtf8(), &error);
    if (!error.isEmpty()) {
        qWarning("radio-scoring: ignoring invalid radio.scoringWeights: %s", qPrintable(error));
    }
    return weights;
}

QHash<QString, TrackScorer::Affinity> AppCore::buildRadioAffinities() const
{
    QHash<QString, TrackScorer::Affinity> affinities;
    if (m_database == nullptr || m_listenHistory == nullptr) {
        return affinities;
    }

    const QSet<QString> noLearnPaths = m_database->flaggedPaths(Database::TrackFlag::NoLearn);
    const QHash<QString, ListenHistoryStore::TrackAffinityRow> affinityRows = m_listenHistory->trackAffinities();
    affinities.reserve(affinityRows.size());
    for (auto it = affinityRows.cbegin(); it != affinityRows.cend(); ++it) {
        if (noLearnPaths.contains(it.key())) {
            continue;
        }
        affinities.insert(it.key(), affinityFromRow(it.value()));
    }

    QHash<QString, QString> pathToSongKey;
    const auto matchRows = m_database->trackMatchRows();
    pathToSongKey.reserve(matchRows.size());
    for (const auto &[path, artist, title, recordingMbid] : matchRows) {
        pathToSongKey.insert(path, FoldKey::songKey(recordingMbid, artist, title));
    }
    pathToSongKey = RadioFilters::excludeFlaggedPathMappings(pathToSongKey, noLearnPaths);
    return RadioFilters::excludeFlaggedAffinities(
        AffinityPool::poolBySongKey(affinities, pathToSongKey), noLearnPaths);
}

void AppCore::saveRadioSessionState()
{
    if (m_state == nullptr || m_player == nullptr || !m_player->radioActive() || !m_radioSession) {
        return;
    }

    QJsonObject root = m_radioSession->constraintState();
    root.insert(QStringLiteral("active"), true);
    root.insert(QStringLiteral("kind"), m_radioSessionKind.isEmpty() ? QStringLiteral("anchorless")
                                                                     : m_radioSessionKind);
    root.insert(QStringLiteral("seedPath"), m_radioSessionSeedPath);
    root.insert(QStringLiteral("exploration"), m_radioSessionExploration);
    root.insert(QStringLiteral("consecutiveEarlySkips"), m_radioConsecutiveEarlySkips);
    m_state->setSetting(QStringLiteral("radio.session.state"),
                        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

void AppCore::clearRadioSessionState()
{
    if (m_state != nullptr) {
        m_state->removeSetting(QStringLiteral("radio.session.state"));
    }
}

void AppCore::maybeRestoreRadioSession()
{
    if (m_radioRestoreDone) {
        return;
    }
    m_radioRestoreDone = true;
    if (m_state == nullptr || m_database == nullptr || m_player == nullptr) {
        return;
    }

    const QString rawState = m_state->setting(QStringLiteral("radio.session.state"));
    if (rawState.isEmpty()) {
        return;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(rawState.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        qInfo("radio-restore: clearing malformed session state JSON");
        clearRadioSessionState();
        return;
    }
    const QJsonObject root = document.object();
    if (!root.value(QStringLiteral("active")).toBool(false)) {
        clearRadioSessionState();
        return;
    }

    const QString kind = root.value(QStringLiteral("kind")).toString();
    const int exploration = std::clamp(root.value(QStringLiteral("exploration")).toInt(kDefaultRadioExploration),
                                       0, 100);
    const qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
    const QHash<QString, QString> genreAliases = m_database->genreAliases();
    const QSet<QString> ignoredRadioGenres = m_database->ignoredRadioGenres();
    const TrackScorer::Weights scoringWeights = radioScoringWeights();
    std::unique_ptr<RadioSession> restored;
    QString seedPath;

    if (kind == QLatin1String("seeded")) {
        seedPath = root.value(QStringLiteral("seedPath")).toString();
        const Track seed = m_database->trackForPath(seedPath);
        if (seed.path.isEmpty()) {
            qInfo("radio-restore: clearing stale seeded session; seed is no longer in the library");
            clearRadioSessionState();
            return;
        }
        const QStringList seedGenresFolded = radioFoldedGenresForTrack(seed.path, genreAliases, ignoredRadioGenres);
        QVector<TrackScorer::Candidate> pool =
            buildRadioCandidatePool(GenreTags::informative(seedGenresFolded), genreAliases, ignoredRadioGenres);
        restored = std::make_unique<RadioSession>(std::move(pool), buildRadioAffinities(),
                                                  buildRadioGenreIdf(genreAliases, ignoredRadioGenres),
                                                  buildRadioSeedCandidate(seed, seedGenresFolded),
                                                  exploration, nowSecs, nullptr, scoringWeights);
    } else if (kind == QLatin1String("anchorless")) {
        QVector<TrackScorer::Candidate> pool = buildRadioCandidatePool({}, genreAliases, ignoredRadioGenres);
        if (pool.isEmpty()) {
            qInfo("radio-restore: clearing stale anchorless session; candidate pool is empty");
            clearRadioSessionState();
            return;
        }
        restored = std::make_unique<RadioSession>(std::move(pool), buildRadioAffinities(),
                                                  buildRadioGenreIdf(genreAliases, ignoredRadioGenres), exploration, nowSecs,
                                                  nullptr, scoringWeights);
    } else if (const std::optional<RadioMix::Mode> mixMode = RadioMix::modeFromString(kind)) {
        QVector<TrackScorer::Candidate> pool = buildRadioFallbackPool(5000, genreAliases, ignoredRadioGenres);
        QHash<QString, TrackScorer::Affinity> affinities = buildRadioAffinities();
        pool = RadioMix::filterCandidates(*mixMode, pool, affinities, nowSecs);
        if (pool.isEmpty()) {
            qInfo("radio-restore: clearing stale mix session; filtered pool is empty");
            clearRadioSessionState();
            return;
        }
        restored = std::make_unique<RadioSession>(std::move(pool), std::move(affinities),
                                                  buildRadioGenreIdf(genreAliases, ignoredRadioGenres), exploration, nowSecs,
                                                  nullptr, scoringWeights);
    } else {
        qInfo("radio-restore: clearing malformed session state with unknown kind");
        clearRadioSessionState();
        return;
    }

    restored->restoreConstraintState(root);
    m_radioSession = std::move(restored);
    m_radioSessionKind = kind;
    m_radioSessionSeedPath = seedPath;
    m_radioSessionExploration = exploration;
    m_radioConsecutiveEarlySkips = std::max(0, root.value(QStringLiteral("consecutiveEarlySkips")).toInt(0));
    m_radioAdventurous = false;
    m_radioShuffleSessionActive = false;
    m_radioPickPaths.clear();
    const QSet<QString> usedPaths = stringSetFromJson(root.value(QStringLiteral("usedPaths")));
    for (const Track &track : m_player->queue()) {
        if (track.path != seedPath && usedPaths.contains(track.path)) {
            m_radioPickPaths.insert(track.path);
        }
    }

    installRadioProvider(/*markPicksAsRadio=*/true);
    m_player->setRadioActive(true);
}

void AppCore::installRadioProvider(bool markPicksAsRadio)
{
    if (m_player == nullptr) {
        return;
    }
    m_player->setRadioProvider([this, markPicksAsRadio](int count, const QSet<QString> &excludePaths) -> QVector<Track> {
        if (!m_radioSession) {
            return {};
        }
        const QVector<Track> picks = m_radioSession->nextTracks(count, excludePaths, [this](const QString &path) {
            return m_database ? m_database->trackForPath(path) : Track{};
        });
        if (markPicksAsRadio) {
            for (const Track &track : picks) {
                if (!track.path.isEmpty()) {
                    m_radioPickPaths.insert(track.path);
                }
            }
        }
        return picks;
    });
}

void AppCore::syncRadioShuffleSession()
{
    if (m_player == nullptr || m_database == nullptr) {
        return;
    }
    if (m_player->shuffleMode() != ShuffleMode::Radio || m_player->radioActive()) {
        if (m_radioShuffleSessionActive) {
            m_radioShuffleSessionActive = false;
            m_radioSession.reset();
            m_radioSessionKind.clear();
            m_radioSessionSeedPath.clear();
            m_player->setRadioProvider({});
        }
        return;
    }

    const QHash<QString, QString> genreAliases = m_database->genreAliases();
    const QSet<QString> ignoredRadioGenres = m_database->ignoredRadioGenres();
    QVector<TrackScorer::Candidate> pool = buildRadioCandidatePool({}, genreAliases, ignoredRadioGenres);
    if (pool.isEmpty()) {
        m_radioShuffleSessionActive = false;
        m_radioSession.reset();
        m_radioSessionKind.clear();
        m_radioSessionSeedPath.clear();
        m_player->setRadioProvider({});
        return;
    }

    m_radioSession = std::make_unique<RadioSession>(std::move(pool), buildRadioAffinities(),
                                                    buildRadioGenreIdf(genreAliases, ignoredRadioGenres),
                                                    radioExploration(), QDateTime::currentSecsSinceEpoch(),
                                                    nullptr, radioScoringWeights());
    m_radioSessionKind = QStringLiteral("anchorless");
    m_radioSessionSeedPath.clear();
    m_radioSessionExploration = radioExploration();
    m_radioShuffleSessionActive = true;
    installRadioProvider(/*markPicksAsRadio=*/false);
    const Track current = m_player->currentTrack();
    if (!current.path.isEmpty()) {
        m_radioSession->notePlayed(current);
    }
}

bool AppCore::startRadio(const QString &seedPath)
{
    if (m_database == nullptr || m_player == nullptr) {
        return false;
    }
    const Track seed = m_database->trackForPath(seedPath);
    if (seed.path.isEmpty()) {
        return false;
    }

    const QHash<QString, QString> genreAliases = m_database->genreAliases();
    const QSet<QString> ignoredRadioGenres = m_database->ignoredRadioGenres();
    // Seed genres drive candidate generation and anchor the mood window. Genre
    // aliases are canonicalized here; raw track_genres rows stay unchanged.
    const QStringList seedGenresFolded = radioFoldedGenresForTrack(seed.path, genreAliases, ignoredRadioGenres);
    // Tagger placeholders ("Other", "Unknown", ...) are not real genres: a seed
    // whose only tag is one of these must not have the whole junk cohort
    // become its candidate pool (see plans/music-recommendation-plan.md, Trial
    // findings — first radio session).
    const QStringList informativeGenres = GenreTags::informative(seedGenresFolded);

    QVector<TrackScorer::Candidate> pool = buildRadioCandidatePool(informativeGenres, genreAliases, ignoredRadioGenres);
    QHash<QString, double> genreIdf = buildRadioGenreIdf(genreAliases, ignoredRadioGenres);
    QHash<QString, TrackScorer::Affinity> affinities = buildRadioAffinities();

    TrackScorer::Candidate seedCandidate = buildRadioSeedCandidate(seed, seedGenresFolded);

    // Dedicated exploration knob so radio never touches the shuffle settings.
    // An armed "adventurous" boost wins over the persisted value for this
    // session's start (one-shot: consumed below regardless of which branch fires).
    const int persistedExploration = std::clamp(
        m_database->setting(QStringLiteral("radio.exploration"), QString::number(kDefaultRadioExploration)).toInt(),
        0, 100);
    const int exploration = m_radioAdventurous ? kAdventurousExploration : persistedExploration;
    m_radioAdventurous = false;

    m_radioBatchSize = std::clamp(
        m_database->setting(QStringLiteral("radio.batchSize"), QString::number(kDefaultRadioBatchSize)).toInt(),
        1, 100);
    m_radioPickPaths.clear();
    m_radioConsecutiveEarlySkips = 0;
    m_radioShuffleSessionActive = false;
    m_radioSessionKind = QStringLiteral("seeded");
    m_radioSessionSeedPath = seed.path;
    m_radioSessionExploration = exploration;

    m_radioSession = std::make_unique<RadioSession>(std::move(pool), std::move(affinities), std::move(genreIdf),
                                                    seedCandidate, exploration, QDateTime::currentSecsSinceEpoch(),
                                                    nullptr, radioScoringWeights());
    // Install the scored provider; it resolves each pick to a full Track by
    // path. Stays installed regardless of batch size: it is the safety net for
    // when the queue runs dry (e.g. the user deleted rows ahead of playback).
    installRadioProvider(/*markPicksAsRadio=*/true);
    m_player->setRadioActive(true);
    // Guard the setup sequence against maybeTopUpRadioQueue: appendAndPlay(seed)
    // below fires currentIndexChanged synchronously with a 1-row queue (well
    // under the top-up threshold), which would otherwise race ahead of the
    // deliberate initial batch a few lines down and double-append.
    m_radioTopUpInProgress = true;
    // Clear the queue and start the seed; the currentTrackChanged handler feeds
    // the seed into the session's rolling context, and auto-advance past the seed
    // pulls the first recommendation through the radio provider.
    m_player->clearAll();
    m_player->appendAndPlay(seed);
    // batchSize == 1 reproduces the original just-in-time behaviour exactly: no
    // batch append here, and maybeTopUpRadioQueue()/rerollRadioQueue() both
    // no-op in that mode too -- the JIT provider above is the only source of
    // picks, generated exactly when decideAutoNext() needs one.
    if (m_radioBatchSize > 1) {
        appendRadioBatch(m_radioBatchSize - 1);
    }
    m_radioTopUpInProgress = false;
    saveRadioSessionState();
    return true;
}

bool AppCore::startMix(const QString &mode)
{
    const std::optional<RadioMix::Mode> mixMode = RadioMix::modeFromString(mode);
    if (!mixMode || m_database == nullptr || m_player == nullptr) {
        return false;
    }

    const QHash<QString, QString> genreAliases = m_database->genreAliases();
    const QSet<QString> ignoredRadioGenres = m_database->ignoredRadioGenres();
    QVector<TrackScorer::Candidate> pool = buildRadioFallbackPool(5000, genreAliases, ignoredRadioGenres);
    QHash<QString, TrackScorer::Affinity> affinities = buildRadioAffinities();
    const qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
    pool = RadioMix::filterCandidates(*mixMode, pool, affinities, nowSecs);
    if (pool.isEmpty()) {
        return false;
    }

    const int persistedExploration = std::clamp(
        m_database->setting(QStringLiteral("radio.exploration"), QString::number(kDefaultRadioExploration)).toInt(),
        0, 100);
    const int exploration = m_radioAdventurous ? kAdventurousExploration : persistedExploration;

    m_radioBatchSize = std::clamp(
        m_database->setting(QStringLiteral("radio.batchSize"), QString::number(kDefaultRadioBatchSize)).toInt(),
        1, 100);

    auto session = std::make_unique<RadioSession>(std::move(pool), affinities,
                                                  buildRadioGenreIdf(genreAliases, ignoredRadioGenres),
                                                  exploration, nowSecs, nullptr, radioScoringWeights());
    QVector<Track> picks = session->nextTracks(m_radioBatchSize, {}, [this](const QString &path) {
        return m_database ? m_database->trackForPath(path) : Track{};
    });
    if (picks.isEmpty()) {
        return false;
    }

    m_radioAdventurous = false;
    m_radioPickPaths.clear();
    m_radioConsecutiveEarlySkips = 0;
    m_radioShuffleSessionActive = false;
    m_radioSessionKind = mode.trimmed().toLower();
    m_radioSessionSeedPath.clear();
    m_radioSessionExploration = exploration;
    m_radioSession = std::move(session);
    for (const Track &track : picks) {
        if (!track.path.isEmpty()) {
            m_radioPickPaths.insert(track.path);
        }
    }

    installRadioProvider(/*markPicksAsRadio=*/true);
    m_player->setRadioActive(true);
    m_radioTopUpInProgress = true;
    m_player->clearAll();
    m_player->injectTracks(picks);
    m_player->playAt(0, true, false, /*explicitJump=*/true);
    m_radioTopUpInProgress = false;
    saveRadioSessionState();
    return true;
}

void AppCore::stopRadio()
{
    if (m_player != nullptr) {
        m_player->setRadioActive(false);
        m_player->setRadioProvider({});
    }
    m_radioSession.reset();
    m_radioPickPaths.clear();
    m_radioConsecutiveEarlySkips = 0;
    m_radioSessionKind.clear();
    m_radioSessionSeedPath.clear();
    // "Resets when a session ends" -- see setRadioAdventurous's doc comment.
    m_radioAdventurous = false;
    clearRadioSessionState();
    syncRadioShuffleSession();
}

QString AppCore::radioPickReason(const QString &path) const
{
    if (!m_radioSession) {
        return {};
    }

    const QList<TrackScorer::Component> components = m_radioSession->reasonComponentsFor(path);
    if (components.isEmpty()) {
        return {};
    }

    const QString sentence = ReasonText::sentence(components);
    const QString breakdown = ReasonText::breakdown(components);
    if (sentence.isEmpty()) {
        return breakdown;
    }
    if (breakdown.isEmpty()) {
        return sentence;
    }
    return sentence + QLatin1Char('\n') + breakdown;
}

bool AppCore::trackFlag(const QString &trackPath, const QString &flag) const
{
    if (m_database == nullptr) {
        return false;
    }
    const std::optional<Database::TrackFlag> parsed = trackFlagFromName(flag);
    if (!parsed.has_value()) {
        return false;
    }
    return m_database->trackFlag(trackPath, parsed.value());
}

bool AppCore::setTrackFlagForSong(const QString &trackPath, const QString &flag, bool on)
{
    if (m_database == nullptr) {
        return false;
    }
    const std::optional<Database::TrackFlag> parsed = trackFlagFromName(flag);
    if (!parsed.has_value()) {
        return false;
    }

    const QStringList paths = pathsForSongKeyOfTrack(trackPath);
    if (paths.isEmpty()) {
        return false;
    }
    for (const QString &path : paths) {
        if (!m_database->setTrackFlag(path, parsed.value(), on)) {
            return false;
        }
    }
    return true;
}

int AppCore::forgetTrackBehaviorForSong(const QString &trackPath, bool includeImportedListens)
{
    if (m_listenHistory == nullptr) {
        return 0;
    }
    const QStringList paths = pathsForSongKeyOfTrack(trackPath);
    if (paths.isEmpty()) {
        return 0;
    }
    return m_listenHistory->forgetTrackBehavior(paths, includeImportedListens);
}

int AppCore::radioExploration() const
{
    return m_database
        ? std::clamp(m_database->setting(QStringLiteral("radio.exploration"),
                                         QString::number(kDefaultRadioExploration)).toInt(), 0, 100)
        : kDefaultRadioExploration;
}

void AppCore::setRadioExploration(int value0To100, bool persist)
{
    const int clamped = std::clamp(value0To100, 0, 100);
    if (m_radioSession) {
        m_radioSession->setExploration(clamped);
        m_radioSessionExploration = clamped;
        saveRadioSessionState();
    }
    if (persist && m_database != nullptr) {
        m_database->setSetting(QStringLiteral("radio.exploration"), QString::number(clamped));
    }
}

int AppCore::radioBatchSize() const
{
    return m_radioBatchSize;
}

void AppCore::setRadioBatchSize(int value1To100)
{
    m_radioBatchSize = std::clamp(value1To100, 1, 100);
    if (m_database != nullptr) {
        m_database->setSetting(QStringLiteral("radio.batchSize"), QString::number(m_radioBatchSize));
    }
}

bool AppCore::radioAdventurous() const
{
    return m_radioAdventurous;
}

void AppCore::setRadioAdventurous(bool on)
{
    m_radioAdventurous = on;
    if (m_radioSession) {
        // Live session: reflect the toggle immediately. Turning it off falls
        // back to the persisted setting, not whatever the session started at.
        m_radioSessionExploration = on ? kAdventurousExploration : radioExploration();
        m_radioSession->setExploration(m_radioSessionExploration);
        saveRadioSessionState();
    }
    // No session yet: just arms the next startRadio (consumed + reset there).
}

void AppCore::appendRadioBatch(int count)
{
    if (!m_radioSession || m_player == nullptr || count <= 0) {
        return;
    }
    QSet<QString> exclude;
    const QVector<Track> &queue = m_player->queue();
    exclude.reserve(queue.size());
    for (const Track &track : queue) {
        exclude.insert(track.path);
    }
    const QVector<Track> picks = m_radioSession->nextTracks(count, exclude, [this](const QString &path) {
        return m_database ? m_database->trackForPath(path) : Track{};
    });
    if (picks.isEmpty()) {
        return;
    }
    for (const Track &track : picks) {
        if (!track.path.isEmpty()) {
            m_radioPickPaths.insert(track.path);
        }
    }
    // Radio picks are queue-only: PlayerCore::injectTracks reuses the single-JIT
    // aboutToInjectLibraryTrack semantics per track. Plain appendTracks (which
    // emits aboutToAddTracks) is NOT safe here -- MainWindow's
    // prepareQueueForTrackAddition mirrors aboutToAddTracks tracks into the
    // active playlist whenever the queue is still playlist-sourced, and
    // AppCore::startRadio's clearAll()+appendAndPlay(seed) does not itself
    // reset that source-kind (that bookkeeping lives in MainWindow, not
    // PlayerCore) -- a radio session started while a playlist-backed queue was
    // playing would otherwise silently mirror every batch pick into it.
    m_player->injectTracks(picks);
    saveRadioSessionState();
}

void AppCore::maybeTopUpRadioQueue()
{
    if (m_radioTopUpInProgress || !m_radioSession || m_player == nullptr || !m_player->radioActive()) {
        return;
    }
    if (m_radioBatchSize <= 1) {
        return; // pure JIT mode: no queued-ahead rows to keep topped up
    }
    const int remaining = static_cast<int>(m_player->queue().size()) - 1 - m_player->queueIndex();
    if (remaining >= kRadioTopUpThreshold) {
        return;
    }
    m_radioTopUpInProgress = true;
    appendRadioBatch(m_radioBatchSize);
    m_radioTopUpInProgress = false;
    saveRadioSessionState();
}

void AppCore::rerollRadioQueue()
{
    if (!m_radioSession || m_player == nullptr || m_radioBatchSize <= 1) {
        return; // pure JIT mode has no queued-ahead radio rows to discard
    }
    const QVector<Track> &queue = m_player->queue();
    const int currentIndex = m_player->queueIndex();
    QVector<int> staleRows;
    QStringList stalePaths;
    for (int row = currentIndex + 1; row < queue.size(); ++row) {
        if (m_radioPickPaths.contains(queue.at(row).path)) {
            staleRows.push_back(row);
            stalePaths.push_back(queue.at(row).path);
        }
    }
    if (staleRows.isEmpty()) {
        return;
    }
    for (const QString &path : stalePaths) {
        m_radioPickPaths.remove(path);
    }
    m_player->removeRows(staleRows);
    appendRadioBatch(m_radioBatchSize);
    saveRadioSessionState();
}

void AppCore::handleRadioPlayEvent(const QString &source, const QString &outcome, qint64 playedMs,
                                   qint64 durationMs)
{
    if (source != QStringLiteral("radio")) {
        // Non-radio starts already reset the streak in setupScrobbleWiring's
        // currentTrackChanged handler; nothing else to do for their play events.
        return;
    }
    if (outcome != QStringLiteral("skipped")) {
        // Any non-skip outcome (finished, stopped, session_end) means the pick
        // landed fine -- the early-skip streak is broken.
        m_radioConsecutiveEarlySkips = 0;
        return;
    }
    if (!RadioSession::isEarlySkip(playedMs, durationMs)) {
        // A late skip: the user heard most of it before moving on. Per spec
        // this is neither a rejection signal nor an explicit reset -- leave the
        // streak counter untouched.
        return;
    }
    if (++m_radioConsecutiveEarlySkips >= kRerollAfterConsecutiveSkips) {
        m_radioConsecutiveEarlySkips = 0;
        rerollRadioQueue();
    }
}

ScrobbleBackfill::LibraryIndex AppCore::buildLibraryIndex() const
{
    ScrobbleBackfill::LibraryIndex index;
    if (m_database == nullptr) {
        return index;
    }
    const auto rows = m_database->trackMatchRows();
    index.byRecordingMbid.reserve(rows.size());
    index.byArtistTitle.reserve(rows.size());
    for (const auto &[path, artist, title, recordingMbid] : rows) {
        if (!recordingMbid.isEmpty()) {
            index.byRecordingMbid.insert(recordingMbid, path);
        }
        index.byArtistTitle.insert(ScrobbleBackfill::foldedArtistTitleKey(artist, title), path);
    }
    return index;
}

QString AppCore::startBackfill(const QString &service)
{
    const QString normalized = service.trimmed().toLower();
    if (normalized != QLatin1String("listenbrainz") && normalized != QLatin1String("lastfm")) {
        return QStringLiteral("unknown-service");
    }
    if (m_backfillRunning) {
        return QStringLiteral("already-running");
    }

    const ScrobbleBackfill::LibraryIndex index = buildLibraryIndex();
    const QString historyPath = listenHistoryPath();
    if (normalized == QLatin1String("listenbrainz")) {
        const QString token = m_database->setting(QStringLiteral("listenbrainz.token")).trimmed();
        if (token.isEmpty()) {
            return QStringLiteral("missing-credentials");
        }
        // A fresh, explicit start (manual or auto-resumed) means any earlier
        // cancel no longer applies — clear it so a later interruption can
        // auto-resume again. Auto-resume itself only gets here when the flag
        // was already clear, so this is a no-op on that path.
        if (m_listenHistory != nullptr && m_listenHistory->isOpen()) {
            m_listenHistory->setMetaValue(ScrobbleBackfill::CanceledMetaKey, QString());
        }
        m_backfillRunning = true;
        m_backfillStatus = BackfillStatus{};
        m_backfillStatus.service = ListenHistoryStore::ListenBrainz;
        m_backfillStatus.running = true;
        emit backfillStatusChanged();
        QMetaObject::invokeMethod(m_scrobbleBackfill, "startListenBrainzImport", Qt::QueuedConnection,
                                  Q_ARG(QString, token), Q_ARG(QString, historyPath),
                                  Q_ARG(ScrobbleBackfill::LibraryIndex, index));
    } else {
        const QString username = m_database->setting(QStringLiteral("lastfm.username")).trimmed();
        // API key fallback chain, mirroring MainWindow::lastFmApiKey().
        QString apiKey = m_database->setting(QStringLiteral("lastfm.apiKey")).trimmed();
        if (apiKey.isEmpty()) {
            apiKey = QString::fromLocal8Bit(qgetenv("LASTFM_API_KEY")).trimmed();
        }
        if (apiKey.isEmpty()) {
            apiKey = QString::fromStdString(LastFmCredentials::defaultApiKey()).trimmed();
        }
        if (username.isEmpty() || apiKey.isEmpty()) {
            return QStringLiteral("missing-credentials");
        }
        m_backfillRunning = true;
        m_backfillStatus = BackfillStatus{};
        m_backfillStatus.service = ListenHistoryStore::LastFm;
        m_backfillStatus.running = true;
        emit backfillStatusChanged();
        QMetaObject::invokeMethod(m_scrobbleBackfill, "startLastFmCountSync", Qt::QueuedConnection,
                                  Q_ARG(QString, apiKey), Q_ARG(QString, username),
                                  Q_ARG(QString, historyPath),
                                  Q_ARG(ScrobbleBackfill::LibraryIndex, index));
    }
    return QStringLiteral("started");
}

void AppCore::cancelBackfill()
{
    // Write the flag first (and from the main thread's own store instance) so
    // it survives even if the app quits before the worker thread acknowledges
    // the abort. Harmless when nothing is running or the job is Last.fm — the
    // flag only gates ListenBrainz auto-resume.
    if (m_listenHistory != nullptr && m_listenHistory->isOpen()) {
        m_listenHistory->setMetaValue(ScrobbleBackfill::CanceledMetaKey, QStringLiteral("1"));
    }
    QMetaObject::invokeMethod(m_scrobbleBackfill, "abort", Qt::QueuedConnection);
}

void AppCore::maybeAutoResumeListenBrainzBackfill()
{
    if (m_backfillRunning || m_listenHistory == nullptr || !m_listenHistory->isOpen()) {
        return;
    }
    const QString cursor = m_listenHistory->metaValue(ScrobbleBackfill::OldestTsMetaKey);
    const QString canceled = m_listenHistory->metaValue(ScrobbleBackfill::CanceledMetaKey);
    const QString token = m_database->setting(QStringLiteral("listenbrainz.token")).trimmed();
    if (cursor.isEmpty() || !canceled.isEmpty() || token.isEmpty()) {
        return;
    }
    qInfo("scrobble-backfill: resuming interrupted ListenBrainz import");
    startBackfill(QStringLiteral("listenbrainz"));
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
        // Build/show the window after this IPC callback returns — showWindow()
        // constructs a whole MainWindow and would otherwise pump the event loop
        // from inside the socket read handler (re-entrancy → use-after-free).
        QMetaObject::invokeMethod(this, [this]() { showWindow(); }, Qt::QueuedConnection);
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
        const auto oldRating = m_database->trackRatingSnapshot(rated.path);
        bool ok = false;
        if (rating >= 0) {
            ok = m_database->setUserTrackRating(rated.path, rating);
            if (ok) {
                m_database->setPendingTrackRatingWrite(rated.path, rating, QStringLiteral("pending"));
            }
        } else {
            ok = m_database->clearUserTrackRating(rated.path);
            if (ok) {
                m_database->clearPendingTrackRatingWrite(rated.path);
            }
        }
        if (!ok) {
            return error(QStringLiteral("rate failed: %1").arg(m_database->lastError()));
        }
        Track eventTrack = rated;
        if (eventTrack.musicBrainz.recordingId.isEmpty()) {
            eventTrack.musicBrainz.recordingId = oldRating.mbRecordingId;
        }
        recordRatingEvent(eventTrack,
                          oldRating.hasUserRating,
                          oldRating.userRating0To100,
                          oldRating.effectiveRating0To100,
                          rating,
                          QStringLiteral("ipc"));
        m_player->updateTrackRating(rated.path, rating >= 0 ? rating : rated.rating0To100, rating >= 0);
        return status();
    }
    if (command == QLatin1String("scrobble-backfill")) {
        const QString service = args.value(QStringLiteral("service")).toString().trimmed().toLower();
        if (service == QLatin1String("cancel")) {
            cancelBackfill();
            return QJsonObject{{QStringLiteral("backfill"), QStringLiteral("cancel-requested")}};
        }
        if (service == QLatin1String("status")) {
            const BackfillStatus current = backfillStatus();
            return QJsonObject{
                {QStringLiteral("service"), current.service},
                {QStringLiteral("running"), current.running},
                {QStringLiteral("processed"), current.processed},
                {QStringLiteral("inserted"), current.inserted},
                {QStringLiteral("reachedTs"), static_cast<double>(current.reachedTs)},
                {QStringLiteral("totalListens"), static_cast<double>(current.totalListens)},
                {QStringLiteral("message"), current.lastMessage},
            };
        }
        const QString result = startBackfill(service);
        if (result == QLatin1String("unknown-service")) {
            return error(QStringLiteral("scrobble-backfill needs \"service\": \"listenbrainz\", \"lastfm\", \"status\", or \"cancel\""));
        }
        return QJsonObject{{QStringLiteral("backfill"), result}, {QStringLiteral("service"), service}};
    }
    if (command == QLatin1String("start-radio")) {
        const QString path = QFileInfo(args.value(QStringLiteral("path")).toString()).absoluteFilePath();
        if (path.isEmpty()) {
            return error(QStringLiteral("start-radio needs a \"path\""));
        }
        const bool started = startRadio(path);
        return QJsonObject{{QStringLiteral("radio"),
                            started ? QStringLiteral("started") : QStringLiteral("unknown-track")}};
    }
    if (command == QLatin1String("start-mix")) {
        const QString mode = args.value(QStringLiteral("mode")).toString().trimmed().toLower();
        if (!RadioMix::modeFromString(mode)) {
            return QJsonObject{{QStringLiteral("mix"), QStringLiteral("unknown-mode")}};
        }
        const bool started = startMix(mode);
        return QJsonObject{{QStringLiteral("mix"),
                            started ? QStringLiteral("started") : QStringLiteral("empty-pool")}};
    }
    if (command == QLatin1String("radio-reasons")) {
        QJsonArray picks;
        if (m_radioSession) {
            QHash<QString, Track> queuedTracks;
            for (const Track &track : m_player->queue()) {
                if (!track.path.isEmpty() && !queuedTracks.contains(track.path)) {
                    queuedTracks.insert(track.path, track);
                }
            }
            for (const RadioSession::PickReason &reason : m_radioSession->pickReasons()) {
                QJsonObject pick{
                    {QStringLiteral("path"), reason.path},
                    {QStringLiteral("components"), reasonComponentsJson(reason.components)},
                    {QStringLiteral("sentence"), ReasonText::sentence(reason.components)},
                    {QStringLiteral("breakdown"), ReasonText::breakdown(reason.components)},
                };
                const auto queued = queuedTracks.constFind(reason.path);
                if (queued != queuedTracks.constEnd()) {
                    pick.insert(QStringLiteral("artist"), queued->artistName);
                    pick.insert(QStringLiteral("title"), queued->title.isEmpty() ? queued->filename : queued->title);
                }
                picks.append(pick);
            }
        }
        return QJsonObject{
            {QStringLiteral("active"), static_cast<bool>(m_radioSession)},
            {QStringLiteral("kind"), m_radioSessionKind},
            {QStringLiteral("picks"), picks},
        };
    }
    if (command == QLatin1String("stop-radio")) {
        stopRadio();
        return QJsonObject{{QStringLiteral("radio"), QStringLiteral("stopped")}};
    }
    return error(QStringLiteral("unknown command \"%1\"").arg(command));
}
