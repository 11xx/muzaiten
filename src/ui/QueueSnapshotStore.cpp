#include "ui/QueueSnapshotStore.h"
#include "ui/MainWindow.h"

#include "Version.h"
#include "app/AppCore.h"
#include "app/AppPaths.h"
#include "core/MusicSort.h"
#include "core/Rating.h"
#include "db/Database.h"
#include "db/PlaylistDatabase.h"
#include "db/SettingsStore.h"
#include "fs/LinkRoot.h"
#include "playback/AudioDeviceControl.h"
#include "player/PlayerCore.h"
#include "playback/PlaybackBackend.h"
#include "mpd/MpdConfig.h"
#include "mpd/MpdImportWorker.h"
#include "ipc/IpcServer.h"
#include "mpris/MprisService.h"
#include "scanner/ArtworkCache.h"
#include "scanner/ScanPipeline.h"
#include "scanner/RatingTagSyncWorker.h"
#include "scrobble/LastFmCredentials.h"
#include "scrobble/LastFmScrobbler.h"
#include "scrobble/ListenBrainzScrobbler.h"
#include "scrobble/ListenHistoryStore.h"
#include "scrobble/ListenTracker.h"
#include "scrobble/ScrobbleBackfill.h"
#include "ui/AlbumGrid.h"
#include "ui/ArtistSidebar.h"
#include "ui/AudioAnalysisDialogs.h"
#include "ui/FileExplorerKeybindings.h"
#include "ui/FileExplorerView.h"
#include "ui/GenreCurationDialog.h"
#include "ui/IdleReleaseController.h"
#include "ui/KeybindingsDialog.h"
#include "ui/LinkRootsDialog.h"
#include "ui/ListeningHistoryDialog.h"
#include "ui/PlayerBar.h"
#include "ui/PanelBorderStyle.h"
#include "ui/PanelOrderDialog.h"
#include "ui/PanelSearchController.h"
#include "ui/PlaybackProfileDialog.h"
#include "ui/PlaybackResumeDialog.h"
#include "ui/QueueKeybindings.h"
#include "ui/QueueScreen.h"
#include "ui/QueueStore.h"
#include "ui/RankingDialog.h"
#include "ui/RightSidebar.h"
#include "ui/PlaylistAddDialog.h"
#include "playlist/PlaylistDropImportWorker.h"
#include "ui/PlaylistImportDialog.h"
#include "ui/PlaylistView.h"
#include "ui/RadioCustomizationDialog.h"
#include "ui/SearchView.h"
#include "ui/ScanController.h"
#include "ui/RatingSyncController.h"
#include "ui/ViewStatePersistence.h"
#include "ui/SemanticSearchDialog.h"
#include "ui/SourceDirectoriesDialog.h"
#include "ui/SplitterPersistence.h"
#include "ui/MainPanelKeybindings.h"
#include "ui/MusicExplorerView.h"
#include "ui/TableNavigationScroll.h"
#include "search/Exclusion.h"
#include "search/RankConfig.h"
#include "search/SearchIndex.h"
#include "ui/TrackPropertiesDialog.h"
#include "ui/TrackTable.h"
#include "ui/UiMetrics.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QEventLoop>
#include <QFileInfo>
#include <QFileDialog>
#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QDateTime>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QLoggingCategory>
#include <QMenu>
#include <QMessageBox>
#include <QCloseEvent>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopedValueRollback>
#include <QShortcut>
#include <QSet>
#include <QSpinBox>
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
#include <cmath>


namespace {
constexpr int kAutomaticSavedQueueLimit = 15;
QString newQueueIdentity()
{
    return QStringLiteral("queue:%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

QString normalizedQueueSourceKind(const QString &kind)
{
    if (kind == QStringLiteral("album") || kind == QStringLiteral("playlist")) {
        return kind;
    }
    return QStringLiteral("queue");
}

bool queueSnapshotIsRadio(const QJsonObject &snapshot)
{
    return snapshot.value(QStringLiteral("source")).toString().trimmed() == QStringLiteral("radio");
}

qint64 queueSnapshotSavedAt(const QJsonObject &snapshot)
{
    qint64 savedAt = snapshot.value(QStringLiteral("savedAt")).toVariant().toLongLong();
    if (savedAt <= 0) {
        savedAt = static_cast<qint64>(snapshot.value(QStringLiteral("savedAt")).toDouble(0));
    }
    return savedAt;
}

QString queueSnapshotKey(const QJsonObject &snapshot)
{
    return QStringLiteral("%1|%2|%3")
        .arg(snapshot.value(QStringLiteral("id")).toString(),
             QString::number(queueSnapshotSavedAt(snapshot)),
             snapshot.value(QStringLiteral("name")).toString());
}

QJsonArray queueBacklogFromRoot(const QJsonObject &root)
{
    QJsonArray backlog;
    QSet<QString> seenIds;
    const auto appendSnapshot = [&backlog, &seenIds](const QJsonObject &snapshot) {
        if (snapshot.isEmpty() || queueSnapshotIsRadio(snapshot)) {
            return;
        }
        const QString id = snapshot.value(QStringLiteral("id")).toString();
        if (!id.isEmpty() && seenIds.contains(id)) {
            return;
        }
        backlog.append(snapshot);
        if (!id.isEmpty()) {
            seenIds.insert(id);
        }
    };

    appendSnapshot(root.value(QStringLiteral("previous")).toObject());
    for (const QJsonValue &value : root.value(QStringLiteral("backlog")).toArray()) {
        appendSnapshot(value.toObject());
    }
    return backlog;
}

QJsonArray radioQueueBacklogFromRoot(const QJsonObject &root)
{
    QJsonArray backlog;
    QSet<QString> seenIds;
    const auto appendSnapshot = [&backlog, &seenIds](QJsonObject snapshot, bool forceRadio) {
        if (snapshot.isEmpty()) {
            return;
        }
        if (forceRadio) {
            snapshot.insert(QStringLiteral("source"), QStringLiteral("radio"));
        } else if (!queueSnapshotIsRadio(snapshot)) {
            return;
        }
        const QString id = snapshot.value(QStringLiteral("id")).toString();
        if (!id.isEmpty() && seenIds.contains(id)) {
            return;
        }
        backlog.append(snapshot);
        if (!id.isEmpty()) {
            seenIds.insert(id);
        }
    };

    for (const QJsonValue &value : root.value(QStringLiteral("radioBacklog")).toArray()) {
        appendSnapshot(value.toObject(), true);
    }
    appendSnapshot(root.value(QStringLiteral("previous")).toObject(), false);
    for (const QJsonValue &value : root.value(QStringLiteral("backlog")).toArray()) {
        appendSnapshot(value.toObject(), false);
    }
    return backlog;
}

QVector<QJsonObject> automaticQueueSnapshotsFromRoot(const QJsonObject &root)
{
    QVector<QJsonObject> snapshots;
    for (const QJsonValue &value : queueBacklogFromRoot(root)) {
        const QJsonObject snapshot = value.toObject();
        if (!snapshot.isEmpty()) {
            snapshots.push_back(snapshot);
        }
    }
    for (const QJsonValue &value : radioQueueBacklogFromRoot(root)) {
        const QJsonObject snapshot = value.toObject();
        if (!snapshot.isEmpty()) {
            snapshots.push_back(snapshot);
        }
    }
    std::stable_sort(snapshots.begin(), snapshots.end(), [](const QJsonObject &left, const QJsonObject &right) {
        return queueSnapshotSavedAt(left) > queueSnapshotSavedAt(right);
    });
    return snapshots;
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
    root.insert(QStringLiteral("missing"), track.missing);
    // Technical fields the playback path needs — codec especially, since the DSD
    // output strategy (native vs PCM) keys off it. Without these a restored queue
    // would lose its codec and silently fall back to PCM for DSD.
    root.insert(QStringLiteral("codec"), track.codec);
    root.insert(QStringLiteral("sampleRateHz"), track.sampleRateHz);
    root.insert(QStringLiteral("bitrateKbps"), track.bitrateKbps);
    root.insert(QStringLiteral("channels"), track.channels);
    root.insert(QStringLiteral("bitDepth"), track.bitDepth);
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
    track.missing = root.value(QStringLiteral("missing")).toBool(false);
    track.codec = root.value(QStringLiteral("codec")).toString();
    // Snapshots written before codec was persisted (and any other gap) fall back
    // to the lower-cased file extension, which is exactly what the scanner stores
    // — so DSD detection recovers immediately even for an old saved queue.
    if (track.codec.isEmpty()) {
        track.codec = QFileInfo(track.path).suffix().toLower();
    }
    track.sampleRateHz = root.value(QStringLiteral("sampleRateHz")).toInt();
    track.bitrateKbps = root.value(QStringLiteral("bitrateKbps")).toInt();
    track.channels = root.value(QStringLiteral("channels")).toInt();
    track.bitDepth = root.value(QStringLiteral("bitDepth")).toInt();
    return track;
}
}

QueueSnapshotStore::QueueSnapshotStore(MainWindow &window) : QObject(&window), m_window(window) {}

void QueueSnapshotStore::loadQueueState()
{
    const QJsonObject root = QJsonDocument::fromJson(m_window.m_state->setting(QStringLiteral("queue.state")).toUtf8()).object();
    const QJsonArray trackValues = root.value(QStringLiteral("tracks")).toArray();
    QVector<Track> tracks;
    tracks.reserve(trackValues.size());
    for (const QJsonValue &value : trackValues) {
        Track track = trackFromJson(value.toObject());
        if (!track.path.isEmpty()) {
            const Track refreshed = m_window.m_database->trackForPath(track.path);
            if (!refreshed.path.isEmpty()) {
                track = refreshed;
            }
            tracks.push_back(track);
        }
    }

    const int savedIndex = root.value(QStringLiteral("index")).toInt(-1);
    m_window.m_player->resetQueue(tracks, savedIndex,
                         root.value(QStringLiteral("playNextInsertIndex")).toInt(savedIndex + 1));
    m_window.m_queueId = root.value(QStringLiteral("queueId")).toString();
    m_window.m_queueSourceKind = normalizedQueueSourceKind(root.value(QStringLiteral("queueSourceKind")).toString(QStringLiteral("queue")));
    m_window.m_queueSourcePlaylistId = root.value(QStringLiteral("queueSourcePlaylistId")).toString().toLongLong();
    if (m_window.m_queueSourcePlaylistId <= 0) {
        m_window.m_queueSourcePlaylistId = static_cast<qint64>(root.value(QStringLiteral("queueSourcePlaylistId")).toDouble(0));
    }
    m_window.m_queueSourceName = root.value(QStringLiteral("queueSourceName")).toString();
    if (m_window.m_player->queue().isEmpty()) {
        m_window.m_queueId.clear();
        m_window.m_queueSourceKind = QStringLiteral("queue");
        m_window.m_queueSourcePlaylistId = 0;
        m_window.m_queueSourceName.clear();
    } else {
        ensureCurrentQueueIdentity();
    }
    // resetQueue() does not emit queueChanged, so push the source-dependent UI
    // (playlist-mirror items, merge gating) directly for the restored queue.
    m_window.refreshQueueSourceDependentUi();
    m_window.m_queueStore->setSnapshot(m_window.m_player->queue(), m_window.m_player->queueIndex(),
                              m_window.m_player->queueIndex() + 1, m_window.m_player->playNextInsertIndex());
    m_window.m_rightSidebar->setCurrentIndex(m_window.m_player->queueIndex(), /*reveal=*/true);
    m_window.refreshPlayNextRange();
    if (m_window.m_player->queueIndex() >= 0) {
        m_window.m_player->presentTrack(m_window.m_player->queue().at(m_window.m_player->queueIndex()));
    }
}

void QueueSnapshotStore::saveQueueState()
{
    if (m_window.m_queueStateSaveTimer != nullptr) {
        m_window.m_queueStateSaveTimer->stop();
    }
    if (!m_window.m_player->queue().isEmpty()) {
        ensureCurrentQueueIdentity();
    }
    QJsonArray tracks;
    for (const Track &track : m_window.m_player->queue()) {
        tracks.append(trackToJson(track));
    }

    QJsonObject root;
    root.insert(QStringLiteral("tracks"), tracks);
    root.insert(QStringLiteral("index"), m_window.m_player->queueIndex());
    root.insert(QStringLiteral("playNextInsertIndex"), m_window.m_player->playNextInsertIndex());
    root.insert(QStringLiteral("queueId"), m_window.m_queueId);
    root.insert(QStringLiteral("queueSourceKind"), m_window.m_queueSourceKind);
    root.insert(QStringLiteral("queueSourcePlaylistId"), QString::number(m_window.m_queueSourcePlaylistId));
    root.insert(QStringLiteral("queueSourceName"), m_window.m_queueSourceName);
    m_window.m_state->setSetting(QStringLiteral("queue.state"), QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

void QueueSnapshotStore::scheduleQueueStateSave(bool immediate)
{
    if (immediate) {
        saveQueueState();
        return;
    }
    if (m_window.m_queueStateSaveTimer != nullptr) {
        m_window.m_queueStateSaveTimer->start();
    }
}

QJsonObject QueueSnapshotStore::queueSnapshotObject(const QString &name, const QString &source) const
{
    QJsonArray tracks;
    for (const Track &track : m_window.m_player->queue()) {
        tracks.append(trackToJson(track));
    }
    QJsonObject snapshot;
    snapshot.insert(QStringLiteral("id"), m_window.m_queueId);
    snapshot.insert(QStringLiteral("name"), name);
    snapshot.insert(QStringLiteral("savedAt"), QDateTime::currentSecsSinceEpoch());
    snapshot.insert(QStringLiteral("index"), m_window.m_player->queueIndex());
    snapshot.insert(QStringLiteral("playNextInsertIndex"), m_window.m_player->playNextInsertIndex());
    snapshot.insert(QStringLiteral("sourceKind"), m_window.m_queueSourceKind);
    snapshot.insert(QStringLiteral("sourcePlaylistId"), QString::number(m_window.m_queueSourcePlaylistId));
    snapshot.insert(QStringLiteral("sourceName"), m_window.m_queueSourceName);
    const QString trimmedSource = source.trimmed();
    if (!trimmedSource.isEmpty()) {
        snapshot.insert(QStringLiteral("source"), trimmedSource);
    }
    snapshot.insert(QStringLiteral("tracks"), tracks);
    return snapshot;
}

QJsonObject QueueSnapshotStore::loadQueueSnapshotsRoot() const
{
    return QJsonDocument::fromJson(m_window.m_state->setting(QStringLiteral("queue.snapshots")).toUtf8()).object();
}

void QueueSnapshotStore::saveQueueSnapshotsRoot(const QJsonObject &root)
{
    m_window.m_state->setSetting(QStringLiteral("queue.snapshots"),
                        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    m_window.refreshSavedQueuePlaylistEntries();
}

QJsonObject QueueSnapshotStore::queueSnapshotByKey(const QString &keyOrId) const
{
    if (keyOrId.isEmpty()) {
        return {};
    }
    const QJsonObject root = loadQueueSnapshotsRoot();
    for (const QJsonObject &snapshot : automaticQueueSnapshotsFromRoot(root)) {
        if (queueSnapshotKey(snapshot) == keyOrId || snapshot.value(QStringLiteral("id")).toString() == keyOrId) {
            return snapshot;
        }
    }
    for (const QJsonValue &value : root.value(QStringLiteral("saved")).toArray()) {
        const QJsonObject snapshot = value.toObject();
        if (queueSnapshotKey(snapshot) == keyOrId || snapshot.value(QStringLiteral("id")).toString() == keyOrId) {
            return snapshot;
        }
    }
    return {};
}

int QueueSnapshotStore::savedQueueLimitSetting() const
{
    return savedQueueUnlimitedSetting() ? 0 : kAutomaticSavedQueueLimit;
}

int QueueSnapshotStore::radioSavedQueueLimitSetting() const
{
    return radioSavedQueueUnlimitedSetting() ? 0 : kAutomaticSavedQueueLimit;
}

bool QueueSnapshotStore::savedQueueUnlimitedSetting() const
{
    const QString value = m_window.m_state->setting(QStringLiteral("queue.savedQueueUnlimited")).trimmed();
    return value == QStringLiteral("1") || value == QStringLiteral("true");
}

bool QueueSnapshotStore::radioSavedQueueUnlimitedSetting() const
{
    const QString value = m_window.m_state->setting(QStringLiteral("queue.radioSavedQueueUnlimited")).trimmed();
    return value == QStringLiteral("1") || value == QStringLiteral("true");
}

void QueueSnapshotStore::ensureCurrentQueueIdentity()
{
    if (m_window.m_queueId.isEmpty()) {
        m_window.m_queueId = newQueueIdentity();
    }
    m_window.m_queueSourceKind = normalizedQueueSourceKind(m_window.m_queueSourceKind);
    if (m_window.m_queueSourceKind != QStringLiteral("playlist")) {
        m_window.m_queueSourcePlaylistId = 0;
    }
}

bool QueueSnapshotStore::currentQueueBacklogEligible() const
{
    return !m_window.m_player->queue().isEmpty() && m_window.m_queueSourceKind == QStringLiteral("queue");
}

void QueueSnapshotStore::pushCurrentQueueToBacklog(const QString &name, const QString &source)
{
    if (!currentQueueBacklogEligible()) {
        return;
    }
    ensureCurrentQueueIdentity();
    QJsonObject root = loadQueueSnapshotsRoot();
    const QString snapshotId = m_window.m_queueId;
    QJsonObject snapshot = queueSnapshotObject(name, source);
    QJsonArray backlog = queueBacklogFromRoot(root);
    QJsonArray radioBacklog = radioQueueBacklogFromRoot(root);
    const bool radioSnapshot = queueSnapshotIsRadio(snapshot);
    const int limit = radioSnapshot ? radioSavedQueueLimitSetting() : savedQueueLimitSetting();
    QJsonArray &targetBacklog = radioSnapshot ? radioBacklog : backlog;
    QJsonArray updatedBacklog;
    updatedBacklog.append(snapshot);
    for (const QJsonValue &value : targetBacklog) {
        const QJsonObject candidate = value.toObject();
        if (candidate.isEmpty() || candidate.value(QStringLiteral("id")).toString() == snapshotId) {
            continue;
        }
        updatedBacklog.append(candidate);
        if (limit > 0 && updatedBacklog.size() >= limit) {
            break;
        }
    }
    targetBacklog = updatedBacklog;
    root.remove(QStringLiteral("previous"));
    root.insert(QStringLiteral("backlog"), backlog);
    root.insert(QStringLiteral("radioBacklog"), radioBacklog);
    saveQueueSnapshotsRoot(root);
}

void QueueSnapshotStore::snapshotCurrentQueueAsPrevious(const QString &source)
{
    pushCurrentQueueToBacklog(QString(), source);
}

void QueueSnapshotStore::markQueueAsSpontaneous(const QString &id)
{
    m_window.m_queueId = id.isEmpty() ? newQueueIdentity() : id;
    m_window.m_queueSourceKind = QStringLiteral("queue");
    m_window.m_queueSourcePlaylistId = 0;
    m_window.m_queueSourceName.clear();
}
