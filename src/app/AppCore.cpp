#include "app/AppCore.h"

#include "app/AppPaths.h"
#include "core/FoldKey.h"
#include "core/GenreTags.h"
#include "core/MetadataBlob.h"
#include "db/Database.h"
#include "db/PlaylistDatabase.h"
#include "db/SettingsStore.h"
#include "features/FeatureStore.h"
#include "features/QualityRank.h"
#include "features/SongIdentity.h"
#include "fs/LinkRoot.h"
#include "ipc/IpcServer.h"
#include "mpris/MprisService.h"
#include "playback/GStreamerPlaybackBackend.h"
#include "playback/PlaybackBackend.h"
#include "player/PlayerCore.h"
#include "reco/AffinityPool.h"
#include "reco/ArtistRadio.h"
#include "reco/RadioFilters.h"
#include "reco/RadioMix.h"
#include "reco/RadioProfile.h"
#include "reco/RadioSession.h"
#include "reco/ReasonText.h"
#include "reco/TrackScorer.h"
#include "scanner/ArtworkCache.h"
#include "scrobble/LastFmCredentials.h"
#include "scrobble/LastFmScrobbler.h"
#include "scrobble/ListenBrainzHub.h"
#include "scrobble/ListenHistoryStore.h"
#include "scrobble/ListenTracker.h"
#include "scrobble/PlayEventRecorder.h"
#include "scrobble/ScrobbleBackfill.h"
#include "ui/MainWindow.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

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
                                        const QSet<QString> &ignoredRadioGenres,
                                        const QHash<QString, QString> &resolvedSongKeys)
{
    TrackScorer::Candidate candidate;
    candidate.path = row.path;
    candidate.songKey = resolvedSongKeys.value(row.path, FoldKey::songKey(row.mbRecordingId, row.artistName, row.title));
    candidate.artistFolded = FoldKey::fold(row.artistName);
    candidate.albumKey = FoldKey::albumGroupKey(row.releaseGroupId, row.albumArtistName, row.albumTitle);
    candidate.genresFolded = canonicalRadioGenres(row.genresFolded, genreAliases, ignoredRadioGenres);
    candidate.year = row.year;
    candidate.effectiveRating0To100 = row.effectiveRating0To100;
    candidate.hasUserRating = row.hasUserRating;
    return candidate;
}

QStringList mediaTagsForPath(const Database &db, const QString &path)
{
    QStringList media;
    const MetadataBlob::FullMetadata metadata = db.fullMetadata(path);
    for (auto it = metadata.tags.cbegin(); it != metadata.tags.cend(); ++it) {
        if (it.key().compare(QStringLiteral("MEDIA"), Qt::CaseInsensitive) == 0) {
            media.append(it.value());
        }
    }
    return media;
}

QualityRank::Copy qualityCopyForTrack(const Track &track, const QStringList &mediaTags)
{
    return QualityRank::Copy{
        track.path,
        track.codec,
        track.bitDepth,
        track.sampleRateHz,
        track.bitrateKbps,
        mediaTags,
    };
}

void applyFeatureScalars(TrackScorer::Candidate &candidate, const FeatureStore::Scalars &scalars)
{
    if (!scalars.valid) {
        return;
    }
    if (scalars.tempoBpm > 0.0) {
        candidate.tempoBpm = scalars.tempoBpm;
    }
    if (scalars.energy >= 0.0) {
        candidate.energy = scalars.energy;
    }
}

// Radio settings defaults/limits (plans/music-recommendation-plan.md, "Batch
// radio queue"). batchSize == 1 must reproduce the pre-batch, pure-JIT
// behaviour exactly -- see AppCore::startRadio/appendRadioBatch.
constexpr int kDefaultRadioExploration = 30;
constexpr int kDefaultRadioBatchSize = 15;
constexpr int kAdventurousExploration = 85;
constexpr int kRadioNeighborAugmentLimit = 200;
constexpr int kDefaultRadioRefillThreshold = 5;

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
    m_radioRefillThreshold = std::clamp(
        m_database->setting(QStringLiteral("radio.refillThreshold"),
                            QString::number(kDefaultRadioRefillThreshold)).toInt(),
        0, 100);

    m_playlistDb = std::make_unique<PlaylistDatabase>(QStringLiteral("playlists-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!m_playlistDb->open(playlistDatabasePath())) {
        qWarning("AppCore: failed to open playlist database: %s", qPrintable(m_playlistDb->lastError()));
    }

    m_state = std::make_unique<SettingsStore>(QDir(AppPaths::stateDir()).filePath(QStringLiteral("state.sqlite")));
    AppPaths::writeDefaultConfigIfMissing();
    m_radioProfileStore.load(m_database->setting(QStringLiteral("radio.scoringWeights")).toUtf8());

    const int artworkSize = std::clamp(m_state->setting(QStringLiteral("artwork.size"), QStringLiteral("1024")).toInt(), 128, 4096);
    m_artworkCache = std::make_unique<ArtworkCache>(QDir(AppPaths::cacheDir()).filePath(QStringLiteral("artwork.sqlite")), artworkSize);

    m_features = std::make_unique<FeatureStore>(featuresPath());
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
        // Only destinations enabled at this moment are owed the listen; one
        // enabled later must not inherit history it never saw.
        const ScrobbleDestinationSet destinations = scrobbleDestinations();
        QStringList owed;
        for (const ScrobbleDestination &destination : destinations.items) {
            if (destination.enabled) {
                owed << destination.id;
            }
        }
        m_listenHistory->recordListen(track, startedAtSecs, owed);
        if (owed.contains(ScrobbleDestinationConfig::listenBrainzId())) {
            m_listenBrainzHub->uploadBacklog();
        }
        if (owed.contains(ScrobbleDestinationConfig::lastFmId())) {
            QMetaObject::invokeMethod(m_lastFmScrobbler, "uploadBacklog", Qt::QueuedConnection);
        }
    });

    m_playEventRecorder = new PlayEventRecorder(this);
    connect(m_playEventRecorder, &PlayEventRecorder::playEventReady, this,
            [this](ListenHistoryStore::PlayEvent event) {
                m_listenHistory->recordPlayEvent(event);
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
        // currentTrackChanged follows this signal synchronously and advances
        // the radio session's rolling context. Queue the refill check so its
        // worker snapshot includes the track that just became current.
        QTimer::singleShot(0, this, [this]() { maybeTopUpRadioQueue(); });
    });
    connect(m_player, &PlayerCore::queueChanged, this, [this]() {
        if (m_player != nullptr && m_player->radioActive()) {
            ++m_radioQueueRevision;
        }
        reconcileRadioPendingState();
    });
    connect(m_player, &PlayerCore::queueReset, this, [this]() {
        if (m_player != nullptr && m_player->radioActive()) {
            ++m_radioQueueRevision;
        }
        reconcileRadioPendingState();
    });
    connect(m_player, &PlayerCore::aboutToNavigateWithoutRejecting, this, [this]() {
        m_nextTransitionIsNavigation = true;
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

    m_listenBrainzHub = new ListenBrainzHub(this);

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
FeatureStore *AppCore::features() const { return m_features.get(); }
ListenHistoryStore *AppCore::listenHistory() const { return m_listenHistory.get(); }
ListenTracker *AppCore::listenTracker() const { return m_listenTracker; }
PlayEventRecorder *AppCore::playEventRecorder() const { return m_playEventRecorder; }
MprisService *AppCore::mpris() const { return m_mpris; }
IpcServer *AppCore::ipc() const { return m_ipc; }
MainWindow *AppCore::window() const { return m_window; }
ScrobbleDestinationSet AppCore::scrobbleDestinations() const
{
    return ScrobbleDestinationConfig::load(
        [this](const QString &key) { return m_database->setting(key); });
}

void AppCore::setScrobbleDestinations(const ScrobbleDestinationSet &destinations)
{
    ScrobbleDestinationConfig::save(
        [this](const QString &key, const QString &value) { m_database->setSetting(key, value); },
        destinations);
}

ListenBrainzHub *AppCore::listenBrainzHub() const { return m_listenBrainzHub; }
LastFmScrobbler *AppCore::lastFmScrobbler() const { return m_lastFmScrobbler; }
QThread *AppCore::lastFmThread() const { return m_lastFmThread; }

QString AppCore::databasePath() const
{
    return QDir(AppPaths::dataDir()).filePath(QStringLiteral("library.sqlite"));
}

QString AppCore::playlistDatabasePath() const
{
    return QDir(AppPaths::dataDir()).filePath(QStringLiteral("playlists.sqlite"));
}

QString AppCore::featuresPath() const
{
    return QDir(AppPaths::dataDir()).filePath(QStringLiteral("features.sqlite"));
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
    connect(m_mpris, &MprisService::stopRequested, m_player, &PlayerCore::stop);
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
        m_listenBrainzHub->playbackStateChanged(playing);
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
        if (!track.path.isEmpty()) {
            m_queueHeardPaths.insert(track.path);
        }
        // Consume the attribution set by the preceding currentIndexChanged /
        // aboutToInjectLibraryTrack regardless of notifyScrobbler, so a silent
        // present/restore does not leave stale flags for the next real start.
        const bool userInitiated = m_nextStartUserInitiated;
        const bool injected = m_nextStartInjected;
        const bool navigation = m_nextTransitionIsNavigation;
        m_nextStartInjected = false;
        m_nextTransitionIsNavigation = false;
        bool radioShuffleResynced = false;
        if (m_radioShuffleAwaitingInitialTrack
            && m_player->shuffleMode() == ShuffleMode::Radio
            && !m_player->radioActive()
            && !track.path.isEmpty()) {
            syncRadioShuffleSession();
            radioShuffleResynced = !m_radioShuffleAwaitingInitialTrack;
        }
        // A silent present/restore (notifyScrobbler == false) must not open a
        // play event; only real track starts do. Consume transition attribution
        // anyway so it cannot leak into a later start.
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
        // Previous and direct row picks end the outgoing spin as navigation,
        // not as a skip/rejection. Next remains a genuine skip.
        m_playEventRecorder->trackStarted(
            track, userInitiated, source,
            navigation ? QStringLiteral("navigated") : QStringLiteral("skipped"));
        // Advance the radio rolling context with every real track start while a
        // session is active — the seed, radio picks, and user-queued
        // interruptions alike (they all shape mood continuity).
        if (!radioShuffleResynced && m_radioSession
            && (m_player->radioActive() || m_radioShuffleSessionActive)) {
            m_radioSession->notePlayed(track);
            ++m_radioSessionRevision;
            saveRadioSessionState();
            reconcileRadioPendingState();
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

void AppCore::recordUserQueueRemovals(const QVector<int> &rows)
{
    if (m_player == nullptr || m_listenHistory == nullptr || rows.isEmpty()) {
        return;
    }

    const QVector<Track> queue = m_player->queue();
    if (queue.isEmpty()) {
        return;
    }

    QVector<int> sortedRows = rows;
    std::sort(sortedRows.begin(), sortedRows.end());
    sortedRows.erase(std::unique(sortedRows.begin(), sortedRows.end()), sortedRows.end());

    const qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
    const bool radioActive = m_player->radioActive() || m_radioShuffleSessionActive;
    for (int row : sortedRows) {
        if (row < 0 || row >= queue.size()) {
            continue;
        }
        const Track &track = queue.at(row);
        if (track.path.isEmpty()) {
            continue;
        }

        ListenHistoryStore::QueueRemovalEvent event;
        event.occurredAtSecs = nowSecs;
        event.track = track;
        event.wasRadioPick = m_radioPickPaths.contains(track.path);
        event.wasUnheard = !m_queueHeardPaths.contains(track.path);
        event.radioActive = radioActive;
        m_listenHistory->recordQueueRemoval(event);

        if (event.wasRadioPick) {
            m_radioPickPaths.remove(track.path);
        }
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
    menu->addAction(QStringLiteral("Stop"), m_player, &PlayerCore::stop);
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
    m_listenBrainzHub->trackStarted(track);
    QMetaObject::invokeMethod(m_lastFmScrobbler, "trackStarted", Qt::QueuedConnection, Q_ARG(Track, track));
}

void AppCore::resumeScrobblers(const Track &track, qint64 elapsedMs, bool playing)
{
    m_listenTracker->resumeTrack(track, elapsedMs, playing);
    m_playEventRecorder->resumeTrack(track, elapsedMs, playing, QStringLiteral("resume"));
    m_listenBrainzHub->resumeTrack(track, elapsedMs, playing);
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

    QHash<QString, SongIdentity::TrackIdentity> identities;
    QStringList pendingPaths{trackPath};
    QStringList pendingMbids;
    QStringList pendingFallbacks;
    QSet<QString> scheduledPaths{trackPath};
    QSet<QString> scheduledMbids;
    QSet<QString> scheduledFallbacks;
    QSet<qint64> expandedGroups;

    const auto schedule = [](const QString &value, QStringList &pending, QSet<QString> &scheduled) {
        if (!value.isEmpty() && !scheduled.contains(value)) {
            scheduled.insert(value);
            pending.push_back(value);
        }
    };

    while (!pendingPaths.isEmpty() || !pendingMbids.isEmpty() || !pendingFallbacks.isEmpty()) {
        const auto rows = m_database->trackMatchRowsForIdentityKeys(
            std::exchange(pendingPaths, QStringList{}),
            std::exchange(pendingMbids, QStringList{}),
            std::exchange(pendingFallbacks, QStringList{}));
        QStringList rowPaths;
        rowPaths.reserve(rows.size());
        for (const auto &[path, artist, title, recordingMbid] : rows) {
            Q_UNUSED(artist);
            Q_UNUSED(title);
            Q_UNUSED(recordingMbid);
            if (!identities.contains(path)) {
                rowPaths.push_back(path);
            }
        }
        const QHash<QString, qint64> contentGroups =
            (m_features != nullptr && m_features->isOpen())
                ? m_features->contentGroupsForPaths(rowPaths)
                : QHash<QString, qint64>{};

        for (const auto &[path, artist, title, recordingMbid] : rows) {
            if (identities.contains(path)) {
                continue;
            }
            const SongIdentity::TrackIdentity identity{
                path,
                artist,
                title,
                recordingMbid,
                contentGroups.value(path, -1),
            };
            identities.insert(path, identity);

            if (!recordingMbid.isEmpty()) {
                schedule(recordingMbid, pendingMbids, scheduledMbids);
            } else {
                schedule(FoldKey::songKey({}, artist, title), pendingFallbacks, scheduledFallbacks);
            }
            if (identity.contentGroupId >= 0 && !expandedGroups.contains(identity.contentGroupId)) {
                expandedGroups.insert(identity.contentGroupId);
                for (const QString &groupPath : m_features->pathsInGroup(identity.contentGroupId)) {
                    schedule(groupPath, pendingPaths, scheduledPaths);
                }
            }
        }
    }

    const QStringList paths = SongIdentity::pathsConnectedToTrack(identities.values(), trackPath);
    return paths.isEmpty() ? QStringList{trackPath} : paths;
}

QHash<QString, QString> AppCore::buildResolvedSongKeyMap() const
{
    QHash<QString, QString> resolved;
    if (m_database == nullptr) {
        return resolved;
    }

    const auto rows = m_database->trackMatchRows();
    QStringList paths;
    paths.reserve(rows.size());
    for (const auto &[path, artist, title, recordingMbid] : rows) {
        Q_UNUSED(artist);
        Q_UNUSED(title);
        Q_UNUSED(recordingMbid);
        paths.push_back(path);
    }

    const QHash<QString, qint64> contentGroups =
        (m_features != nullptr && m_features->isOpen())
            ? m_features->contentGroupsForPaths(paths)
            : QHash<QString, qint64>{};

    QList<SongIdentity::TrackIdentity> identities;
    identities.reserve(rows.size());
    for (const auto &[path, artist, title, recordingMbid] : rows) {
        identities.push_back({
            path,
            artist,
            title,
            recordingMbid,
            contentGroups.value(path, -1),
        });
    }
    return SongIdentity::resolvedSongKeys(identities);
}

void AppCore::attachRadioFeatures(QVector<TrackScorer::Candidate> &candidates) const
{
    if (m_features == nullptr || !m_features->isOpen() || candidates.isEmpty()) {
        return;
    }

    QStringList paths;
    paths.reserve(candidates.size());
    for (const TrackScorer::Candidate &candidate : candidates) {
        if (!candidate.path.isEmpty()) {
            paths.push_back(candidate.path);
        }
    }
    const QHash<QString, qint64> groups = m_features->contentGroupsForPaths(paths);
    if (groups.isEmpty()) {
        return;
    }

    QList<qint64> groupIds;
    QSet<qint64> seenGroups;
    groupIds.reserve(groups.size());
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        if (it.value() < 0 || seenGroups.contains(it.value())) {
            continue;
        }
        seenGroups.insert(it.value());
        groupIds.push_back(it.value());
    }
    const QHash<qint64, FeatureStore::Scalars> scalarsByGroup = m_features->scalarsForGroups(groupIds);

    for (TrackScorer::Candidate &candidate : candidates) {
        const qint64 groupId = groups.value(candidate.path, -1);
        candidate.contentGroupId = groupId;
        applyFeatureScalars(candidate, scalarsByGroup.value(groupId));
    }
}

void AppCore::attachRadioFeatures(TrackScorer::Candidate &candidate) const
{
    QVector<TrackScorer::Candidate> candidates{candidate};
    attachRadioFeatures(candidates);
    candidate = candidates.first();
}

QStringList AppCore::radioNeighborCandidatePaths(const QStringList &anchorPaths) const
{
    QStringList paths;
    if (m_database == nullptr || m_features == nullptr || !m_features->isOpen() || anchorPaths.isEmpty()) {
        return paths;
    }

    const QHash<QString, qint64> anchorGroups = m_features->contentGroupsForPaths(anchorPaths);
    QSet<qint64> seenAnchorGroups;
    QSet<QString> seenPaths;
    for (auto it = anchorGroups.cbegin(); it != anchorGroups.cend(); ++it) {
        const qint64 anchorGroup = it.value();
        if (anchorGroup < 0 || seenAnchorGroups.contains(anchorGroup)) {
            continue;
        }
        seenAnchorGroups.insert(anchorGroup);

        const QList<QPair<qint64, double>> neighbors =
            m_features->neighborsOfGroup(anchorGroup, kRadioNeighborAugmentLimit);
        for (const auto &neighbor : neighbors) {
            const QStringList groupPaths = m_features->pathsInGroup(neighbor.first);
            if (groupPaths.isEmpty()) {
                continue;
            }
            Track representative = m_database->trackForPath(groupPaths.first());
            if (representative.path.isEmpty()) {
                continue;
            }
            m_database->enrichTrackForStatus(representative);
            const Track best = bestRadioCopyForPick(representative, {});
            const QString path = best.path.isEmpty() ? representative.path : best.path;
            if (!path.isEmpty() && !seenPaths.contains(path)) {
                seenPaths.insert(path);
                paths.push_back(path);
            }
        }
    }
    return paths;
}

QHash<qint64, QVector<float>> AppCore::radioEmbeddingsForSession(
    const QVector<TrackScorer::Candidate> &pool,
    const QVector<TrackScorer::Candidate> &anchors) const
{
    QHash<qint64, QVector<float>> embeddings;
    if (m_features == nullptr || !m_features->isOpen()) {
        return embeddings;
    }

    QList<qint64> groupIds;
    QSet<qint64> seenGroups;
    auto addGroup = [&](qint64 groupId) {
        if (groupId < 0 || seenGroups.contains(groupId)) {
            return;
        }
        seenGroups.insert(groupId);
        groupIds.push_back(groupId);
    };
    for (const TrackScorer::Candidate &anchor : anchors) {
        addGroup(anchor.contentGroupId);
    }
    for (const TrackScorer::Candidate &candidate : pool) {
        addGroup(candidate.contentGroupId);
    }
    return m_features->embeddingsForGroups(groupIds);
}

TrackScorer::Candidate AppCore::buildRadioSeedCandidate(const Track &seed,
                                                        const QStringList &seedGenresFolded,
                                                        const QHash<QString, QString> &resolvedSongKeys) const
{
    TrackScorer::Candidate seedCandidate;
    seedCandidate.path = seed.path;
    seedCandidate.songKey = resolvedSongKeys.value(seed.path,
                                                   FoldKey::songKey(seed.musicBrainz.recordingId,
                                                                    seed.artistName, seed.title));
    seedCandidate.artistFolded = FoldKey::fold(seed.artistName);
    seedCandidate.albumKey = FoldKey::albumKey(seed.albumArtistName, seed.albumTitle);
    seedCandidate.genresFolded = seedGenresFolded;
    seedCandidate.year = trackYear(seed);
    seedCandidate.effectiveRating0To100 = seed.effectiveRating0To100;
    seedCandidate.hasUserRating = seed.hasUserRating;
    attachRadioFeatures(seedCandidate);
    return seedCandidate;
}

QVector<TrackScorer::Candidate> AppCore::buildRadioCandidatePool(const QStringList &informativeGenres,
                                                                 const QHash<QString, QString> &genreAliases,
                                                                 const QSet<QString> &ignoredRadioGenres,
                                                                 const QHash<QString, QString> &resolvedSongKeys,
                                                                 const QStringList &neighborAnchorPaths) const
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

    QSet<QString> seenPaths;
    seenPaths.reserve(rows.size());
    for (const RadioCandidateRow &row : rows) {
        seenPaths.insert(row.path);
    }
    const QStringList neighborPaths = radioNeighborCandidatePaths(neighborAnchorPaths);
    if (!neighborPaths.isEmpty()) {
        const QVector<RadioCandidateRow> neighborRows = m_database->radioCandidatesForPaths(neighborPaths);
        for (const RadioCandidateRow &row : neighborRows) {
            if (!seenPaths.contains(row.path)) {
                seenPaths.insert(row.path);
                rows.push_back(row);
            }
        }
    }

    QVector<TrackScorer::Candidate> pool;
    pool.reserve(rows.size());
    for (const RadioCandidateRow &row : rows) {
        pool.push_back(candidateFromRow(row, genreAliases, ignoredRadioGenres, resolvedSongKeys));
    }
    // User taste flags are path-scoped storage but AppCore applies them at the
    // recommender boundary, after every SQL candidate source has been merged.
    pool = RadioFilters::excludeFlaggedCandidates(
        pool, m_database->flaggedPaths(Database::TrackFlag::NeverRadio));
    attachRadioFeatures(pool);
    return pool;
}

QVector<TrackScorer::Candidate> AppCore::buildRadioFallbackPool(int limit,
                                                                const QHash<QString, QString> &genreAliases,
                                                                const QSet<QString> &ignoredRadioGenres,
                                                                const QHash<QString, QString> &resolvedSongKeys) const
{
    QVector<TrackScorer::Candidate> pool;
    if (m_database == nullptr) {
        return pool;
    }

    const QVector<RadioCandidateRow> rows = m_database->radioFallbackCandidates(limit);
    pool.reserve(rows.size());
    for (const RadioCandidateRow &row : rows) {
        pool.push_back(candidateFromRow(row, genreAliases, ignoredRadioGenres, resolvedSongKeys));
    }
    pool = RadioFilters::excludeFlaggedCandidates(
        pool, m_database->flaggedPaths(Database::TrackFlag::NeverRadio));
    attachRadioFeatures(pool);
    return pool;
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
    return m_radioProfileStore.activeProfile().weights;
}

TrackScorer::RadioSessionDecay AppCore::radioSessionDecay() const
{
    return m_radioProfileStore.activeProfile().sessionDecay;
}

const QVector<RadioProfile> &AppCore::radioProfiles() const
{
    return m_radioProfileStore.profiles();
}

const RadioProfile &AppCore::activeRadioProfile() const
{
    return m_radioProfileStore.activeProfile();
}

void AppCore::applyActiveRadioProfile()
{
    m_radioSessionWeights = radioScoringWeights();
    m_radioSessionDecay = radioSessionDecay();
    if (m_radioSession != nullptr) {
        m_radioSession->setWeights(m_radioSessionWeights);
        m_radioSession->setSessionDecay(m_radioSessionDecay);
        ++m_radioSessionRevision;
    }
}

bool AppCore::selectRadioProfile(const QString &name)
{
    if (!m_radioProfileStore.setActiveProfileName(name)) {
        return false;
    }
    applyActiveRadioProfile();
    return m_radioProfileStore.save();
}

bool AppCore::previewActiveRadioProfile(const RadioProfile &profile)
{
    if (!m_radioProfileStore.previewActiveProfile(profile)) {
        return false;
    }
    applyActiveRadioProfile();
    return true;
}

bool AppCore::commitActiveRadioProfilePreview()
{
    if (!m_radioProfileStore.commitActivePreview()) {
        return false;
    }
    return m_radioProfileStore.save();
}

bool AppCore::restoreActiveRadioProfile(const RadioProfile &profile)
{
    if (!m_radioProfileStore.restoreActiveProfile(profile)) {
        return false;
    }
    applyActiveRadioProfile();
    return m_radioProfileStore.save();
}

bool AppCore::undoRadioProfile()
{
    if (!m_radioProfileStore.undo()) {
        return false;
    }
    applyActiveRadioProfile();
    return m_radioProfileStore.save();
}

bool AppCore::redoRadioProfile()
{
    if (!m_radioProfileStore.redo()) {
        return false;
    }
    applyActiveRadioProfile();
    return m_radioProfileStore.save();
}

bool AppCore::canUndoRadioProfile() const { return m_radioProfileStore.canUndo(); }
bool AppCore::canRedoRadioProfile() const { return m_radioProfileStore.canRedo(); }

bool AppCore::createRadioProfile(const QString &name, bool duplicateActive)
{
    if (!m_radioProfileStore.createProfile(name, duplicateActive)) {
        return false;
    }
    applyActiveRadioProfile();
    return m_radioProfileStore.save();
}

bool AppCore::renameActiveRadioProfile(const QString &name)
{
    if (!m_radioProfileStore.renameActiveProfile(name)) {
        return false;
    }
    applyActiveRadioProfile();
    return m_radioProfileStore.save();
}

bool AppCore::deleteActiveRadioProfile()
{
    if (!m_radioProfileStore.deleteActiveProfile()) {
        return false;
    }
    applyActiveRadioProfile();
    return m_radioProfileStore.save();
}

bool AppCore::resetActiveRadioProfile()
{
    if (!m_radioProfileStore.resetActiveProfile()) {
        return false;
    }
    applyActiveRadioProfile();
    return m_radioProfileStore.save();
}

void AppCore::recordRadioPicks(const QVector<Track> &picks)
{
    if (m_listenHistory == nullptr || m_radioSession == nullptr || picks.isEmpty()) {
        return;
    }

    const qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
    const QString sessionKind = m_radioSessionKind.isEmpty() ? QStringLiteral("anchorless") : m_radioSessionKind;
    const QByteArray weightsJson = TrackScorer::weightsToJson(m_radioSessionWeights);
    for (const Track &track : picks) {
        if (track.path.isEmpty()) {
            continue;
        }
        const QList<TrackScorer::Component> components = m_radioSession->reasonComponentsFor(track.path);
        QVector<ListenHistoryStore::RadioPickComponent> persistedComponents;
        persistedComponents.reserve(components.size());
        double score = 0.0;
        for (const TrackScorer::Component &component : components) {
            persistedComponents.push_back({component.name, component.value});
            score += component.value;
        }

        ListenHistoryStore::RadioPickEvent event;
        event.occurredAtSecs = nowSecs;
        event.track = track;
        event.sessionKind = sessionKind;
        event.exploration0To100 = m_radioSessionExploration;
        event.weightsJson = weightsJson;
        event.components = persistedComponents;
        event.score = score;
        m_listenHistory->recordRadioPick(event);
    }
}

QHash<QString, TrackScorer::Affinity> AppCore::buildRadioAffinities(const QHash<QString, QString> &resolvedSongKeys) const
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

    QHash<QString, QString> pathToSongKey = RadioFilters::excludeFlaggedPathMappings(resolvedSongKeys, noLearnPaths);
    return RadioFilters::excludeFlaggedAffinities(
        AffinityPool::poolBySongKey(affinities, pathToSongKey), noLearnPaths);
}

Track AppCore::bestRadioCopyForPick(const Track &track, const QSet<QString> &blockedPaths) const
{
    if (m_database == nullptr || m_features == nullptr || !m_features->isOpen() || track.path.isEmpty()) {
        return track;
    }

    const qint64 groupId = m_features->contentGroupForPath(track.path);
    if (groupId < 0) {
        return track;
    }
    const QStringList groupPaths = m_features->pathsInGroup(groupId);
    if (groupPaths.size() < 2) {
        return track;
    }

    const QString pinnedPath = m_database->contentGroupPin(groupId);
    QVector<QualityRank::Copy> copies;
    QHash<QString, Track> tracksByPath;
    copies.reserve(groupPaths.size());
    tracksByPath.reserve(groupPaths.size());
    for (const QString &path : groupPaths) {
        if (blockedPaths.contains(path)) {
            continue;
        }
        Track copy = m_database->trackForPath(path);
        if (copy.path.isEmpty()) {
            continue;
        }
        m_database->enrichTrackForStatus(copy);
        const QStringList mediaTags = mediaTagsForPath(*m_database, path);
        copies.push_back(qualityCopyForTrack(copy, mediaTags));
        tracksByPath.insert(path, copy);
    }

    const QString bestPath = QualityRank::bestPath(copies, pinnedPath);
    if (bestPath.isEmpty() || bestPath == track.path) {
        return track;
    }
    return tracksByPath.value(bestPath, track);
}

Track AppCore::resolveRadioPick(const QString &path, const QSet<QString> &blockedPaths) const
{
    if (m_database == nullptr || path.isEmpty()) {
        return {};
    }
    Track track = m_database->trackForPath(path);
    if (track.path.isEmpty()) {
        return {};
    }
    return bestRadioCopyForPick(track, blockedPaths);
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
    QJsonArray seedPaths;
    for (const QString &path : m_radioSessionSeedPaths) {
        seedPaths.append(path);
    }
    root.insert(QStringLiteral("seedPaths"), seedPaths);
    QStringList sortedRadioPickPaths = m_radioPickPaths.values();
    sortedRadioPickPaths.sort();
    QJsonArray radioPickPaths;
    for (const QString &path : sortedRadioPickPaths) {
        radioPickPaths.append(path);
    }
    root.insert(QStringLiteral("radioPickPaths"), radioPickPaths);
    root.insert(QStringLiteral("anchorMode"), m_radioSessionAnchorMode);
    root.insert(QStringLiteral("artistName"), m_radioSessionArtistName);
    root.insert(QStringLiteral("exploration"), m_radioSessionExploration);
    m_state->setSetting(QStringLiteral("radio.session.state"),
                        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

void AppCore::clearRadioSessionState()
{
    if (m_state != nullptr) {
        m_state->removeSetting(QStringLiteral("radio.session.state"));
    }
}

void AppCore::reconcileRadioPendingState()
{
    if (m_radioSession == nullptr || m_player == nullptr || !m_player->radioActive()) {
        return;
    }

    QStringList orderedPaths;
    const QVector<Track> &queue = m_player->queue();
    const int firstPendingRow = std::max(0, m_player->queueIndex() + 1);
    for (int row = firstPendingRow; row < queue.size(); ++row) {
        const QString &path = queue.at(row).path;
        if (m_radioPickPaths.contains(path)) {
            orderedPaths.push_back(path);
        }
    }
    if (m_radioSession->retainPendingPaths(orderedPaths)) {
        ++m_radioSessionRevision;
        saveRadioSessionState();
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
    const QHash<QString, QString> resolvedSongKeys = buildResolvedSongKeyMap();
    const TrackScorer::Weights scoringWeights = radioScoringWeights();
    const TrackScorer::RadioSessionDecay sessionDecay = radioSessionDecay();
    std::unique_ptr<RadioSession> restored;
    QStringList seedPaths;
    QString seedPath;
    QString anchorMode = root.value(QStringLiteral("anchorMode")).toString();
    if (anchorMode != QLatin1String("drift")) {
        anchorMode = QStringLiteral("pinned");
    }
    QString artistName;

    if (kind == QLatin1String("seeded")) {
        if (root.contains(QStringLiteral("seedPaths"))) {
            const QJsonValue value = root.value(QStringLiteral("seedPaths"));
            if (!value.isArray()) {
                clearRadioSessionState();
                return;
            }
            QSet<QString> seenPaths;
            for (const QJsonValue &item : value.toArray()) {
                if (!item.isString() || item.toString().isEmpty() || seenPaths.contains(item.toString())) {
                    if (!item.isString() || item.toString().isEmpty()) {
                        clearRadioSessionState();
                        return;
                    }
                    continue;
                }
                seenPaths.insert(item.toString());
                seedPaths.push_back(item.toString());
            }
        } else {
            seedPath = root.value(QStringLiteral("seedPath")).toString();
            if (!seedPath.isEmpty()) {
                seedPaths.push_back(seedPath);
            }
        }
        if (seedPaths.isEmpty()) {
            qInfo("radio-restore: clearing stale seeded session; no anchors are saved");
            clearRadioSessionState();
            return;
        }

        QVector<Track> seedTracks;
        seedTracks.reserve(seedPaths.size());
        QStringList resolvedSeedPaths;
        QSet<QString> seenResolvedPaths;
        for (const QString &path : seedPaths) {
            const Track seed = m_database->trackForPath(path);
            if (seed.path.isEmpty()) {
                qInfo("radio-restore: clearing stale seeded session; an anchor is no longer in the library");
                clearRadioSessionState();
                return;
            }
            if (seenResolvedPaths.contains(seed.path)) {
                continue;
            }
            seenResolvedPaths.insert(seed.path);
            seedTracks.push_back(seed);
            resolvedSeedPaths.push_back(seed.path);
        }
        seedPaths = std::move(resolvedSeedPaths);
        seedPath = seedPaths.first();

        QStringList informativeGenres;
        QSet<QString> seenGenres;
        QVector<TrackScorer::Candidate> anchors;
        anchors.reserve(seedTracks.size());
        for (const Track &seed : seedTracks) {
            const QStringList seedGenresFolded = radioFoldedGenresForTrack(seed.path, genreAliases, ignoredRadioGenres);
            for (const QString &genre : GenreTags::informative(seedGenresFolded)) {
                if (!seenGenres.contains(genre)) {
                    seenGenres.insert(genre);
                    informativeGenres.push_back(genre);
                }
            }
            anchors.push_back(buildRadioSeedCandidate(seed, seedGenresFolded, resolvedSongKeys));
        }
        QVector<TrackScorer::Candidate> pool =
            buildRadioCandidatePool(informativeGenres, genreAliases, ignoredRadioGenres,
                                    resolvedSongKeys, seedPaths);
        QHash<QString, TrackScorer::Affinity> affinities = buildRadioAffinities(resolvedSongKeys);
        QHash<qint64, QVector<float>> embeddings = radioEmbeddingsForSession(pool, anchors);
        const RadioSession::ContextMode contextMode = anchorMode == QLatin1String("drift")
            ? RadioSession::ContextMode::MovingContext
            : RadioSession::ContextMode::PermanentAnchor;
        if (anchorMode == QLatin1String("pinned") && anchors.size() == 1) {
            restored = std::make_unique<RadioSession>(
                std::move(pool), std::move(affinities), buildRadioGenreIdf(genreAliases, ignoredRadioGenres),
                anchors.first(), exploration, nowSecs, nullptr, scoringWeights,
                std::move(embeddings), sessionDecay);
        } else {
            restored = std::make_unique<RadioSession>(
                std::move(pool), std::move(affinities), buildRadioGenreIdf(genreAliases, ignoredRadioGenres),
                std::move(anchors), contextMode, exploration, nowSecs, nullptr, scoringWeights,
                std::move(embeddings), sessionDecay);
        }
    } else if (kind == QLatin1String("artist")) {
        artistName = root.value(QStringLiteral("artistName")).toString().trimmed();
        const QVector<Track> artistTracks = m_database->tracksForArtist(artistName);
        if (artistName.isEmpty() || artistTracks.isEmpty()) {
            qInfo("radio-restore: clearing stale artist session; artist is no longer in the library");
            clearRadioSessionState();
            return;
        }
        const QStringList seedGenresFolded = ArtistRadio::aggregateSeedGenres(
            m_database->genreCountsForArtist(artistName), genreAliases, ignoredRadioGenres);
        QHash<QString, TrackScorer::Affinity> affinities = buildRadioAffinities(resolvedSongKeys);
        const Track representative = ArtistRadio::representativeTrack(artistTracks, affinities);
        QVector<TrackScorer::Candidate> pool =
            buildRadioCandidatePool(GenreTags::informative(seedGenresFolded), genreAliases, ignoredRadioGenres,
                                    resolvedSongKeys,
                                    representative.path.isEmpty() ? QStringList{} : QStringList{representative.path});
        TrackScorer::Candidate seedCandidate = ArtistRadio::syntheticSeedCandidate(
            artistName, seedGenresFolded, ArtistRadio::medianTrackYear(artistTracks));
        if (m_features != nullptr && m_features->isOpen() && !representative.path.isEmpty()) {
            seedCandidate.contentGroupId = m_features->contentGroupForPath(representative.path);
        }
        const QVector<TrackScorer::Candidate> anchors{seedCandidate};
        QHash<qint64, QVector<float>> embeddings = radioEmbeddingsForSession(pool, anchors);
        if (anchorMode == QLatin1String("pinned")) {
            restored = std::make_unique<RadioSession>(
                std::move(pool), std::move(affinities), buildRadioGenreIdf(genreAliases, ignoredRadioGenres),
                seedCandidate, exploration, nowSecs, nullptr, scoringWeights,
                std::move(embeddings), sessionDecay);
        } else {
            restored = std::make_unique<RadioSession>(
                std::move(pool), std::move(affinities), buildRadioGenreIdf(genreAliases, ignoredRadioGenres),
                QVector<TrackScorer::Candidate>{seedCandidate}, RadioSession::ContextMode::MovingContext,
                exploration, nowSecs, nullptr, scoringWeights, std::move(embeddings), sessionDecay);
        }
    } else if (kind == QLatin1String("anchorless")) {
        const Track current = m_player->currentTrack();
        QVector<TrackScorer::Candidate> pool =
            buildRadioCandidatePool({}, genreAliases, ignoredRadioGenres, resolvedSongKeys,
                                    current.path.isEmpty() ? QStringList{} : QStringList{current.path});
        if (pool.isEmpty()) {
            qInfo("radio-restore: clearing stale anchorless session; candidate pool is empty");
            clearRadioSessionState();
            return;
        }
        QHash<qint64, QVector<float>> embeddings = radioEmbeddingsForSession(pool);
        restored = std::make_unique<RadioSession>(std::move(pool), buildRadioAffinities(resolvedSongKeys),
                                                  buildRadioGenreIdf(genreAliases, ignoredRadioGenres), exploration, nowSecs,
                                                  nullptr, scoringWeights, std::move(embeddings), sessionDecay);
    } else if (const std::optional<RadioMix::Mode> mixMode = RadioMix::modeFromString(kind)) {
        QVector<TrackScorer::Candidate> pool =
            buildRadioFallbackPool(5000, genreAliases, ignoredRadioGenres, resolvedSongKeys);
        QHash<QString, TrackScorer::Affinity> affinities = buildRadioAffinities(resolvedSongKeys);
        pool = RadioMix::filterCandidates(*mixMode, pool, affinities, nowSecs);
        if (pool.isEmpty()) {
            qInfo("radio-restore: clearing stale mix session; filtered pool is empty");
            clearRadioSessionState();
            return;
        }
        QHash<qint64, QVector<float>> embeddings = radioEmbeddingsForSession(pool);
        restored = std::make_unique<RadioSession>(std::move(pool), std::move(affinities),
                                                  buildRadioGenreIdf(genreAliases, ignoredRadioGenres), exploration, nowSecs,
                                                  nullptr, scoringWeights, std::move(embeddings), sessionDecay);
    } else {
        qInfo("radio-restore: clearing malformed session state with unknown kind");
        clearRadioSessionState();
        return;
    }

    restored->restoreConstraintState(root);
    m_radioSession = std::move(restored);
    m_radioSessionWeights = scoringWeights;
    m_radioSessionDecay = sessionDecay;
    m_radioSessionKind = kind;
    m_radioSessionSeedPaths = seedPaths;
    m_radioSessionSeedPath = seedPath;
    m_radioSessionAnchorMode = anchorMode;
    m_radioSessionArtistName = artistName;
    m_radioSessionExploration = exploration;
    m_radioAdventurous = false;
    m_radioShuffleSessionActive = false;
    m_radioPickPaths.clear();
    const QSet<QString> persistedRadioPickPaths = root.contains(QStringLiteral("radioPickPaths"))
        ? stringSetFromJson(root.value(QStringLiteral("radioPickPaths")))
        : root.contains(QStringLiteral("pendingPaths"))
            ? stringSetFromJson(root.value(QStringLiteral("pendingPaths")))
            : stringSetFromJson(root.value(QStringLiteral("usedPaths")));
    const QSet<QString> anchorPaths(seedPaths.cbegin(), seedPaths.cend());
    for (const Track &track : m_player->queue()) {
        if (!anchorPaths.contains(track.path) && persistedRadioPickPaths.contains(track.path)) {
            m_radioPickPaths.insert(track.path);
        }
    }

    installRadioProvider(/*markPicksAsRadio=*/true);
    m_player->setRadioActive(true);
    reconcileRadioPendingState();
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
        const QSet<QString> neverRadioPaths = m_database != nullptr
            ? m_database->flaggedPaths(Database::TrackFlag::NeverRadio)
            : QSet<QString>{};
        QSet<QString> blockedPaths = neverRadioPaths;
        blockedPaths.unite(excludePaths);
        // A batch that resolves nothing can still have advanced the owned RNG,
        // consumed an anchor, or retained pending paths. Comparing the session's
        // mutation generation catches that; comparing whether picks came back
        // does not, and an asynchronous batch already in flight would then be
        // scored against constraint state that has since moved.
        const quint64 generationBefore = m_radioSession->mutationGeneration();
        const QVector<Track> picks = m_radioSession->nextTracks(count, excludePaths, [this, blockedPaths](const QString &path) {
            return resolveRadioPick(path, blockedPaths);
        });
        if (!picks.isEmpty() || m_radioSession->mutationGeneration() != generationBefore) {
            ++m_radioSessionRevision;
            // A speculative mutation would otherwise reach disk only when some
            // later trigger happened to save, so a restart could resume from
            // constraint state the session had already left behind.
            saveRadioSessionState();
        }
        recordRadioPicks(picks);
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
        m_radioShuffleAwaitingInitialTrack = false;
        if (m_radioShuffleSessionActive) {
            m_radioShuffleSessionActive = false;
            m_radioSession.reset();
            m_radioSessionKind.clear();
            m_radioSessionSeedPaths.clear();
            m_radioSessionSeedPath.clear();
            m_radioSessionAnchorMode = QStringLiteral("pinned");
            m_radioSessionArtistName.clear();
            m_player->setRadioProvider({});
        }
        return;
    }

    const QHash<QString, QString> genreAliases = m_database->genreAliases();
    const QSet<QString> ignoredRadioGenres = m_database->ignoredRadioGenres();
    const QHash<QString, QString> resolvedSongKeys = buildResolvedSongKeyMap();
    const Track current = m_player->currentTrack();
    const Track seed = current.path.isEmpty() ? Track{} : m_database->trackForPath(current.path);
    m_radioShuffleAwaitingInitialTrack = seed.path.isEmpty();
    const QStringList seedGenresFolded = seed.path.isEmpty()
        ? QStringList{}
        : radioFoldedGenresForTrack(seed.path, genreAliases, ignoredRadioGenres);
    QVector<TrackScorer::Candidate> pool =
        buildRadioCandidatePool(GenreTags::informative(seedGenresFolded), genreAliases, ignoredRadioGenres,
                                resolvedSongKeys, seed.path.isEmpty() ? QStringList{} : QStringList{seed.path});
    if (!seed.path.isEmpty()) {
        TrackScorer::Candidate currentCandidate = buildRadioSeedCandidate(seed, seedGenresFolded, resolvedSongKeys);
        const auto existing = std::find_if(pool.begin(), pool.end(), [&seed](const TrackScorer::Candidate &candidate) {
            return candidate.path == seed.path;
        });
        if (existing == pool.end()) {
            pool.push_back(std::move(currentCandidate));
        } else {
            *existing = std::move(currentCandidate);
        }
    }
    if (pool.isEmpty()) {
        m_radioShuffleSessionActive = false;
        m_radioSession.reset();
        m_radioSessionKind.clear();
        m_radioSessionSeedPaths.clear();
        m_radioSessionSeedPath.clear();
        m_radioSessionAnchorMode = QStringLiteral("pinned");
        m_radioSessionArtistName.clear();
        m_player->setRadioProvider({});
        return;
    }

    m_radioSessionWeights = radioScoringWeights();
    m_radioSessionDecay = radioSessionDecay();
    QHash<qint64, QVector<float>> embeddings = radioEmbeddingsForSession(pool);
    m_radioSession = std::make_unique<RadioSession>(
        std::move(pool), buildRadioAffinities(resolvedSongKeys),
        buildRadioGenreIdf(genreAliases, ignoredRadioGenres), radioExploration(),
        QDateTime::currentSecsSinceEpoch(), nullptr, m_radioSessionWeights, std::move(embeddings), m_radioSessionDecay);
    m_radioSessionKind = QStringLiteral("anchorless");
    m_radioSessionSeedPaths.clear();
    m_radioSessionSeedPath.clear();
    m_radioSessionAnchorMode = QStringLiteral("pinned");
    m_radioSessionArtistName.clear();
    m_radioSessionExploration = radioExploration();
    m_radioShuffleSessionActive = true;
    installRadioProvider(/*markPicksAsRadio=*/false);
    if (!seed.path.isEmpty()) {
        m_radioSession->notePlayed(current);
    }
}

bool AppCore::startRadio(const QString &seedPath)
{
    return startRadio(QStringList{seedPath});
}

bool AppCore::startRadio(const QStringList &requestedSeedPaths)
{
    if (m_database == nullptr || m_player == nullptr) {
        return false;
    }

    QStringList seedPaths;
    QVector<Track> seedTracks;
    QSet<QString> seenRequestedPaths;
    QSet<QString> seenResolvedPaths;
    for (const QString &requestedPath : requestedSeedPaths) {
        if (requestedPath.isEmpty() || seenRequestedPaths.contains(requestedPath)) {
            continue;
        }
        seenRequestedPaths.insert(requestedPath);
        const Track seed = m_database->trackForPath(requestedPath);
        if (seed.path.isEmpty()) {
            return false;
        }
        if (seenResolvedPaths.contains(seed.path)) {
            continue;
        }
        seenResolvedPaths.insert(seed.path);
        seedPaths.push_back(seed.path);
        seedTracks.push_back(seed);
    }
    if (seedTracks.isEmpty()) {
        return false;
    }

    const quint64 requestId = ++m_radioRequestId;
    ++m_radioQueueRevision;
    m_radioSession.reset();
    m_radioPickPaths.clear();
    m_radioShuffleSessionActive = false;
    m_radioSessionKind = QStringLiteral("seeded");
    m_radioSessionSeedPaths = seedPaths;
    m_radioSessionSeedPath = seedPaths.first();
    m_radioSessionAnchorMode = radioAnchorMode();
    m_radioSessionArtistName.clear();
    m_radioBatchSize = std::clamp(
        m_database->setting(QStringLiteral("radio.batchSize"), QString::number(kDefaultRadioBatchSize)).toInt(),
        1, 100);
    m_radioRefillThreshold = std::clamp(
        m_database->setting(QStringLiteral("radio.refillThreshold"),
                            QString::number(kDefaultRadioRefillThreshold)).toInt(),
        0, 100);

    m_player->setRadioProvider({});
    m_player->setRadioActive(true);
    m_radioTopUpInProgress = false;
    const Track &seed = seedTracks.first();
    const bool seedAlreadyPlaying = m_player->currentTrack().path == seed.path
        && m_playback != nullptr && m_playback->hasSource()
        && m_playback->state() != PlaybackBackend::State::Stopped;
    if (seedAlreadyPlaying) {
        // Starting radio from the audible track is a queue-mode migration, not
        // a new play. Keep the backend and timeline untouched while replacing
        // the old queue tail with the radio tail that is about to arrive.
        m_player->clearKeepingCurrent();
    } else {
        m_player->clearAll();
        m_player->appendAndPlay(seed);
    }

    setRadioLoading(true);
    // Yield back to the event loop after playback/queue migration. Candidate
    // preparation can touch several SQLite-backed stores, while the expensive
    // repeated scoring for the batch itself runs on a worker below.
    QTimer::singleShot(0, this, [this, seedPaths, requestId]() {
        finishSeededRadioStart(seedPaths, requestId);
    });
    return true;
}

void AppCore::finishSeededRadioStart(const QStringList &requestedSeedPaths, quint64 requestId)
{
    if (requestId != m_radioRequestId || m_database == nullptr || m_player == nullptr
        || !m_player->radioActive()) {
        return;
    }

    QStringList seedPaths;
    QVector<Track> seedTracks;
    QSet<QString> seenPaths;
    for (const QString &path : requestedSeedPaths) {
        const Track seed = m_database->trackForPath(path);
        if (seed.path.isEmpty()) {
            stopRadio();
            return;
        }
        if (seenPaths.contains(seed.path)) {
            continue;
        }
        seenPaths.insert(seed.path);
        seedPaths.push_back(seed.path);
        seedTracks.push_back(seed);
    }
    if (seedTracks.isEmpty()) {
        stopRadio();
        return;
    }

    const QHash<QString, QString> genreAliases = m_database->genreAliases();
    const QSet<QString> ignoredRadioGenres = m_database->ignoredRadioGenres();
    const QHash<QString, QString> resolvedSongKeys = buildResolvedSongKeyMap();
    QStringList informativeGenres;
    QSet<QString> seenGenres;
    QVector<TrackScorer::Candidate> anchors;
    anchors.reserve(seedTracks.size());
    for (const Track &seed : seedTracks) {
        const QStringList seedGenresFolded = radioFoldedGenresForTrack(seed.path, genreAliases, ignoredRadioGenres);
        for (const QString &genre : GenreTags::informative(seedGenresFolded)) {
            if (!seenGenres.contains(genre)) {
                seenGenres.insert(genre);
                informativeGenres.push_back(genre);
            }
        }
        anchors.push_back(buildRadioSeedCandidate(seed, seedGenresFolded, resolvedSongKeys));
    }

    QVector<TrackScorer::Candidate> pool =
        buildRadioCandidatePool(informativeGenres, genreAliases, ignoredRadioGenres, resolvedSongKeys, seedPaths);
    QHash<QString, double> genreIdf = buildRadioGenreIdf(genreAliases, ignoredRadioGenres);
    QHash<QString, TrackScorer::Affinity> affinities = buildRadioAffinities(resolvedSongKeys);
    QHash<qint64, QVector<float>> embeddings = radioEmbeddingsForSession(pool, anchors);

    // Dedicated exploration knob so radio never touches the shuffle settings.
    // An armed "adventurous" boost wins over the persisted value for this
    // session's start (one-shot: consumed below regardless of which branch fires).
    const int persistedExploration = std::clamp(
        m_database->setting(QStringLiteral("radio.exploration"), QString::number(kDefaultRadioExploration)).toInt(),
        0, 100);
    const int exploration = m_radioAdventurous ? kAdventurousExploration : persistedExploration;
    m_radioAdventurous = false;

    m_radioSessionSeedPaths = seedPaths;
    m_radioSessionSeedPath = seedPaths.first();
    m_radioSessionExploration = exploration;
    m_radioSessionWeights = radioScoringWeights();
    m_radioSessionDecay = radioSessionDecay();
    const QString anchorMode = m_radioSessionAnchorMode == QLatin1String("drift")
        ? QStringLiteral("drift")
        : QStringLiteral("pinned");
    if (anchorMode == QLatin1String("pinned") && anchors.size() == 1) {
        m_radioSession = std::make_unique<RadioSession>(
            std::move(pool), std::move(affinities), std::move(genreIdf), anchors.first(), exploration,
            QDateTime::currentSecsSinceEpoch(), nullptr, m_radioSessionWeights,
            std::move(embeddings), m_radioSessionDecay);
    } else {
        const RadioSession::ContextMode contextMode = anchorMode == QLatin1String("drift")
            ? RadioSession::ContextMode::MovingContext
            : RadioSession::ContextMode::PermanentAnchor;
        m_radioSession = std::make_unique<RadioSession>(
            std::move(pool), std::move(affinities), std::move(genreIdf), std::move(anchors), contextMode,
            exploration, QDateTime::currentSecsSinceEpoch(), nullptr, m_radioSessionWeights,
            std::move(embeddings), m_radioSessionDecay);
    }
    // Install the scored provider; it resolves each pick to a full Track by
    // path. Stays installed regardless of batch size: it is the safety net for
    // when the queue runs dry (e.g. the user deleted rows ahead of playback).
    installRadioProvider(/*markPicksAsRadio=*/true);
    const Track current = m_player->currentTrack();
    if (!current.path.isEmpty()) {
        m_radioSession->notePlayed(current);
        ++m_radioSessionRevision;
    }
    // batchSize == 1 reproduces the original just-in-time behaviour exactly: no
    // batch append here, and maybeTopUpRadioQueue() is a no-op in that mode too.
    // The JIT provider above is the only source of
    // picks, generated exactly when decideAutoNext() needs one.
    if (m_radioBatchSize > 1) {
        appendRadioBatch(m_radioBatchSize - 1);
    } else {
        setRadioLoading(false);
    }
    saveRadioSessionState();
}

bool AppCore::startArtistRadio(const QString &artistName)
{
    if (m_database == nullptr || m_player == nullptr) {
        return false;
    }
    const QString trimmedArtist = artistName.trimmed();
    const QVector<Track> artistTracks = m_database->tracksForArtist(trimmedArtist);
    if (trimmedArtist.isEmpty() || artistTracks.isEmpty()) {
        return false;
    }
    const QHash<QString, QString> genreAliases = m_database->genreAliases();
    const QSet<QString> ignoredRadioGenres = m_database->ignoredRadioGenres();
    const QHash<QString, QString> resolvedSongKeys = buildResolvedSongKeyMap();
    const QStringList seedGenresFolded = ArtistRadio::aggregateSeedGenres(
        m_database->genreCountsForArtist(trimmedArtist), genreAliases, ignoredRadioGenres);
    const QStringList informativeGenres = GenreTags::informative(seedGenresFolded);

    QHash<QString, double> genreIdf = buildRadioGenreIdf(genreAliases, ignoredRadioGenres);
    QHash<QString, TrackScorer::Affinity> affinities = buildRadioAffinities(resolvedSongKeys);

    const Track representative = ArtistRadio::representativeTrack(artistTracks, affinities);
    if (representative.path.isEmpty()) {
        return false;
    }
    ++m_radioRequestId;
    ++m_radioQueueRevision;
    setRadioLoading(false);
    QVector<TrackScorer::Candidate> pool =
        buildRadioCandidatePool(informativeGenres, genreAliases, ignoredRadioGenres, resolvedSongKeys,
                                {representative.path});
    TrackScorer::Candidate seedCandidate = ArtistRadio::syntheticSeedCandidate(
        trimmedArtist, seedGenresFolded, ArtistRadio::medianTrackYear(artistTracks));
    if (m_features != nullptr && m_features->isOpen()) {
        seedCandidate.contentGroupId = m_features->contentGroupForPath(representative.path);
    }
    const QVector<TrackScorer::Candidate> anchors{seedCandidate};
    QHash<qint64, QVector<float>> embeddings = radioEmbeddingsForSession(pool, anchors);

    const int persistedExploration = std::clamp(
        m_database->setting(QStringLiteral("radio.exploration"), QString::number(kDefaultRadioExploration)).toInt(),
        0, 100);
    const int exploration = m_radioAdventurous ? kAdventurousExploration : persistedExploration;
    m_radioAdventurous = false;

    m_radioBatchSize = std::clamp(
        m_database->setting(QStringLiteral("radio.batchSize"), QString::number(kDefaultRadioBatchSize)).toInt(),
        1, 100);
    m_radioPickPaths.clear();
    m_radioPickPaths.insert(representative.path);
    m_radioShuffleSessionActive = false;
    m_radioSessionKind = QStringLiteral("artist");
    m_radioSessionSeedPaths.clear();
    m_radioSessionSeedPath.clear();
    m_radioSessionAnchorMode = radioAnchorMode();
    m_radioSessionArtistName = trimmedArtist;
    m_radioSessionExploration = exploration;

    m_radioSessionWeights = radioScoringWeights();
    m_radioSessionDecay = radioSessionDecay();
    if (m_radioSessionAnchorMode == QLatin1String("drift")) {
        m_radioSession = std::make_unique<RadioSession>(
            std::move(pool), std::move(affinities), std::move(genreIdf), anchors,
            RadioSession::ContextMode::MovingContext, exploration, QDateTime::currentSecsSinceEpoch(),
            nullptr, m_radioSessionWeights, std::move(embeddings), m_radioSessionDecay);
    } else {
        m_radioSession = std::make_unique<RadioSession>(
            std::move(pool), std::move(affinities), std::move(genreIdf), seedCandidate,
            exploration, QDateTime::currentSecsSinceEpoch(), nullptr, m_radioSessionWeights,
            std::move(embeddings), m_radioSessionDecay);
    }
    installRadioProvider(/*markPicksAsRadio=*/true);
    m_player->setRadioActive(true);
    m_radioTopUpInProgress = true;
    m_player->clearAll();
    m_player->appendAndPlay(representative);
    m_radioTopUpInProgress = false;
    if (m_radioBatchSize > 1) {
        appendRadioBatch(m_radioBatchSize - 1);
    }
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
    const QHash<QString, QString> resolvedSongKeys = buildResolvedSongKeyMap();
    QVector<TrackScorer::Candidate> pool =
        buildRadioFallbackPool(5000, genreAliases, ignoredRadioGenres, resolvedSongKeys);
    QHash<QString, TrackScorer::Affinity> affinities = buildRadioAffinities(resolvedSongKeys);
    const qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
    pool = RadioMix::filterCandidates(*mixMode, pool, affinities, nowSecs);
    if (pool.isEmpty()) {
        return false;
    }

    const int batchSize = std::clamp(
        m_database->setting(QStringLiteral("radio.batchSize"), QString::number(kDefaultRadioBatchSize)).toInt(),
        1, 100);
    const int persistedExploration = std::clamp(
        m_database->setting(QStringLiteral("radio.exploration"), QString::number(kDefaultRadioExploration)).toInt(),
        0, 100);
    const int exploration = m_radioAdventurous ? kAdventurousExploration : persistedExploration;
    const TrackScorer::Weights sessionWeights = radioScoringWeights();
    const TrackScorer::RadioSessionDecay sessionDecay = radioSessionDecay();
    QHash<qint64, QVector<float>> embeddings = radioEmbeddingsForSession(pool);
    auto session = std::make_unique<RadioSession>(std::move(pool), affinities,
                                                   buildRadioGenreIdf(genreAliases, ignoredRadioGenres),
                                                   exploration, nowSecs, nullptr, sessionWeights,
                                                   std::move(embeddings), sessionDecay);
    const QSet<QString> neverRadioPaths = m_database != nullptr
        ? m_database->flaggedPaths(Database::TrackFlag::NeverRadio)
        : QSet<QString>{};
    QVector<Track> picks = session->nextTracks(batchSize, {}, [this, neverRadioPaths](const QString &path) {
        return resolveRadioPick(path, neverRadioPaths);
    });
    if (picks.isEmpty()) {
        return false;
    }

    ++m_radioRequestId;
    ++m_radioQueueRevision;
    setRadioLoading(false);
    m_radioBatchSize = batchSize;
    m_radioSessionWeights = sessionWeights;
    m_radioSessionDecay = sessionDecay;
    m_radioAdventurous = false;
    m_radioPickPaths.clear();
    m_radioShuffleSessionActive = false;
    m_radioSessionKind = mode.trimmed().toLower();
    m_radioSessionSeedPaths.clear();
    m_radioSessionSeedPath.clear();
    m_radioSessionAnchorMode = QStringLiteral("pinned");
    m_radioSessionArtistName.clear();
    m_radioSessionExploration = exploration;
    m_radioSession = std::move(session);
    recordRadioPicks(picks);
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
    ++m_radioRequestId;
    ++m_radioQueueRevision;
    m_radioTopUpInProgress = false;
    setRadioLoading(false);
    if (m_player != nullptr) {
        m_player->setRadioActive(false);
        m_player->setRadioProvider({});
    }
    m_radioSession.reset();
    m_radioPickPaths.clear();
    m_radioSessionKind.clear();
    m_radioSessionSeedPaths.clear();
    m_radioSessionSeedPath.clear();
    m_radioSessionAnchorMode = QStringLiteral("pinned");
    m_radioSessionArtistName.clear();
    // "Resets when a session ends" -- see setRadioAdventurous's doc comment.
    m_radioAdventurous = false;
    clearRadioSessionState();
    syncRadioShuffleSession();
}

void AppCore::setRadioLoading(bool loading)
{
    if (m_radioLoading == loading) {
        return;
    }
    m_radioLoading = loading;
    emit radioLoadingChanged(loading);
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
        ++m_radioSessionRevision;
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

int AppCore::radioRefillThreshold() const
{
    return m_radioRefillThreshold;
}

void AppCore::setRadioRefillThreshold(int value0To100)
{
    m_radioRefillThreshold = std::clamp(value0To100, 0, 100);
    if (m_database != nullptr) {
        m_database->setSetting(QStringLiteral("radio.refillThreshold"),
                               QString::number(m_radioRefillThreshold));
    }
    maybeTopUpRadioQueue();
}

QString AppCore::radioAnchorMode() const
{
    if (m_database != nullptr
        && m_database->setting(QStringLiteral("radio.anchorMode")) == QLatin1String("drift")) {
        return QStringLiteral("drift");
    }
    return QStringLiteral("pinned");
}

void AppCore::setRadioAnchorMode(const QString &mode)
{
    if (m_database == nullptr) {
        return;
    }
    m_database->setSetting(QStringLiteral("radio.anchorMode"),
                           mode == QLatin1String("drift") ? QStringLiteral("drift")
                                                            : QStringLiteral("pinned"));
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
        ++m_radioSessionRevision;
        saveRadioSessionState();
    }
    // No session yet: just arms the next startRadio (consumed + reset there).
}

void AppCore::appendRadioBatch(int count)
{
    if (!m_radioSession || m_player == nullptr || count <= 0 || m_radioTopUpInProgress) {
        return;
    }
    QSet<QString> exclude;
    const QVector<Track> &queue = m_player->queue();
    exclude.reserve(queue.size());
    for (const Track &track : queue) {
        exclude.insert(track.path);
    }
    const quint64 requestId = m_radioRequestId;
    const quint64 sessionRevision = m_radioSessionRevision;
    const quint64 queueRevision = m_radioQueueRevision;
    auto session = std::make_shared<RadioSession>(*m_radioSession);
    auto candidatePicks = std::make_shared<QVector<Track>>();
    auto *watcher = new QFutureWatcher<void>(this);
    m_radioTopUpInProgress = true;
    setRadioLoading(true);
    connect(watcher, &QFutureWatcher<void>::finished, this,
            [this, watcher, session, candidatePicks, exclude, requestId, sessionRevision, queueRevision]() {
        watcher->deleteLater();
        // A newer radio start owns both the loading indicator and the shared
        // in-progress flag. Its batch may already be running, so an older
        // watcher's completion must not clear either one.
        if (requestId != m_radioRequestId) {
            return;
        }
        m_radioTopUpInProgress = false;
        if (sessionRevision != m_radioSessionRevision || queueRevision != m_radioQueueRevision
            || m_player == nullptr || !m_player->radioActive() || !m_radioSession) {
            if (m_player != nullptr && m_player->radioActive() && m_radioSession) {
                maybeTopUpRadioQueue();
                if (!m_radioTopUpInProgress) {
                    setRadioLoading(false);
                }
            } else {
                setRadioLoading(false);
            }
            return;
        }

        const QSet<QString> neverRadioPaths = m_database != nullptr
            ? m_database->flaggedPaths(Database::TrackFlag::NeverRadio)
            : QSet<QString>{};
        QSet<QString> blockedPaths = neverRadioPaths;
        blockedPaths.unite(exclude);
        QStringList orderedPendingPaths;
        const QVector<Track> &liveQueue = m_player->queue();
        const int firstPendingRow = std::max(0, m_player->queueIndex() + 1);
        for (int row = firstPendingRow; row < liveQueue.size(); ++row) {
            const QString &path = liveQueue.at(row).path;
            if (m_radioPickPaths.contains(path)) {
                orderedPendingPaths.push_back(path);
            }
        }
        QVector<Track> picks;
        picks.reserve(candidatePicks->size());
        for (const Track &candidate : *candidatePicks) {
            const Track resolved = resolveRadioPick(candidate.path, blockedPaths);
            if (resolved.path.isEmpty()) {
                continue;
            }
            session->aliasResolvedPath(candidate.path, resolved.path);
            blockedPaths.insert(resolved.path);
            picks.push_back(resolved);
            orderedPendingPaths.push_back(resolved.path);
        }
        session->retainPendingPaths(orderedPendingPaths);

        m_radioSession = std::make_unique<RadioSession>(*session);
        ++m_radioSessionRevision;
        recordRadioPicks(picks);
        for (const Track &track : picks) {
            m_radioPickPaths.insert(track.path);
        }
        if (!picks.isEmpty()) {
            // Radio picks are queue-only: PlayerCore::injectTracks reuses the
            // single-JIT injection signal without mirroring into a playlist.
            m_player->injectTracks(picks);
        }
        saveRadioSessionState();
        setRadioLoading(false);
    });
    watcher->setFuture(QtConcurrent::run([session, candidatePicks, count, exclude]() {
        *candidatePicks = session->nextTracks(count, exclude, [](const QString &path) {
            Track placeholder;
            placeholder.path = path;
            return placeholder;
        });
    }));
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
    if (remaining > m_radioRefillThreshold) {
        return;
    }
    appendRadioBatch(m_radioBatchSize);
}

bool AppCore::refreshRadioPicksBelow(int queueRow)
{
    if (!m_radioSession || m_player == nullptr || !m_player->radioActive()
        || m_radioTopUpInProgress) {
        return false;
    }

    const QVector<Track> &queue = m_player->queue();
    const int currentIndex = m_player->queueIndex();
    if (queueRow < currentIndex || queueRow < 0 || queueRow >= queue.size()) {
        return false;
    }

    QVector<int> staleRows;
    QStringList stalePaths;
    for (int row = queueRow + 1; row < queue.size(); ++row) {
        if (m_radioPickPaths.contains(queue.at(row).path)) {
            staleRows.push_back(row);
            stalePaths.push_back(queue.at(row).path);
        }
    }
    if (staleRows.isEmpty()) {
        return false;
    }

    for (const QString &path : stalePaths) {
        m_radioPickPaths.remove(path);
    }
    m_player->removeRows(staleRows);
    appendRadioBatch(static_cast<int>(staleRows.size()));
    return true;
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

QString AppCore::resetBackfill(const QString &service)
{
    const QString normalized = service.trimmed().toLower();
    if (normalized != QLatin1String("listenbrainz") && normalized != QLatin1String("lastfm")) {
        return QStringLiteral("unknown-service");
    }
    // Refuse mid-run: the worker owns the live cursor/marker, so clearing it now
    // would race the import. Only one backfill runs at a time (m_backfillRunning
    // is engine-wide), so this covers "a backfill for that service is running".
    if (m_backfillRunning) {
        return QStringLiteral("already-running");
    }
    if (m_listenHistory == nullptr || !m_listenHistory->isOpen()) {
        return QStringLiteral("history-unavailable");
    }
    ScrobbleBackfill::clearCompletedMarker(*m_listenHistory, normalized);
    return QStringLiteral("reset");
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
        m_player->stop();
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
        if (service == QLatin1String("reset")) {
            const QString target = args.value(QStringLiteral("target")).toString().trimmed().toLower();
            const QString result = resetBackfill(target);
            if (result == QLatin1String("unknown-service")) {
                return error(QStringLiteral("scrobble-backfill reset needs a service: listenbrainz or lastfm"));
            }
            if (result == QLatin1String("already-running")) {
                return error(QStringLiteral("cannot reset while a backfill is running; cancel it first"));
            }
            if (result == QLatin1String("history-unavailable")) {
                return error(QStringLiteral("listen history store is unavailable"));
            }
            return QJsonObject{{QStringLiteral("backfill"), result}, {QStringLiteral("service"), target}};
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
    if (command == QLatin1String("start-artist-radio")) {
        const QString artist = args.value(QStringLiteral("artist")).toString().trimmed();
        if (artist.isEmpty()) {
            return error(QStringLiteral("start-artist-radio needs an \"artist\""));
        }
        const bool started = startArtistRadio(artist);
        return QJsonObject{{QStringLiteral("radio"),
                            started ? QStringLiteral("started") : QStringLiteral("unknown-artist")}};
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
