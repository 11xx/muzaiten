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
#include "ui/ScoringWeightsDialog.h"
#include "ui/SearchView.h"
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

Q_LOGGING_CATEGORY(uiLog, "muzaiten.ui")

namespace {

// Album titles are joined into one key string (for persistence and change
// detection) and split back. Use the ASCII Unit Separator (0x1F), which cannot
// occur in a real tag value, so a title that itself contains a newline still
// round-trips to a single filter entry instead of several bogus ones.
constexpr char16_t kAlbumFilterSeparator = u'\x1F';
constexpr int kRootSplitterMinimumTotal = 800;
constexpr int kCenterSplitterMinimumTotal = 300;
constexpr int kArtistSidebarMinimumWidth = 180;
constexpr int kCenterPaneMinimumWidth = 500;
constexpr int kRightSidebarMinimumWidth = 220;
constexpr int kPanelMinimumHeight = 140;
constexpr int kDefaultIdleReleaseSeconds = 60;
constexpr int kDefaultDeepReleaseSeconds = 300;
constexpr int kMaxMemoryReleaseSeconds = 3600;
constexpr int kAutomaticSavedQueueLimit = 15;

QString mainArtistSidebarStyleSheet(const QWidget *widget)
{
    return panelBorderStyleSheet(QStringLiteral("ArtistSidebar#MainArtistSidebar"), panelTopBorder(), widget);
}

QString mainCenterFrameStyleSheet(const QWidget *widget)
{
    return panelBorderStyleSheet(QStringLiteral("QSplitter#MainCenterFrame"),
                                 panelTopBorder(),
                                 widget);
}

QString mainAlbumGridStyleSheet(const QWidget *widget)
{
    return panelBorderStyleSheet(
        QStringLiteral("AlbumGrid#MainAlbumGrid"),
        panelNoBorders(),
        widget);
}

void setStyleSheetIfChanged(QWidget *widget, const QString &style)
{
    if (widget != nullptr && widget->styleSheet() != style) {
        widget->setStyleSheet(style);
    }
}

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

RepeatMode repeatModeFromString(const QString &value)
{
    if (value == QStringLiteral("all")) {
        return RepeatMode::All;
    }
    if (value == QStringLiteral("one")) {
        return RepeatMode::One;
    }
    return RepeatMode::Off;
}

ShuffleMode shuffleModeFromString(const QString &value)
{
    if (value == QStringLiteral("queue")) {
        return ShuffleMode::Queue;
    }
    if (value == QStringLiteral("library")) {
        return ShuffleMode::Library;
    }
    if (value == QStringLiteral("radio")) {
        return ShuffleMode::Radio;
    }
    return ShuffleMode::Off;
}

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

qint64 queueSnapshotSavedAt(const QJsonObject &snapshot)
{
    qint64 savedAt = snapshot.value(QStringLiteral("savedAt")).toVariant().toLongLong();
    if (savedAt <= 0) {
        savedAt = static_cast<qint64>(snapshot.value(QStringLiteral("savedAt")).toDouble(0));
    }
    return savedAt;
}

QString queueSnapshotTimestamp(qint64 timestamp)
{
    if (timestamp <= 0) {
        return {};
    }
    return QDateTime::fromSecsSinceEpoch(timestamp).toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss"));
}

QString queueSnapshotLabel(const QJsonObject &snapshot, const QString &fallback)
{
    const QString name = snapshot.value(QStringLiteral("name")).toString().trimmed();
    if (!name.isEmpty()) {
        return name;
    }
    if (snapshot.value(QStringLiteral("source")).toString().trimmed() == QStringLiteral("radio")) {
        const QString timestamp = queueSnapshotTimestamp(queueSnapshotSavedAt(snapshot));
        if (!timestamp.isEmpty()) {
            return QStringLiteral("Radio session %1").arg(timestamp);
        }
        return QStringLiteral("Radio session");
    }
    return fallback;
}

bool queueSnapshotIsRadio(const QJsonObject &snapshot)
{
    return snapshot.value(QStringLiteral("source")).toString().trimmed() == QStringLiteral("radio");
}

QString defaultSavedQueueName(int ordinal)
{
    return QStringLiteral("saved queue %1").arg(std::max(1, ordinal));
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

void trimQueueBacklog(QJsonArray *backlog, int limit)
{
    if (backlog == nullptr || limit <= 0) {
        return;
    }
    while (backlog->size() > limit) {
        backlog->removeLast();
    }
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

struct QueueSnapshotChoice {
    QJsonObject snapshot;
    QString key;
    QString name;
    QString meta;
};

QueueSnapshotChoice queueSnapshotChoice(const QJsonObject &snapshot, int ordinal)
{
    QueueSnapshotChoice choice;
    choice.snapshot = snapshot;
    choice.key = queueSnapshotKey(snapshot);
    choice.name = queueSnapshotLabel(snapshot, defaultSavedQueueName(ordinal));
    choice.meta = queueSnapshotTimestamp(queueSnapshotSavedAt(snapshot));
    return choice;
}

bool removeQueueSnapshotFromArray(QJsonArray *array, const QString &keyOrId)
{
    if (array == nullptr || keyOrId.isEmpty()) {
        return false;
    }
    for (qsizetype i = 0; i < array->size(); ++i) {
        const QJsonObject snapshot = array->at(i).toObject();
        if (queueSnapshotKey(snapshot) == keyOrId || snapshot.value(QStringLiteral("id")).toString() == keyOrId) {
            array->removeAt(i);
            return true;
        }
    }
    return false;
}

bool removeQueueSnapshotFromRoot(QJsonObject *root, const QString &keyOrId)
{
    if (root == nullptr || keyOrId.isEmpty()) {
        return false;
    }
    QJsonArray backlog = queueBacklogFromRoot(*root);
    QJsonArray radioBacklog = radioQueueBacklogFromRoot(*root);
    const bool removedFromBacklog = removeQueueSnapshotFromArray(&backlog, keyOrId);
    const bool removedFromRadioBacklog = removeQueueSnapshotFromArray(&radioBacklog, keyOrId);
    const bool removedAutomatic = removedFromBacklog || removedFromRadioBacklog;
    if (removedAutomatic) {
        root->remove(QStringLiteral("previous"));
        root->insert(QStringLiteral("backlog"), backlog);
        root->insert(QStringLiteral("radioBacklog"), radioBacklog);
        return true;
    }
    QJsonArray saved = root->value(QStringLiteral("saved")).toArray();
    if (removeQueueSnapshotFromArray(&saved, keyOrId)) {
        root->insert(QStringLiteral("saved"), saved);
        return true;
    }
    const QJsonObject previous = root->value(QStringLiteral("previous")).toObject();
    if (queueSnapshotKey(previous) == keyOrId || previous.value(QStringLiteral("id")).toString() == keyOrId) {
        root->remove(QStringLiteral("previous"));
        return true;
    }
    return false;
}

QVector<QueueSnapshotChoice> queueSnapshotChoicesFromRoot(const QJsonObject &root)
{
    QVector<QueueSnapshotChoice> choices;
    int ordinal = 1;
    for (const QJsonObject &snapshot : automaticQueueSnapshotsFromRoot(root)) {
        if (snapshot.isEmpty()) {
            continue;
        }
        choices.push_back(queueSnapshotChoice(snapshot, ordinal++));
    }
    for (const QJsonValue &value : root.value(QStringLiteral("saved")).toArray()) {
        const QJsonObject snapshot = value.toObject();
        if (snapshot.isEmpty()) {
            continue;
        }
        choices.push_back(queueSnapshotChoice(snapshot, ordinal++));
    }
    return choices;
}

QString nextSavedQueueName(const QJsonObject &root)
{
    QSet<QString> existing;
    for (const QueueSnapshotChoice &choice : queueSnapshotChoicesFromRoot(root)) {
        existing.insert(choice.name);
    }
    for (int i = 1; i <= 999; ++i) {
        const QString candidate = defaultSavedQueueName(i);
        if (!existing.contains(candidate)) {
            return candidate;
        }
    }
    return defaultSavedQueueName(static_cast<int>(existing.size()) + 1);
}

int chooseQueueSnapshot(QWidget *parent,
                        const QString &title,
                        const QString &prompt,
                        const QVector<QueueSnapshotChoice> &choices)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    auto *layout = new QVBoxLayout(&dialog);
    auto *label = new QLabel(prompt, &dialog);
    layout->addWidget(label);

    auto *list = new QListWidget(&dialog);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setWordWrap(true);
    for (int i = 0; i < choices.size(); ++i) {
        const QueueSnapshotChoice &choice = choices.at(i);
        auto *item = new QListWidgetItem(choice.meta.isEmpty()
                                             ? choice.name
                                             : QStringLiteral("%1\n%2").arg(choice.name, choice.meta),
                                         list);
        item->setData(Qt::UserRole, i);
        item->setSizeHint(QSize(360, choice.meta.isEmpty() ? 24 : 42));
    }
    if (list->count() > 0) {
        list->setCurrentRow(0);
    }
    layout->addWidget(list, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
    dialog.resize(420, 320);
    if (dialog.exec() != QDialog::Accepted || list->currentItem() == nullptr) {
        return -1;
    }
    bool indexOk = false;
    const int index = list->currentItem()->data(Qt::UserRole).toInt(&indexOk);
    return indexOk ? index : -1;
}

// Heuristic: does a GStreamer error string read like "the output device is
// busy / can't be opened"? Those are the failures a takeover can fix; other
// errors (decode, file-not-found) must not flash a takeover button.
bool deviceErrorLooksLikeBusy(const QString &error)
{
    static const char *const kBusyMarkers[] = {
        "internal data stream",
        "could not open audio device",
        "device is being used",
        "resource busy",
        "device or resource busy",
        "failed to open",
    };
    const QString lower = error.toLower();
    for (const char *marker : kBusyMarkers) {
        if (lower.contains(QLatin1String(marker)))
            return true;
    }
    return false;
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

    // Cross-mode memory. Fall back to the active fields so profiles saved before
    // these were introduced still seed sensible per-mode defaults.
    const bool sharedActive = profile.mode != QStringLiteral("bit-perfect");
    profile.sharedSink = root.value(QStringLiteral("sharedSink"))
        .toString(sharedActive ? profile.sink : QString());
    profile.sharedSoftwareVolume = root.value(QStringLiteral("sharedSoftwareVolume"))
        .toBool(sharedActive ? profile.softwareVolume : true);
    profile.sharedAllowResample = root.value(QStringLiteral("sharedAllowResample"))
        .toBool(sharedActive ? profile.allowResample : false);
    profile.sharedReleaseSinkOnPause = root.value(QStringLiteral("sharedReleaseSinkOnPause"))
        .toBool(sharedActive ? profile.releaseSinkOnPause : true);
    profile.deviceId = root.value(QStringLiteral("deviceId")).toString(profile.deviceId);
    profile.autoReleaseExclusiveDevice = root.value(QStringLiteral("autoReleaseExclusiveDevice"))
        .toBool(profile.autoReleaseExclusiveDevice);
    profile.autoReleaseTimeoutSec = std::clamp(
        root.value(QStringLiteral("autoReleaseTimeoutSec")).toInt(profile.autoReleaseTimeoutSec), 1, 600);
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
    root.insert(QStringLiteral("sharedSink"), profile.sharedSink);
    root.insert(QStringLiteral("sharedSoftwareVolume"), profile.sharedSoftwareVolume);
    root.insert(QStringLiteral("sharedAllowResample"), profile.sharedAllowResample);
    root.insert(QStringLiteral("sharedReleaseSinkOnPause"), profile.sharedReleaseSinkOnPause);
    root.insert(QStringLiteral("deviceId"), profile.deviceId);
    root.insert(QStringLiteral("autoReleaseExclusiveDevice"), profile.autoReleaseExclusiveDevice);
    root.insert(QStringLiteral("autoReleaseTimeoutSec"), profile.autoReleaseTimeoutSec);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QString mainViewName(MainView view)
{
    switch (view) {
    case MainView::LibraryPanels:
        return QStringLiteral("libraryPanels");
    case MainView::LibraryMusicExplorer:
        return QStringLiteral("libraryMusicExplorer");
    case MainView::LibraryFileExplorer:
        return QStringLiteral("libraryFileExplorer");
    case MainView::FreeRoamFileExplorer:
        return QStringLiteral("freeRoamFileExplorer");
    case MainView::Search:
        return QStringLiteral("search");
    case MainView::Queue:
        return QStringLiteral("queue");
    case MainView::Playlist:
        return QStringLiteral("playlist");
    }
    return QStringLiteral("libraryPanels");
}

MainView mainViewFromName(const QString &name)
{
    if (name == QStringLiteral("libraryMusicExplorer")) {
        return MainView::LibraryMusicExplorer;
    }
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
    if (name == QStringLiteral("playlist")) {
        return MainView::Playlist;
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

static PlaylistItem playlistItemFromTrack(const Track &track, const QString &query);

MainWindow::MainWindow(AppCore *core, QWidget *parent)
    : QMainWindow(parent)
    , m_core(core)
{
    m_player    = m_core->player();
    m_playback  = m_core->backend();
    m_database  = m_core->database();
    m_playlistDb= m_core->playlistDatabase();
    m_state     = m_core->settings();
    m_artworkCache = m_core->artworkCache();
    m_listenHistory= m_core->listenHistory();
    m_listenTracker= m_core->listenTracker();
    m_mpris     = m_core->mpris();
    m_ipc       = m_core->ipc();
    m_listenBrainzScrobbler = m_core->listenBrainzScrobbler();
    m_lastFmScrobbler       = m_core->lastFmScrobbler();
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
    m_rootSplitter->setChildrenCollapsible(false);
    m_artistSidebar = new ArtistSidebar(m_rootSplitter);
    m_artistSidebar->setObjectName(QStringLiteral("MainArtistSidebar"));
    m_artistSidebar->setMinimumWidth(kArtistSidebarMinimumWidth);

    m_libraryCenterStack = new QStackedWidget(m_rootSplitter);
    m_libraryCenterStack->setMinimumWidth(kCenterPaneMinimumWidth);

    m_centerSplitter = new QSplitter(Qt::Vertical, m_libraryCenterStack);
    m_centerSplitter->setObjectName(QStringLiteral("MainCenterFrame"));
    m_centerSplitter->setChildrenCollapsible(false);
    m_centerSplitter->setMinimumWidth(kCenterPaneMinimumWidth);
    m_albumGrid = new AlbumGrid(m_centerSplitter);
    m_albumGrid->setObjectName(QStringLiteral("MainAlbumGrid"));
    m_albumGrid->setMinimumHeight(kPanelMinimumHeight);
    m_trackTable = new TrackTable(m_centerSplitter);
    m_trackTable->setMinimumHeight(kPanelMinimumHeight);
    m_trackTable->setTrackFlagResolver([this](const Track &track, const QString &flag) {
        return m_core->trackFlag(track.path, flag);
    });
    m_centerSplitter->setStretchFactor(0, 55);
    m_centerSplitter->setStretchFactor(1, 45);
    m_centerSplitter->setSizes({540, 430});
    m_libraryCenterStack->addWidget(m_centerSplitter);

    m_rightSidebar = new RightSidebar(m_rootSplitter);
    m_rightSidebar->setObjectName(QStringLiteral("MainRightSidebar"));
    m_rightSidebar->setMinimumWidth(kRightSidebarMinimumWidth);
    m_rightSidebar->setQueueStore(m_queueStore);
    m_rightSidebar->setPickReasonResolver([this](const QString &path) {
        return m_core->radioPickReason(path);
    });
    m_rightSidebar->setTrackFlagResolver([this](const Track &track, const QString &flag) {
        return m_core->trackFlag(track.path, flag);
    });

    m_rootSplitter->addWidget(m_artistSidebar);
    m_rootSplitter->addWidget(m_libraryCenterStack);
    m_rootSplitter->addWidget(m_rightSidebar);
    m_rootSplitter->setStretchFactor(0, 0);
    m_rootSplitter->setStretchFactor(1, 1);
    m_rootSplitter->setStretchFactor(2, 0);
    m_rootSplitter->setSizes({197, 1296, 298});
    restylePanelBorders();

    // The library home stays constructed (it's the default view), but its decoded
    // cover thumbnails are the single biggest heap consumer. Drop them once the
    // user has been on another screen past the idle window, and re-stream them
    // when they return.
    new IdleReleaseController(m_rootSplitter,
                             [this] { m_albumGrid->releaseArtwork(); },
                             [this] { m_albumGrid->reloadArtwork(); },
                             idleReleaseMs());
    // A longer second tier asks SQLite to drop retained artwork-cache pages too.
    // Returning to the library can still re-stream covers, but a long stay on a
    // different screen reclaims more memory than dropping per-cell QIcons alone.
    new IdleReleaseController(m_rootSplitter,
                             [this] {
                                 QMetaObject::invokeMethod(m_artworkCache, "releaseCacheMemory", Qt::QueuedConnection);
                             },
                             {},
                             deepReleaseMs());

    m_panelSearch = new PanelSearchController(central);

    m_mainStack->addWidget(m_rootSplitter);

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

    m_takeOverDeviceButton = new QPushButton(QStringLiteral("Take over device"), this);
    m_takeOverDeviceButton->setVisible(false);
    m_takeOverDeviceButton->setToolTip(
        QStringLiteral("Free the audio device from PipeWire and retry bit-perfect playback"));
    statusBar()->addPermanentWidget(m_takeOverDeviceButton);
    connect(m_takeOverDeviceButton, &QPushButton::clicked, this, [this]() {
        if (m_dsdTakeoverPromptActive) {
            resolveDsdTakeoverPrompt(/*accepted=*/true);
        } else {
            attemptDeviceTakeover();
        }
    });
    m_dsdTakeoverPromptTimer = new QTimer(this);
    m_dsdTakeoverPromptTimer->setInterval(1000);
    connect(m_dsdTakeoverPromptTimer, &QTimer::timeout, this, [this]() {
        --m_dsdTakeoverSecondsRemaining;
        if (m_dsdTakeoverSecondsRemaining <= 0) {
            resolveDsdTakeoverPrompt(/*accepted=*/false);
        } else {
            updateDsdTakeoverPromptText();
        }
    });
    m_takenOverDsdReleaseTimer = new QTimer(this);
    m_takenOverDsdReleaseTimer->setSingleShot(true);
    connect(m_takenOverDsdReleaseTimer, &QTimer::timeout, this, &MainWindow::releaseHeldOutputDevice);

    // PlayerCore, MprisService, and data stores are now owned by AppCore.
    // m_player, m_playback, m_mpris, m_database, etc. are all borrowed pointers
    // assigned from AppCore at the top of this constructor.
    connect(m_player, &PlayerCore::aboutToAddTracks, this, &MainWindow::prepareQueueForTrackAddition);
    connect(m_player, &PlayerCore::aboutToInjectLibraryTrack, this, &MainWindow::prepareQueueForLibraryInjection);
    connect(m_player, &PlayerCore::queueChanged, this, &MainWindow::syncQueueState);
    connect(m_player, &PlayerCore::queueTracksChanged, this, &MainWindow::patchQueueRows);
    connect(m_player, &PlayerCore::currentIndexChanged, this, &MainWindow::onPlayerIndexChanged);
    connect(m_player, &PlayerCore::playNextRangeChanged, this, &MainWindow::refreshPlayNextRange);
    connect(m_player, &PlayerCore::currentTrackChanged, this, &MainWindow::presentTrack);
    connect(m_player, &PlayerCore::currentTrackChanged, this, [this](const Track &track, bool) {
        // Selecting a different playable track invalidates a still-visible DSD
        // question. PlayerCore has already discarded its pending request; this
        // only tears down the stale status-bar affordance.
        if (m_dsdTakeoverPromptActive) {
            m_dsdTakeoverPromptTimer->stop();
            m_dsdTakeoverPromptActive = false;
            m_pendingDsdTakeoverDevice.clear();
            m_takeOverDeviceButton->setVisible(false);
            m_takeOverDeviceButton->setText(QStringLiteral("Take over device"));
        }
        releaseDsdDeviceForPcmTrack(track);
    });
    connect(m_player, &PlayerCore::currentTrackUpdated, this, &MainWindow::presentCurrentTrackUpdate);
    connect(m_player, &PlayerCore::playbackCleared, this, &MainWindow::clearPresentedTrack);
    connect(m_player, &PlayerCore::volumeChanged, this, &MainWindow::applyPlayerVolume);
    // PlayerCore is the source of truth: reflect mode changes on the player bar.
    // AppCore handles MPRIS mirroring and state persistence independently.
    connect(m_player, &PlayerCore::repeatModeChanged, this, [this](RepeatMode mode) {
        m_playerBar->setRepeatMode(mode);
    });
    connect(m_player, &PlayerCore::shuffleModeChanged, this, [this](ShuffleMode mode) {
        m_playerBar->setShuffleMode(mode);
    });
    connect(m_player, &PlayerCore::radioActiveChanged, this, [this](bool active) {
        m_playerBar->setRadioActive(active);
        // Radio replaces the queue with its own session. Sever any playlist
        // backing BEFORE the seed lands: radioActiveChanged fires ahead of the
        // clearAll/appendAndPlay sequence in AppCore::startRadio, and with a
        // playlist-sourced queue still marked, prepareQueueForTrackAddition
        // would mirror the radio seed into that playlist.
        if (active) {
            markQueueAsSpontaneous();
        }
    });
    // Shuffle-percent persistence is handled by AppCore (state + MPRIS).
    connect(m_player, &PlayerCore::trackUnresolvable, this, [this](const Track &track) {
        QMessageBox::warning(this, QStringLiteral("Playback"),
                             QStringLiteral("Could not resolve a readable file for %1").arg(track.path));
    });
    m_player->setPlaybackStartPlanner([this](const Track &track) {
        PlayerCore::PlaybackStartPlan plan;
        if (!isDsdTrack(track)) {
            return plan;
        }

        const PlaybackBackend::DsdSupport support = m_playback->dsdSupport();
        const auto skip = [](const QString &reason) {
            PlayerCore::PlaybackStartPlan rejected;
            rejected.action = PlayerCore::PlaybackStartPlan::Action::Skip;
            rejected.reason = reason;
            return rejected;
        };
        // No DFF/DSDIFF demuxer ships with GStreamer or ffmpeg today, so neither
        // the native passthrough nor the decode-to-PCM path can even parse a .dff
        // container (avdemux_dsf is DSF-only). Skip honestly up front rather than
        // routing it to playbin, which would only surface a decode error — and
        // rather than implying resampling could rescue it, which it cannot.
        if (track.codec.compare(QStringLiteral("dff"), Qt::CaseInsensitive) == 0) {
            return skip(QStringLiteral("DFF/DSDIFF playback isn't supported by the installed GStreamer plugins (DSF only)"));
        }

        // Shared output with resampling is deliberately the straightforward
        // decode-to-PCM path: playbin chooses the installed DSD decoder and no
        // device takeover or skip is involved.
        if (m_playbackProfile.mode != QStringLiteral("bit-perfect")
            && m_playbackProfile.allowResample) {
            if (support.pcmDecode) {
                return plan;
            }
            return skip(QStringLiteral("DSD PCM playback needs gst-plugins-bad and gst-libav"));
        }
        if (!support.nativePassthrough) {
            return skip(QStringLiteral("Native DSD playback needs gst-plugins-bad and gst-libav"));
        }

        QString device = m_playbackProfile.device;
        if (m_playbackProfile.mode != QStringLiteral("bit-perfect")) {
            if (m_playbackProfile.deviceId.isEmpty()) {
                return skip(QStringLiteral("Choose a direct output device in Playback → Output profile before native DSD playback"));
            }
            const auto dev = AudioDeviceControl::findByStableId(m_playbackProfile.deviceId);
            if (!dev || dev->hwPath.isEmpty()) {
                return skip(QStringLiteral("The configured native DSD device is no longer available"));
            }
            device = dev->hwPath;
            plan.device = device;
            plan.action = dev->heldByPipeWire()
                ? PlayerCore::PlaybackStartPlan::Action::DeferForDsdTakeover
                : PlayerCore::PlaybackStartPlan::Action::NativeDsd;
            return plan;
        }

        if (device.isEmpty()) {
            return skip(QStringLiteral("Choose a bit-perfect output device before native DSD playback"));
        }
        plan.action = PlayerCore::PlaybackStartPlan::Action::NativeDsd;
        plan.device = device;
        return plan;
    });
    connect(m_player, &PlayerCore::dsdTakeoverRequested, this, &MainWindow::showDsdTakeoverPrompt);
    connect(m_player, &PlayerCore::trackStartSkipped, this, [this](const Track &track, const QString &reason) {
        const QString title = track.title.isEmpty() ? track.filename : track.title;
        statusBar()->showMessage(reason.isEmpty()
                                     ? QStringLiteral("Skipped %1").arg(title)
                                     : QStringLiteral("Skipped %1: %2").arg(title, reason),
                                 8000);
    });
    m_playbackStateSaveTimer = new QTimer(this);
    m_playbackStateSaveTimer->setSingleShot(true);
    m_playbackStateSaveTimer->setInterval(2000);
    connect(m_playbackStateSaveTimer, &QTimer::timeout, this, [this]() {
        savePlaybackState();
    });
    m_queueStateSaveTimer = new QTimer(this);
    m_queueStateSaveTimer->setSingleShot(true);
    m_queueStateSaveTimer->setInterval(500);
    connect(m_queueStateSaveTimer, &QTimer::timeout, this, [this]() {
        saveQueueState();
    });

    // m_mpris->setDatabase(...) was moved to AppCore constructor.
    // m_rightSidebar still needs the database for track enrichment:
    if (m_rightSidebar) {
        m_rightSidebar->setDatabase(m_database);
    }

    connect(m_artworkCache, &ArtworkCache::artworkReady, this, &MainWindow::onArtworkReady);
    connect(m_artworkCache, &ArtworkCache::artworkMissing, this, &MainWindow::onArtworkMissing);

    // scrobbler signal connections TO the UI (statusBar, QMessageBox).
    // The workers and threads themselves are owned by AppCore.
    connect(m_listenBrainzScrobbler, &ListenBrainzScrobbler::submissionFailed, this, [this](const QString &message) {
        statusBar()->showMessage(message, 10000);
    });
    connect(m_listenBrainzScrobbler, &ListenBrainzScrobbler::backlogProcessed, this, [this](int sent, int skipped, int remaining) {
        statusBar()->showMessage(QStringLiteral("ListenBrainz processed %1 listens (%2 skipped, %3 pending)")
                                     .arg(sent)
                                     .arg(skipped)
                                     .arg(remaining),
                                 6000);
        updateScrobbleBacklogActions();
    });
    connect(m_listenBrainzScrobbler, &ListenBrainzScrobbler::disabledAfterFailures, this, [this](const QString &message) {
        m_database->setSetting(QStringLiteral("listenbrainz.enabled"), QStringLiteral("false"));
        m_playerBar->setListenBrainzEnabled(false);
        statusBar()->showMessage(message, 15000);
        QMessageBox::warning(this, QStringLiteral("ListenBrainz"), message);
    });
    connect(m_listenBrainzScrobbler, &ListenBrainzScrobbler::tokenValidated, this, [this](bool valid, const QString &username) {
        statusBar()->showMessage(valid ? QStringLiteral("ListenBrainz token valid — connected as %1").arg(username)
                                       : QStringLiteral("ListenBrainz token is invalid."),
                                 8000);
    });

    connect(m_lastFmScrobbler, &LastFmScrobbler::submissionFailed, this, [this](const QString &message) {
        statusBar()->showMessage(message, 10000);
    });
    connect(m_lastFmScrobbler, &LastFmScrobbler::backlogProcessed, this, [this](int sent, int skipped, int remaining) {
        statusBar()->showMessage(QStringLiteral("Last.fm processed %1 scrobbles (%2 skipped, %3 pending)")
                                     .arg(sent)
                                     .arg(skipped)
                                     .arg(remaining),
                                 6000);
        updateScrobbleBacklogActions();
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

    connect(m_artistSidebar, &ArtistSidebar::artistSelected, this, &MainWindow::selectArtist);
    connect(m_artistSidebar, &ArtistSidebar::artistPlayRequested, this, &MainWindow::playArtistReplacingQueue);
    connect(m_artistSidebar, &ArtistSidebar::artistAddToQueueRequested, this, &MainWindow::addArtistToQueue);
    connect(m_artistSidebar, &ArtistSidebar::startArtistRadioRequested,
            this, &MainWindow::startArtistRadio);
    connect(m_stopScanButton, &QPushButton::clicked, this, &MainWindow::cancelScan);
    connect(m_artistSidebar, &ArtistSidebar::librarySourceChanged, this, &MainWindow::onLibrarySourceChanged);
    connect(m_trackTable, &TrackTable::trackActivated, this, &MainWindow::appendAndPlayTrack);
    connect(m_trackTable, &TrackTable::playNextRequested, this, [this](const QVector<Track> &tracks) {
        enqueueTracksFromMenu(tracks, QueueAddMode::PlayNext, false);
    });
    connect(m_trackTable, &TrackTable::addToQueueRequested, this, [this](const QVector<Track> &tracks) {
        enqueueTracksFromMenu(tracks, QueueAddMode::Append, false);
    });
    connect(m_trackTable, &TrackTable::playNextTemporaryRequested, this, [this](const QVector<Track> &tracks) {
        enqueueTracksFromMenu(tracks, QueueAddMode::PlayNext, true);
    });
    connect(m_trackTable, &TrackTable::addToQueueTemporaryRequested, this, [this](const QVector<Track> &tracks) {
        enqueueTracksFromMenu(tracks, QueueAddMode::Append, true);
    });
    connect(m_trackTable, &TrackTable::trackRatingChanged, this, [this](const Track &track, int rating) {
        applyTrackRating(track, rating, QStringLiteral("track_table"));
    });
    connect(m_trackTable, &TrackTable::trackFlagChanged, this, &MainWindow::applyTrackFlag);
    connect(m_trackTable, &TrackTable::viewSettingsChanged, this, &MainWindow::saveTrackTableViewSettings);
    connect(m_albumGrid, &AlbumGrid::albumSelectionToggled, this, &MainWindow::selectAlbumFilter);
    connect(m_albumGrid, &AlbumGrid::albumSelectionCleared, this, &MainWindow::clearAlbumFilter);
    connect(m_albumGrid, &AlbumGrid::albumSelectionNarrowRequested, this, &MainWindow::narrowAlbumFilters);
    connect(m_albumGrid, &AlbumGrid::albumNarrowFollowRequested, this, &MainWindow::setAlbumNarrowFromGrid);
    connect(m_albumGrid, &AlbumGrid::albumPlayNextRequested, this, &MainWindow::playNextAlbum);
    connect(m_albumGrid, &AlbumGrid::albumPlayReplaceRequested, this, &MainWindow::playAlbumsReplacingQueue);
    connect(m_albumGrid, &AlbumGrid::albumAddToQueueRequested, this, &MainWindow::addAlbumToQueue);
    connect(m_albumGrid, &AlbumGrid::albumPlayNextTemporaryRequested, this, &MainWindow::playNextAlbumTemporary);
    connect(m_albumGrid, &AlbumGrid::albumAddToQueueTemporaryRequested, this, &MainWindow::addAlbumToQueueTemporary);
    connect(m_albumGrid, &AlbumGrid::albumStartRadioRequested, this, &MainWindow::startRadioFromAlbum);
    connect(m_albumGrid, &AlbumGrid::albumAddToPlaylistRequested, this, [this](const QStringList &titles) {
        QVector<Track> tracks;
        for (const QString &title : titles) {
            const QVector<Track> albumTracks = m_librarySource == LibrarySource::Mpd
                ? m_database->mpdTracksForArtist(m_currentArtist, mpdMusicDirectory(), title)
                : m_database->tracksForArtist(m_currentArtist, title);
            tracks.append(albumTracks);
        }
        openAddToPlaylistDialog(tracks);
    });
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
    m_panelSearch->registerTarget(libraryAlbumsPanelTarget());
    m_panelSearch->registerTarget(libraryTracksPanelTarget());
    connect(m_panelSearch, &PanelSearchController::statusMessage, this, [this](const QString &message, int timeoutMs) {
        statusBar()->showMessage(message, timeoutMs);
    });
    connect(m_panelSearch, &PanelSearchController::activePanelChanged, this, [this](MainPanelId) {
        saveMainWindowViewSettings();
    });
    connect(m_rootSplitter, &QSplitter::splitterMoved, this, [this]() {
        saveMainWindowViewSettings(/*captureSplitterSizes=*/true);
    });
    connect(m_centerSplitter, &QSplitter::splitterMoved, this, [this]() {
        saveMainWindowViewSettings(/*captureSplitterSizes=*/true);
    });
    connect(m_rightSidebar, &RightSidebar::queueTrackActivated, this, [this](int index) {
        playQueueIndex(index, /*notifyScrobbler=*/true, /*startPaused=*/false, /*explicitJump=*/true);
    });
    connect(m_rightSidebar, &RightSidebar::queueTrackRatingChanged, this, [this](const Track &track, int rating) {
        applyTrackRating(track, rating, QStringLiteral("right_sidebar"));
    });
    connect(m_rightSidebar, &RightSidebar::queueRowsMoveRequested, this, &MainWindow::moveQueueRows);
    connect(m_rightSidebar, &RightSidebar::queueRowsRemoveRequested, this, &MainWindow::removeQueueRows);
    connect(m_rightSidebar, &RightSidebar::removeAllMissingTracksRequested, this, &MainWindow::removeMissingTracks);
    connect(m_rightSidebar, &RightSidebar::queueClearRequested, this, &MainWindow::clearQueue);
    connect(m_rightSidebar, &RightSidebar::clearPlayNextPriorityRequested, this, &MainWindow::clearPlayNextPriority);
    // MPRIS transport, IPC, and tray are handled by AppCore.
    connect(m_playerBar, &PlayerBar::openLibraryRequested, this, &MainWindow::openLibraryFolder);
    connect(m_playerBar, &PlayerBar::sourceDirectoriesRequested, this, &MainWindow::configureSourceDirectories);
    connect(m_playerBar, &PlayerBar::scanEnabledSourcesRequested, this, &MainWindow::scanEnabledSourceDirectories);
    connect(m_playerBar, &PlayerBar::forceRescanRequested, this, &MainWindow::forceRescanEnabledSourceDirectories);
    connect(m_playerBar, &PlayerBar::scanProfileChanged, this, [this](int profile) {
        static const char *const names[3] = {"background", "balanced", "turbo"};
        if (profile >= 0 && profile <= 2) {
            m_state->setSetting(QStringLiteral("scan.profile"), QString::fromLatin1(names[profile]));
        }
    });
    m_playerBar->setScanProfile(scanProfileSetting());
    connect(m_playerBar, &PlayerBar::analysisPowerChanged, this, [this](int power) {
        static const char *const names[3] = {"background", "balanced", "turbo"};
        if (power < 0 || power > 2) {
            return;
        }
        const QString value = QString::fromLatin1(names[power]);
        const QString previous = m_state->setting(QStringLiteral("analysis.power"), QStringLiteral("background"));
        m_state->setSetting(QStringLiteral("analysis.power"), value);
        // Apply live: stop the running scan (completed work is committed as
        // it streams) and restart it at the new power once it exits.
        if (value != previous && m_audioAnalysisProcess != nullptr) {
            m_audioAnalysisRestartPending = true;
            statusBar()->showMessage(QStringLiteral("Applying %1 analysis power — restarting analysis...").arg(value), 6000);
            cancelAudioAnalysis();
        }
    });
    m_playerBar->setAnalysisPower(analysisPowerSetting());
    m_database->setGuessedPlaceholdersVisible(guessedPlaceholdersEnabled());
    m_playerBar->setShowGuessedPlaceholders(guessedPlaceholdersEnabled());
    connect(m_playerBar, &PlayerBar::showGuessedPlaceholdersChanged, this, [this](bool show) {
        m_state->setSetting(QStringLiteral("scan.guessedPlaceholders"), show ? QStringLiteral("1") : QStringLiteral("0"));
        m_database->setGuessedPlaceholdersVisible(show);
        if (m_librarySource == LibrarySource::Local) {
            refreshArtists();  // apply the new visibility to the browse immediately
        }
    });
    connect(m_playerBar, &PlayerBar::removeMissingTracksRequested, this, &MainWindow::removeMissingTracks);
    connect(m_playerBar, &PlayerBar::listUnsupportedFilesChanged, this, [this](bool show) {
        if (m_freeRoamFileExplorer != nullptr) {
            m_freeRoamFileExplorer->setShowUnsupportedFiles(show);
        }
        if (m_libraryFileExplorer != nullptr) {
            m_libraryFileExplorer->setShowUnsupportedFiles(show);
        }
        m_state->setSetting(QStringLiteral("fileExplorer.showUnsupported"), show ? QStringLiteral("true") : QStringLiteral("false"));
    });
    connect(m_playerBar, &PlayerBar::syncCurrentTrackRatingTagsRequested, this, &MainWindow::syncCurrentTrackRatingTags);
    connect(m_playerBar, &PlayerBar::syncCurrentArtistRatingTagsRequested, this, &MainWindow::syncCurrentArtistRatingTags);
    connect(m_playerBar, &PlayerBar::syncAllSavedRatingTagsRequested, this, &MainWindow::syncAllSavedRatingTags);
    connect(m_playerBar, &PlayerBar::retryPendingRatingTagsRequested, this, &MainWindow::retryPendingRatingTags);
    connect(m_playerBar, &PlayerBar::currentTrackLibraryRequested, this, &MainWindow::jumpToPlayingSong);
    connect(m_playerBar, &PlayerBar::currentTrackFileRequested, this, [this]() {
        const Track current = m_player->currentTrack();
        if (!current.path.isEmpty()) {
            findTrackFile(current);
        }
    });
    connect(m_playerBar, &PlayerBar::playbackProfileRequested, this, &MainWindow::configurePlaybackProfile);
    connect(m_playerBar, &PlayerBar::playbackResumeRequested, this, &MainWindow::configurePlaybackResume);
    connect(m_playerBar, &PlayerBar::releaseDeviceRequested, this, &MainWindow::releaseHeldOutputDevice);
    connect(m_playerBar, &PlayerBar::playbackMenuAboutToShow, this, [this]() {
        m_playerBar->setReleaseDeviceVisible(canReleaseOutputDevice());
    });
    connect(m_playerBar, &PlayerBar::linkRootsRequested, this, &MainWindow::configureLinkRoots);
    connect(m_playerBar, &PlayerBar::scoringWeightsRequested, this, &MainWindow::showScoringWeights);
    connect(m_playerBar, &PlayerBar::genreCurationRequested, this, &MainWindow::showGenreCuration);
    connect(m_playerBar, &PlayerBar::audioAnalysisStartRequested, this, &MainWindow::startAudioAnalysis);
    connect(m_playerBar, &PlayerBar::audioAnalysisCancelRequested, this, &MainWindow::cancelAudioAnalysis);
    connect(m_playerBar, &PlayerBar::analysisStatusRequested, this, &MainWindow::showAnalysisStatus);
    connect(m_playerBar, &PlayerBar::duplicateCopiesRequested, this, &MainWindow::showDuplicateCopies);
    connect(m_playerBar, &PlayerBar::mpdSourceRequested, this, &MainWindow::configureMpdSource);
    connect(m_playerBar, &PlayerBar::mpdImportRequested, this, &MainWindow::importMpdLibraryMetadata);
    connect(m_playerBar, &PlayerBar::listeningHistoryRequested, this, &MainWindow::showListeningHistory);
    connect(m_playerBar, &PlayerBar::scrobblersMenuAboutToShow, this, &MainWindow::updateScrobbleBacklogActions);
    connect(m_playerBar, &PlayerBar::lastFmBacklogClearRequested, this, [this]() {
        clearScrobbleBacklog(ListenHistoryStore::LastFm);
    });
    connect(m_playerBar, &PlayerBar::listenBrainzBacklogClearRequested, this, [this]() {
        clearScrobbleBacklog(ListenHistoryStore::ListenBrainz);
    });
    connect(m_playerBar, &PlayerBar::backfillStartRequested, this, [this](const QString &service) {
        const QString result = m_core->startBackfill(service);
        QString message;
        if (result == QLatin1String("already-running")) {
            message = QStringLiteral("A scrobble backfill is already running");
        } else if (result == QLatin1String("missing-credentials")) {
            message = service == QLatin1String("listenbrainz")
                ? QStringLiteral("Set a ListenBrainz token first")
                : QStringLiteral("Set Last.fm credentials first");
        } else if (result == QLatin1String("unknown-service")) {
            message = QStringLiteral("Unknown backfill service \"%1\"").arg(service);
        }
        if (!message.isEmpty()) {
            statusBar()->showMessage(message, 5000);
        }
    });
    connect(m_playerBar, &PlayerBar::backfillCancelRequested, this, [this]() {
        m_core->cancelBackfill();
        statusBar()->showMessage(QStringLiteral("Cancelling scrobble backfill..."), 4000);
    });
    connect(m_core, &AppCore::backfillStatusChanged, this, [this]() {
        const AppCore::BackfillStatus status = m_core->backfillStatus();
        // A running->idle transition is the finished/failed outcome: surface it
        // as a transient toast exactly once (the menu shows the same text as a
        // persistent "last outcome" line until the next run starts).
        if (m_backfillWasRunning && !status.running && !status.lastMessage.isEmpty()) {
            statusBar()->showMessage(status.lastMessage, 6000);
        }
        m_backfillWasRunning = status.running;
        updateBackfillStatusDisplay();
    });
    connect(m_playerBar, &PlayerBar::compactMenuChanged, this, &MainWindow::applyCompactMenu);
    connect(m_playerBar, &PlayerBar::alwaysShowTrayChanged, this, [this](bool enabled) {
        m_core->setTrayAlwaysVisible(enabled);
    });
    connect(m_playerBar, &PlayerBar::trackInfoPaneVisibleChanged, this, &MainWindow::applyTrackInfoPaneVisible);
    connect(m_playerBar, &PlayerBar::trackInfoPaneSettingsRequested, this, &MainWindow::configureTrackInfoPanel);
    connect(m_playerBar, &PlayerBar::albumArtResolutionRequested, this, &MainWindow::configureAlbumArtResolution);
    connect(m_playerBar, &PlayerBar::playlistMetadataDisplayRequested, this, &MainWindow::configurePlaylistMetadataDisplay);
    connect(m_playerBar, &PlayerBar::searchRankingRequested, this, &MainWindow::configureSearchRanking);
    connect(m_playerBar, &PlayerBar::memoryReclaimRequested, this, &MainWindow::configureMemoryReclaim);
    connect(m_playerBar, &PlayerBar::keybindingsRequested, this, &MainWindow::configureKeybindings);
    connect(m_playerBar, &PlayerBar::resetPanelOrderRequested, this, &MainWindow::resetPanelOrder);
    connect(m_playerBar, &PlayerBar::resetViewPreferencesRequested, this, &MainWindow::resetViewPreferences);
    connect(m_playerBar, &PlayerBar::panelOrderRequested, this, &MainWindow::openPanelOrderDialog);
    connect(m_playerBar, &PlayerBar::queueViewRequested, this, [this]() { switchMainView(MainView::Queue); });
    connect(m_playerBar, &PlayerBar::queueClearRequested, this, &MainWindow::clearQueue);
    connect(m_playerBar, &PlayerBar::queueClearPlayNextPriorityRequested, this, &MainWindow::clearPlayNextPriority);
    connect(m_playerBar, &PlayerBar::queueSaveAsRequested, this, &MainWindow::saveCurrentQueueAs);
    connect(m_playerBar, &PlayerBar::queueRestorePreviousRequested, this, &MainWindow::restorePreviousQueue);
    connect(m_playerBar, &PlayerBar::queueMergeSavedRequested, this, &MainWindow::mergeSavedQueueViaPlayNext);
    connect(m_playerBar, &PlayerBar::queueSavedLimitRequested, this, &MainWindow::configureSavedQueueLimit);
    connect(m_playerBar, &PlayerBar::stopRadioRequested, this, [this]() {
        m_core->stopRadio();
        statusBar()->showMessage(QStringLiteral("Radio stopped — queue kept"), 4000);
    });
    connect(m_playerBar, &PlayerBar::radioMenuAboutToShow, this, [this]() {
        m_playerBar->setRadioAdventurous(m_core->radioAdventurous());
    });
    connect(m_playerBar, &PlayerBar::radioAdventurousChanged, this, [this](bool on) {
        m_core->setRadioAdventurous(on);
    });
    connect(m_playerBar, &PlayerBar::radioExplorationSettingsRequested, this, [this]() {
        bool ok = false;
        const int percent = QInputDialog::getInt(
            this, QStringLiteral("Radio exploration"),
            QStringLiteral("How far radio strays from the seed's mood (0 = conservative, 100 = adventurous):"),
            m_core->radioExploration(), 0, 100, 5, &ok);
        if (ok) {
            m_core->setRadioExploration(percent, /*persist=*/true);
        }
    });
    connect(m_playerBar, &PlayerBar::radioBatchSizeSettingsRequested, this, [this]() {
        bool ok = false;
        const int size = QInputDialog::getInt(
            this, QStringLiteral("Radio batch size"),
            QStringLiteral("Picks to queue ahead at a time (1 = generate one at a time, as needed):"),
            m_core->radioBatchSize(), 1, 100, 1, &ok);
        if (ok) {
            m_core->setRadioBatchSize(size);
        }
    });
    connect(m_playerBar, &PlayerBar::startRadioFromCurrentRequested, this, [this]() {
        const Track current = m_player->currentTrack();
        if (current.path.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("Start radio: nothing is playing"), 4000);
            return;
        }
        startRadioFromSeed(current.path);
    });
    connect(m_playerBar, &PlayerBar::startArtistRadioFromCurrentRequested, this, [this]() {
        const Track current = m_player->currentTrack();
        if (current.artistName.trimmed().isEmpty()) {
            statusBar()->showMessage(QStringLiteral("Start artist radio: nothing is playing"), 4000);
            return;
        }
        startArtistRadio(current.artistName);
    });
    connect(m_playerBar, &PlayerBar::aboutRequested, this, [this]() {
        QMessageBox::about(
            this, QStringLiteral("About muzaiten"),
            QStringLiteral("<b>muzaiten</b> %1<br/>"
                           "A local-first music player with an offline recommendation engine.<br/>"
                           "Released into the public domain under the Unlicense.")
                .arg(QCoreApplication::applicationVersion().toHtmlEscaped()));
    });
    connect(m_playerBar, &PlayerBar::rediscoveryMixRequested, this, [this]() {
        startMix(QStringLiteral("rediscovery"));
    });
    connect(m_playerBar, &PlayerBar::deepCutsMixRequested, this, [this]() {
        startMix(QStringLiteral("deepcuts"));
    });
    connect(m_playerBar, &PlayerBar::playlistViewRequested, this, [this]() { switchMainView(MainView::Playlist); });
    connect(m_playerBar, &PlayerBar::playlistNewRequested, this, [this]() {
        switchMainView(MainView::Playlist);
        ensurePlaylistView()->createPlaylist();
    });
    connect(m_playerBar, &PlayerBar::listenBrainzEnabledChanged, this, &MainWindow::setListenBrainzEnabled);
    connect(m_playerBar, &PlayerBar::listenBrainzTokenRequested, this, &MainWindow::setListenBrainzToken);
    connect(m_playerBar, &PlayerBar::lastFmEnabledChanged, this, &MainWindow::setLastFmEnabled);
    connect(m_playerBar, &PlayerBar::lastFmSettingsRequested, this, &MainWindow::showLastFmSettings);
    connect(m_playerBar, &PlayerBar::scrobbleOfflineChanged, this, &MainWindow::setScrobbleOffline);
    connect(m_playerBar, &PlayerBar::previousRequested, this, &MainWindow::playPreviousTrack);
    connect(m_playerBar, &PlayerBar::playPauseRequested, this, &MainWindow::togglePlayback);
    connect(m_playerBar, &PlayerBar::nextRequested, this, &MainWindow::playNextTrack);
    connect(m_playerBar, &PlayerBar::repeatModeChangeRequested, this, [this](RepeatMode mode) {
        m_player->setRepeatMode(mode);
    });
    connect(m_playerBar, &PlayerBar::shuffleModeChangeRequested, this, [this](ShuffleMode mode) {
        m_player->setShuffleMode(mode);
    });
    connect(m_playerBar, &PlayerBar::libraryShuffleSettingsRequested, this, [this]() {
        bool ok = false;
        const int percent = QInputDialog::getInt(
            this, QStringLiteral("Library shuffle"),
            QStringLiteral("Chance to pull a track from the whole library on each advance (%):"),
            m_player->libraryShufflePercent(), 0, 100, 5, &ok);
        if (ok) {
            m_player->setLibraryShufflePercent(percent);
        }
    });
    connect(m_playerBar, &PlayerBar::radioShuffleSettingsRequested, this, [this]() {
        bool ok = false;
        const int percent = QInputDialog::getInt(
            this, QStringLiteral("Radio shuffle"),
            QStringLiteral("Chance to pull a radio pick from the library on each advance (%):"),
            m_player->radioShufflePercent(), 0, 100, 5, &ok);
        if (ok) {
            m_player->setRadioShufflePercent(percent);
        }
    });
    connect(m_playerBar, &PlayerBar::stopRequested, m_playback, &PlaybackBackend::stop);
    connect(m_playerBar, &PlayerBar::seekRequested, m_playback, &PlaybackBackend::seek);
    connect(m_playerBar, &PlayerBar::volumeChanged, this, [this](int volume) {
        const int clamped = std::clamp(volume, 0, 100);
        m_player->setVolume(static_cast<double>(clamped) / 100.0);
    });
    connect(m_playerBar, &PlayerBar::currentTrackRatingChanged, this, [this](int rating) {
        if (!m_player->currentTrack().path.isEmpty()) {
            applyTrackRating(m_player->currentTrack(), rating, QStringLiteral("player_bar"));
        }
    });
    connect(m_trackTable, &TrackTable::findFileRequested, this, &MainWindow::findTrackFile);
    connect(m_trackTable, &TrackTable::propertiesRequested, this, &MainWindow::showTrackProperties);
    connect(m_trackTable, &TrackTable::addToPlaylistRequested, this, &MainWindow::openAddToPlaylistDialog);
    connect(m_trackTable, &TrackTable::startRadioRequested, this, [this](const Track &track) {
        startRadioFromSeed(track.path);
    });
    connect(m_trackTable, &TrackTable::startArtistRadioRequested, this, &MainWindow::startArtistRadio);
    connect(m_rightSidebar, &RightSidebar::findFileRequested, this, &MainWindow::findTrackFile);
    connect(m_rightSidebar, &RightSidebar::propertiesRequested, this, &MainWindow::showTrackProperties);
    connect(m_rightSidebar, &RightSidebar::startRadioRequested, this, [this](const Track &track) {
        startRadioFromSeed(track.path);
    });
    connect(m_rightSidebar, &RightSidebar::trackFlagChanged, this, &MainWindow::applyTrackFlag);
    connect(m_rightSidebar, &RightSidebar::saveQueueAsRequested, this, &MainWindow::saveCurrentQueueAs);
    connect(m_rightSidebar, &RightSidebar::restorePreviousQueueRequested, this, &MainWindow::restorePreviousQueue);
    connect(m_rightSidebar, &RightSidebar::unlinkQueueFromPlaylistRequested, this, &MainWindow::unlinkQueueFromPlaylist);
    connect(m_rightSidebar, &RightSidebar::trackLibraryRequested, this, &MainWindow::revealTrackInLibrary);
    connect(m_rightSidebar, &RightSidebar::artistRequested, this, &MainWindow::jumpToTrackInfoArtist);
    connect(m_rightSidebar, &RightSidebar::albumRequested, this, &MainWindow::jumpToTrackInfoAlbum);
    connect(m_playback, &PlaybackBackend::positionChanged, this, &MainWindow::updatePlaybackPosition);
    connect(m_playback, &PlaybackBackend::durationChanged, this, &MainWindow::updatePlaybackPosition);
    connect(m_playback, &PlaybackBackend::stateChanged, this, [this](PlaybackBackend::State state) {
        const bool playing = state == PlaybackBackend::State::Playing;
        m_playerBar->setPlaying(playing);
        // AppCore handles MPRIS, listen-tracker, and scrobbler state mirroring.
        schedulePlaybackStateSave(state != PlaybackBackend::State::Playing);
        if (state != PlaybackBackend::State::Error)
            m_takeOverDeviceButton->setVisible(false);
        // Auto-release of a held card is opt-in (a takeover is deliberate). When
        // enabled, hand it back after playback goes idle for the configured time;
        // otherwise it stays ours until released explicitly.
        if (canReleaseOutputDevice()) {
            if (state == PlaybackBackend::State::Playing) {
                m_takenOverDsdReleaseTimer->stop();
            } else if (m_playbackProfile.autoReleaseExclusiveDevice
                       && (state == PlaybackBackend::State::Paused
                           || state == PlaybackBackend::State::Stopped)) {
                scheduleHeldDeviceRelease(
                    std::clamp(m_playbackProfile.autoReleaseTimeoutSec, 1, 600) * 1000);
            }
        }
    });
    connect(m_playback, &PlaybackBackend::errorOccurred, this, [this](const QString &errorString) {
        if (errorString.isEmpty())
            return;
        statusBar()->showMessage(QStringLiteral("Playback error: %1").arg(errorString), 10000);

        // A bit-perfect stream that can't open its card is almost always a busy
        // device PipeWire is still holding. Offer an in-place takeover when the
        // target really is held, instead of making the user hunt for a script.
        bool offerTakeover = false;
        if (m_playbackProfile.mode == QStringLiteral("bit-perfect")
            && !m_playbackProfile.device.isEmpty()
            && deviceErrorLooksLikeBusy(errorString)) {
            if (const auto dev = AudioDeviceControl::findByHwPath(m_playbackProfile.device))
                offerTakeover = dev->heldByPipeWire();
        }
        m_takeOverDeviceButton->setVisible(offerTakeover);
        m_takeOverDeviceButton->setEnabled(offerTakeover);
    });

    auto *queueShortcut = new QShortcut(QKeySequence(QStringLiteral("1")), this);
    connect(queueShortcut, &QShortcut::activated, this, [this]() {
        if (qobject_cast<QLineEdit *>(QApplication::focusWidget()) != nullptr) {
            return;
        }
        if (m_mainView == MainView::Queue) {
            ensureQueueScreen()->revealCurrentPlaying();
        } else {
            switchMainView(MainView::Queue);
        }
    });
    auto *libraryPanelsShortcut = new QShortcut(QKeySequence(QStringLiteral("2")), this);
    connect(libraryPanelsShortcut, &QShortcut::activated, this, [this]() {
        if (qobject_cast<QLineEdit *>(QApplication::focusWidget()) != nullptr) {
            return;
        }
        switchMainView(m_mainView == MainView::LibraryPanels ? MainView::LibraryMusicExplorer : MainView::LibraryPanels);
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
            ensureSearchView()->forceRefresh();  // re-press 4 in browse mode forces a refresh
        } else {
            switchMainView(MainView::Search);
        }
    });
    auto *playlistShortcut = new QShortcut(QKeySequence(QStringLiteral("5")), this);
    connect(playlistShortcut, &QShortcut::activated, this, [this]() {
        if (qobject_cast<QLineEdit *>(QApplication::focusWidget()) != nullptr) {
            return;
        }
        switchMainView(MainView::Playlist);
    });
    auto *jumpToPlayingShortcut = new QShortcut(QKeySequence(QStringLiteral("o")), this);
    connect(jumpToPlayingShortcut, &QShortcut::activated, this, [this]() {
        if (qobject_cast<QLineEdit *>(QApplication::focusWidget()) != nullptr) {
            return;
        }
        if (m_mainView == MainView::Queue) {
            ensureQueueScreen()->revealCurrentPlaying();
            return;
        }
        if (m_mainView == MainView::Playlist && ensurePlaylistView()->revealNowPlaying()) {
            return;
        }
        jumpToPlayingSong();
    });

    auto *repeatShortcut = new QShortcut(QKeySequence(QStringLiteral("r")), this);
    connect(repeatShortcut, &QShortcut::activated, this, [this]() {
        if (qobject_cast<QLineEdit *>(QApplication::focusWidget()) != nullptr) {
            return;
        }
        m_playerBar->cycleRepeatMode();
    });

    auto *shuffleShortcut = new QShortcut(QKeySequence(QStringLiteral("s")), this);
    connect(shuffleShortcut, &QShortcut::activated, this, [this]() {
        if (qobject_cast<QLineEdit *>(QApplication::focusWidget()) != nullptr) {
            return;
        }
        m_playerBar->cycleShuffleMode();
    });

    m_albumGrid->setArtworkCache(m_artworkCache);
    loadPlaybackProfile();
    loadPlaybackResumeSettings();
    loadViewSettings();
    loadSearchRankingConfig();
    loadPlaybackModes();
    loadQueueState();
    refreshSavedQueuePlaylistEntries();
    loadExplorerState();
    m_playerBar->setScrobbleOffline(scrobbleOffline());
    configureListenBrainz();
    configureLastFm();
    loadExistingLibrary();
    // Playback resume is handled once at AppCore startup; do not re-run it on
    // window rebuild — it would restart/seek live audio.
    // Resume the lazy fill if a previous run left placeholder rows (e.g. closed
    // mid-fill or after a canceled scan). Deferred so the window shows first.
    QTimer::singleShot(0, this, [this]() { pumpMetadataFill(); });
}

MainWindow::~MainWindow()
{
    // AppCore can destroy the widget tree while playback remains live in the
    // tray. Only restore a taken-over card here during a real application quit;
    // otherwise releasing it would interrupt headless native DSD playback.
    if (m_core != nullptr && m_core->isQuitting()) {
        releaseTakenOverDsdDevice();
    }
    if (m_scanPipeline != nullptr) {
        m_scanPipeline->cancel();
    }
    if (m_scanThread != nullptr) {
        m_scanThread->quit();
        m_scanThread->wait(3000);
    }
    if (m_fillPipeline != nullptr) {
        m_fillPipeline->cancel();
    }
    if (m_fillThread != nullptr) {
        m_fillThread->quit();
        m_fillThread->wait(3000);
    }
    if (m_audioAnalysisProcess != nullptr) {
        m_audioAnalysisProcess->terminate();
        if (!m_audioAnalysisProcess->waitForFinished(3000)) {
            m_audioAnalysisProcess->kill();
            m_audioAnalysisProcess->waitForFinished(1000);
        }
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
    if (m_dropImportThread != nullptr) {
        // requestStop() ends matching after the current (CPU-bound, prompt) entry,
        // so the worker never outlives the window and emits into a freed DB
        // connection. Partial matches are already persisted via addItem.
        if (m_dropImportWorker != nullptr) {
            m_dropImportWorker->requestStop();
        }
        m_dropImportThread->quit();
        if (!m_dropImportThread->wait(5000)) {
            m_dropImportThread->wait();
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
    if (!m_core->isQuitting()) {
        // Keep the window alive while a scan, fill, or MPD import is in
        // flight — those pipelines live in MainWindow and report into
        // window widgets.  Fall back to plain hide() so playback continues.
        if (m_scanThread != nullptr || m_fillThread != nullptr || m_mpdImportThread != nullptr
            || m_dropImportThread != nullptr) {
            hide();
            event->ignore();
            return;
        }
        event->ignore();
        QMetaObject::invokeMethod(m_core, &AppCore::releaseWindow, Qt::QueuedConnection);
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::StyleChange) {
        restylePanelBorders();
        if (m_playerBar != nullptr) {
            m_playerBar->scheduleThemeRefresh();
        }
    }
}

void MainWindow::persistViewState()
{
    rememberTrackTableViewState();
}

void MainWindow::restylePanelBorders()
{
    setStyleSheetIfChanged(m_artistSidebar, mainArtistSidebarStyleSheet(m_artistSidebar));
    setStyleSheetIfChanged(m_centerSplitter, mainCenterFrameStyleSheet(m_centerSplitter));
    setStyleSheetIfChanged(m_albumGrid, mainAlbumGridStyleSheet(m_albumGrid));
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
    ensureIngestSession();
    // A foreground scan supersedes the background fill; pause it (it resumes once
    // the scan finishes and re-pumps the placeholder backlog).
    if (m_fillPipeline != nullptr) {
        m_fillPipeline->cancel();
    }

    ScanPipeline::Options options;
    options.forceFullRescan = m_forceFullRescan;
    options.profile = static_cast<ScanPipeline::Profile>(scanProfileSetting());
    options.guessPlaceholders = guessedPlaceholdersEnabled();

    m_scanThread = new QThread(this);
    m_scanPipeline = new ScanPipeline(m_activeScanRootPath, scanRootId,
                                      m_database->trackFingerprints(m_activeScanRootPath), options);
    m_scanPipeline->moveToThread(m_scanThread);

    connect(m_scanThread, &QThread::started, m_scanPipeline, &ScanPipeline::run);
    connect(m_scanPipeline, &ScanPipeline::enumeratedReady, this, &MainWindow::ingestEnumeratedPlaceholders);
    connect(m_scanPipeline, &ScanPipeline::batchReady, this, &MainWindow::ingestScanBatch);
    connect(m_scanPipeline, &ScanPipeline::progress, this,
            [this](qint64 enumerated, qint64 toProcess, qint64 processed, const QString &phase) {
                // The foreground pass only enumerates and re-reads *changed* files;
                // new files are deferred to the background metadata fill.
                if (phase == QStringLiteral("enumerating")) {
                    statusBar()->showMessage(QStringLiteral("Scanning: enumerating files..."));
                } else if (toProcess > 0) {
                    statusBar()->showMessage(QStringLiteral("Scanning: re-read %1 of %2 changed (%3 found)")
                                                 .arg(processed).arg(toProcess).arg(enumerated));
                } else {
                    statusBar()->showMessage(QStringLiteral("Scanning: %1 files found").arg(enumerated));
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
            pumpMetadataFill();  // lazily tag-read the placeholders this scan created
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

    patchQueueTracksFromMetadata(tracks);
    scheduleIncrementalRefresh();
}

void MainWindow::ingestEnumeratedPlaceholders(const QVector<Track> &tracks)
{
    if (tracks.isEmpty()) {
        return;
    }
    if (!m_database->insertEnumeratedPlaceholders(tracks)) {
        QMessageBox::warning(this, QStringLiteral("Scanner"), m_database->lastError());
        return;
    }
    // Placeholders only surface in the directory/file view; coalesce the refresh
    // with the rest of the ingest so a flood of new paths doesn't rebuild per chunk.
    scheduleIncrementalRefresh();
}

void MainWindow::scheduleIncrementalRefresh()
{
    // Throttle (not debounce): during a continuous scan/fill, batches arrive faster
    // than the interval, so we refresh at most once per window while dirty rather
    // than never until the stream pauses. Keeps the browse/explorer filling in
    // light chunks without rebuilding on every batch.
    if (m_incrementalRefreshTimer == nullptr) {
        m_incrementalRefreshTimer = new QTimer(this);
        m_incrementalRefreshTimer->setSingleShot(true);
        connect(m_incrementalRefreshTimer, &QTimer::timeout, this, [this]() {
            if (m_incrementalRefreshDirty) {
                m_incrementalRefreshDirty = false;
                refreshArtists();
                refreshLibraryFileExplorer();
            }
        });
    }
    m_incrementalRefreshDirty = true;
    if (!m_incrementalRefreshTimer->isActive()) {
        m_incrementalRefreshTimer->start(1500);
    }
}

void MainWindow::flushIncrementalRefresh()
{
    if (m_incrementalRefreshTimer != nullptr) {
        m_incrementalRefreshTimer->stop();
    }
    m_incrementalRefreshDirty = false;
    refreshArtists();
    refreshLibraryFileExplorer();
}

int MainWindow::scanProfileSetting() const
{
    const QString value = m_state->setting(QStringLiteral("scan.profile"), QStringLiteral("balanced"));
    if (value == QStringLiteral("background")) {
        return 0;
    }
    if (value == QStringLiteral("turbo")) {
        return 2;
    }
    return 1;
}

int MainWindow::analysisPowerSetting() const
{
    const QString value = m_state->setting(QStringLiteral("analysis.power"), QStringLiteral("background"));
    if (value == QStringLiteral("balanced")) {
        return 1;
    }
    if (value == QStringLiteral("turbo")) {
        return 2;
    }
    return 0;
}

bool MainWindow::guessedPlaceholdersEnabled() const
{
    return m_state->setting(QStringLiteral("scan.guessedPlaceholders"), QStringLiteral("1")) != QStringLiteral("0");
}

void MainWindow::ensureIngestSession()
{
    if (!m_ingestSessionActive) {
        m_database->beginScanSession();
        m_ingestSessionActive = true;
    }
}

void MainWindow::endIngestSessionIfIdle()
{
    if (m_ingestSessionActive && m_scanThread == nullptr && m_fillThread == nullptr) {
        m_database->endScanSession();
        m_ingestSessionActive = false;
    }
}

QStringList MainWindow::nextFillChunk()
{
    // Prefer the directory the user is looking at (on-access prioritization),
    // then drain the rest of the backlog in bounded chunks.
    if (!m_priorityFillDir.isEmpty()) {
        const QStringList dirPaths = m_database->enumeratedOnlyPaths(m_priorityFillDir, 256);
        if (!dirPaths.isEmpty()) {
            return dirPaths;
        }
        m_priorityFillDir.clear();
    }
    return m_database->enumeratedOnlyPaths({}, 512);
}

void MainWindow::pumpMetadataFill()
{
    // One ingest worker at a time: never run a fill alongside a foreground scan or
    // another fill — that would double-read files and thrash a slow/HDD mount.
    if (m_scanThread != nullptr || m_fillThread != nullptr || m_librarySource != LibrarySource::Local) {
        return;
    }
    const QStringList chunk = nextFillChunk();
    if (chunk.isEmpty()) {
        endIngestSessionIfIdle();
        return;
    }
    startMetadataFill(chunk);
}

void MainWindow::startMetadataFill(const QStringList &paths)
{
    if (paths.isEmpty() || m_fillThread != nullptr || m_scanThread != nullptr) {
        return;
    }
    ensureIngestSession();

    ScanPipeline::Options options;
    options.lowPriority = true;
    options.batchSize = 64;  // small batches keep the UI fill smooth
    options.profile = static_cast<ScanPipeline::Profile>(scanProfileSetting());

    const QString hint = QFileInfo(paths.first()).absolutePath();
    m_fillThread = new QThread(this);
    m_fillPipeline = new ScanPipeline(hint, paths, options);
    m_fillPipeline->moveToThread(m_fillThread);
    connect(m_fillThread, &QThread::started, m_fillPipeline, &ScanPipeline::run);
    connect(m_fillPipeline, &ScanPipeline::batchReady, this, &MainWindow::ingestScanBatch);
    connect(m_fillPipeline, &ScanPipeline::progress, this,
            [this](qint64, qint64 toProcess, qint64 processed, const QString &phase) {
                Q_UNUSED(processed);
                if (phase == QStringLiteral("filling") && toProcess > 0) {
                    // Show the live backlog (this chunk + everything still queued),
                    // not just the chunk size — one cheap indexed COUNT per batch.
                    statusBar()->showMessage(
                        QStringLiteral("Filling metadata: %1 tracks remaining").arg(m_database->enumeratedOnlyCount()),
                        2000);
                }
            });
    connect(m_fillPipeline, &ScanPipeline::finished, this, &MainWindow::finishMetadataFill);
    connect(m_fillPipeline, &ScanPipeline::finished, m_fillThread, &QThread::quit);
    connect(m_fillThread, &QThread::finished, m_fillPipeline, &QObject::deleteLater);
    connect(m_fillThread, &QThread::finished, m_fillThread, &QObject::deleteLater);
    connect(m_fillThread, &QThread::finished, this, [this]() {
        m_fillThread = nullptr;
        m_fillPipeline = nullptr;
        pumpMetadataFill();  // next chunk, or end the ingest session when drained
    });
    m_fillThread->start();
}

void MainWindow::finishMetadataFill(qint64 enumerated, qint64 indexed, qint64 skipped, bool canceled)
{
    Q_UNUSED(enumerated);
    Q_UNUSED(indexed);
    Q_UNUSED(skipped);
    Q_UNUSED(canceled);
    // ingestScanBatch already refreshed the views incrementally during the chunk;
    // when the whole backlog is drained, do a final browse + search-index refresh.
    if (m_database->enumeratedOnlyPaths({}, 1).isEmpty()) {
        qCInfo(uiLog) << "background metadata fill complete";
        flushIncrementalRefresh();
        if (m_searchView != nullptr) {
            m_searchView->invalidateIndex(databasePath());
        }
        statusBar()->showMessage(QStringLiteral("Library metadata complete"), 4000);
    }
}

void MainWindow::ensureDirectoryScanned(const QString &directory)
{
    if (directory.isEmpty() || m_librarySource != LibrarySource::Local) {
        return;
    }
    if (m_database->enumeratedOnlyPaths(directory, 1).isEmpty()) {
        return;  // nothing pending in this directory
    }
    // Jump this directory to the front of the fill so opening it reads its tags now.
    m_priorityFillDir = directory;
    pumpMetadataFill();
}

void MainWindow::finishScan(qint64 enumerated, qint64 indexed, qint64 skipped, bool canceled)
{
    // This is the foreground pass finishing (enumerate + re-read changed files), not
    // the whole library: new files were turned into placeholders and their metadata
    // is read lazily by the background fill. Report both phases honestly.
    const int pendingFill = m_database->enumeratedOnlyCount();
    qCInfo(uiLog).nospace() << "scan pass finished: enumerated " << enumerated
                            << ", re-read " << indexed << " changed, " << skipped << " unchanged, "
                            << pendingFill << " queued for background metadata fill"
                            << (canceled ? " (canceled)" : "");
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
    QString summary;
    if (canceled) {
        summary = QStringLiteral("Scan canceled: %1 enumerated, %2 unchanged").arg(enumerated).arg(skipped);
    } else if (pendingFill > 0) {
        summary = QStringLiteral("Scan complete: %1 files (%2 changed, %3 unchanged) — reading metadata for %4 in the background")
                      .arg(enumerated).arg(indexed).arg(skipped).arg(pendingFill);
    } else {
        summary = QStringLiteral("Scan complete: %1 files (%2 changed, %3 unchanged)")
                      .arg(enumerated).arg(indexed).arg(skipped);
    }
    statusBar()->showMessage(summary, 10000);
    flushIncrementalRefresh();
    // Rebuild the search index with fresh library data
    if (!canceled && m_searchView != nullptr) {
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
        m_player->markTracksMissing(paths);
        if (m_playlistDb != nullptr && m_playlistDb->markItemsMissing(paths) > 0 && m_playlistView != nullptr) {
            m_playlistView->reloadItems();
            m_playlistView->reloadPlaylists();
        }
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
    const QStringList missingPaths = m_database->missingTrackPaths();
    QSet<QString> removedPaths;
    removedPaths.reserve(missingPaths.size());
    for (const QString &path : missingPaths) {
        removedPaths.insert(path);
    }
    const int removed = m_database->removeMissingTracks();
    if (removed > 0 && m_playlistDb != nullptr && m_playlistDb->markItemsMissing(missingPaths) > 0 && m_playlistView != nullptr) {
        m_playlistView->reloadItems();
        m_playlistView->reloadPlaylists();
    }
    if (removed > 0 && !removedPaths.isEmpty()) {
        QVector<int> queueRows;
        const QVector<Track> &queue = m_player->queue();
        queueRows.reserve(queue.size());
        for (int row = 0; row < queue.size(); ++row) {
            if (removedPaths.contains(queue.at(row).path)) {
                queueRows.push_back(row);
            }
        }
        if (!queueRows.isEmpty()) {
            removeQueueRows(queueRows);
        }
    }
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
    const QString sidebarArtistBefore = m_artistSidebar->currentArtistName();
    if (m_librarySource == LibrarySource::Mpd) {
        const qint64 sourceId = m_database->mpdSourceId();
        m_artistSidebar->setMpdAvailable(sourceId > 0);
        const QVector<Artist> artists = m_database->mpdAlbumArtists();
        m_artistSidebar->setArtists(artists);
        if (m_panelSearch != nullptr) {
            m_panelSearch->refreshPanel(MainPanelId::Artists);
        }
        const bool preserveNavigationCursor = !sidebarArtistBefore.isEmpty()
            && sidebarArtistBefore != m_currentArtist
            && m_artistSidebar->currentArtistName() == sidebarArtistBefore;
        if (!m_currentArtist.isEmpty()
            && (preserveNavigationCursor || m_artistSidebar->selectArtist(m_currentArtist, /*reveal=*/false))) {
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

    const bool preserveNavigationCursor = !sidebarArtistBefore.isEmpty()
        && sidebarArtistBefore != m_currentArtist
        && m_artistSidebar->currentArtistName() == sidebarArtistBefore;
    if (!m_currentArtist.isEmpty()
        && (preserveNavigationCursor || m_artistSidebar->selectArtist(m_currentArtist, /*reveal=*/false))) {
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

void MainWindow::setAlbumNarrowFromGrid(const QStringList &albumTitles)
{
    // Live "narrowing follows selection" path from the album grid (cursor moves,
    // mark changes). Unlike narrowAlbumFilters it refreshes only the track table
    // and updates the grid's narrow highlight in place — no album-grid rebuild —
    // so it is cheap enough to run on every keystroke. An empty set clears the
    // narrowing (back to the whole artist).
    const QStringList next = normalizedAlbumTitles(albumTitles);
    if (next == m_selectedAlbumTitles) {
        return;
    }
    rememberTrackTableViewState();
    m_selectedAlbumTitles = next;
    m_selectedAlbumTitle = next.size() == 1 ? next.first() : QString();
    rememberCurrentSourceSelection();
    refreshTrackTable();
    restoreTrackTableViewState();
    m_albumGrid->setSelectedAlbumTitle(m_selectedAlbumTitle);
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
    QVector<Album> albums;
    if (m_librarySource == LibrarySource::Mpd) {
        albums = m_database->mpdAlbumsForArtist(m_currentArtist, mpdMusicDirectory());
    } else {
        albums = m_database->albumsForArtist(m_currentArtist);
    }
    m_albumGrid->setAlbums(albums, freshLoad);
    if (m_musicExplorerView != nullptr) {
        m_musicExplorerView->setAlbums(albums);
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
    if (m_musicExplorerView != nullptr) {
        m_musicExplorerView->refreshExpandedTracks();
    }
}

void MainWindow::applyTrackRating(const Track &track, int rating0To100, const QString &sourceSurface)
{
    const auto oldRating = m_database->trackRatingSnapshot(track.path);
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
    Track eventTrack = track;
    if (eventTrack.musicBrainz.recordingId.isEmpty()) {
        eventTrack.musicBrainz.recordingId = oldRating.mbRecordingId;
    }
    m_core->recordRatingEvent(eventTrack,
                              oldRating.hasUserRating,
                              oldRating.userRating0To100,
                              oldRating.effectiveRating0To100,
                              rating0To100,
                              sourceSurface);
    // Patch the rated row in place instead of rebuilding the whole track table
    // (a full reload also dropped scroll/selection, hence the old remember/restore
    // dance). The album grid still refreshes because its star reflects the album's
    // average track rating, which this edit can shift. track.rating0To100 already
    // carries the scanned file rating (or unset), so it is the right fallback when
    // a user rating is cleared.
    const bool nowHasUserRating = rating0To100 >= 0;
    m_trackTable->updateTrackRating(track.path, nowHasUserRating ? rating0To100 : track.rating0To100, nowHasUserRating);
    if (m_musicExplorerView != nullptr) {
        m_musicExplorerView->refreshExpandedTracks();
    }
    refreshAlbumGrid();
    m_player->updateTrackRating(track.path, rating0To100 >= 0 ? rating0To100 : track.rating0To100, rating0To100 >= 0);
    if (m_player->currentTrack().path == track.path) {
        const Track &current = m_player->currentTrack();
        const QString title = current.title.isEmpty() ? current.filename : current.title;
        QString subtitle = QStringLiteral("%1 - %2").arg(current.artistName, current.albumTitle);
        if (!current.date.isEmpty()) {
            subtitle += QStringLiteral(" (%1)").arg(current.date.left(4));
        }
        m_playerBar->setTrackInfo(title, subtitle, current.effectiveRating0To100);
        m_rightSidebar->setTrackInfo(current);
        m_mpris->setTrack(current);
    }
    m_queueStore->updateTrackRating(track.path, rating0To100 >= 0 ? rating0To100 : track.rating0To100, rating0To100 >= 0);
    if (m_playlistView != nullptr) {
        m_playlistView->updateTrackRating(track.path, rating0To100 >= 0 ? rating0To100 : track.rating0To100);
    }
    scheduleQueueStateSave();

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
            if (m_musicExplorerView != nullptr) {
                m_musicExplorerView->refreshExpandedTracks();
            }
            if (m_playlistView != nullptr) {
                m_playlistView->updateTrackRating(update.path, effective);
            }
            currentTrackChanged = m_player->applyRatingSync(update.path, effective) || currentTrackChanged;
        }
        if (currentTrackChanged) {
            const Track &current = m_player->currentTrack();
            const QString title = current.title.isEmpty() ? current.filename : current.title;
            QString subtitle = QStringLiteral("%1 - %2").arg(current.artistName, current.albumTitle);
            if (!current.date.isEmpty()) {
                subtitle += QStringLiteral(" (%1)").arg(current.date.left(4));
            }
            m_playerBar->setTrackInfo(title, subtitle, current.effectiveRating0To100);
            m_rightSidebar->setTrackInfo(current);
            m_mpris->setTrack(current);
        }
        if (!summary.updates.isEmpty()) {
            m_queueStore->setSnapshot(m_player->queue(), m_player->queueIndex(),
                                      m_player->queueIndex() + 1, m_player->playNextInsertIndex());
            refreshPlayNextRange();
            scheduleQueueStateSave();
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
    const Track current = m_player->currentTrack();
    if (m_librarySource != LibrarySource::Local || current.path.isEmpty() || current.effectiveRating0To100 < 0) {
        statusBar()->showMessage(QStringLiteral("No current local rated track to sync"), 5000);
        return;
    }
    startRatingTagSync({current}, static_cast<int>(RatingTagSyncRequest::Scope::Track));
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
    m_loadingViewSettings = true;
    m_trackTable->applyViewSettingsJson(m_state->setting(QStringLiteral("trackTable.view")));
    if (m_musicExplorerView != nullptr) {
        m_musicExplorerView->applyAlbumGridViewSettingsJson(m_state->setting(QStringLiteral("albumGrid.view")));
        m_musicExplorerView->applyTrackTableViewSettingsJson(m_state->setting(QStringLiteral("trackTable.view")));
    }
    const QString rightSidebarSettings = m_state->setting(QStringLiteral("rightSidebar.view"));
    m_rightSidebar->applyViewSettingsJson(rightSidebarSettings);
    m_playerBar->setTrackInfoPaneVisible(QJsonDocument::fromJson(rightSidebarSettings.toUtf8()).object().value(QStringLiteral("showTrackInfo")).toBool(true));
    const QJsonObject playerBar = QJsonDocument::fromJson(m_state->setting(QStringLiteral("playerBar.view")).toUtf8()).object();
    m_playerBar->setCompactMenu(playerBar.value(QStringLiteral("compactMenu")).toBool(false));
    m_playerBar->setAlwaysShowTray(m_state->setting(QStringLiteral("tray.alwaysVisible"), QStringLiteral("false")) == QStringLiteral("true"));

    const int volume = std::clamp(m_state->setting(QStringLiteral("volume"), QStringLiteral("100")).toInt(), 0, 100);
    m_player->setVolume(static_cast<double>(volume) / 100.0);
    m_playerBar->setVolume(volume);
    m_albumGrid->applyViewSettingsJson(m_state->setting(QStringLiteral("albumGrid.view")));
    m_artistSidebar->applyViewSettingsJson(m_state->setting(QStringLiteral("artistSidebar.view")));
    const QJsonObject mainWindow = QJsonDocument::fromJson(m_state->setting(QStringLiteral("mainWindow.view")).toUtf8()).object();
    const QByteArray geometry = QByteArray::fromBase64(mainWindow.value(QStringLiteral("geometry")).toString().toLatin1());
    if (!geometry.isEmpty()) {
        restoreGeometry(geometry);
    }
    SplitterPersistence::restoreSplitterIfStable(m_rootSplitter,
                            mainWindow.value(QStringLiteral("rootSplitter")).toArray(),
                            {kArtistSidebarMinimumWidth, kCenterPaneMinimumWidth, kRightSidebarMinimumWidth},
                            kRootSplitterMinimumTotal);
    SplitterPersistence::restoreSplitterIfStable(m_centerSplitter,
                            mainWindow.value(QStringLiteral("centerSplitter")).toArray(),
                            {kPanelMinimumHeight, kPanelMinimumHeight},
                            kCenterSplitterMinimumTotal);
    m_mainView = mainViewFromName(mainWindow.value(QStringLiteral("mainView")).toString());
    m_libraryExplorerDirectory = mainWindow.value(QStringLiteral("libraryExplorerDirectory")).toString();
    m_freeRoamDirectory = mainWindow.value(QStringLiteral("freeRoamDirectory")).toString(QDir::homePath());

    const bool showUnsupported = m_state->setting(QStringLiteral("fileExplorer.showUnsupported")) == QStringLiteral("true");
    m_playerBar->setListUnsupportedFiles(showUnsupported);

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
    m_artistSidebar->setNavigationScrollPadding(mainPanelScrollPadding);
    m_trackTable->setNavigationScrollPadding(mainPanelScrollPadding);
    if (m_musicExplorerView != nullptr) {
        m_musicExplorerView->setNavigationScrollPadding(mainPanelScrollPadding);
    }

    switchMainView(m_mainView);
    applySharedTableSettings();
    m_loadingViewSettings = false;
}

void MainWindow::saveTrackTableViewSettings()
{
    if (m_loadingViewSettings || m_applyingTrackTableViewSettings) {
        return;
    }

    QObject *source = sender();
    const QString settings = source == m_musicExplorerView && m_musicExplorerView != nullptr
        ? m_musicExplorerView->trackTableViewSettingsJson()
        : m_trackTable->viewSettingsJson();
    m_state->setSetting(QStringLiteral("trackTable.view"), settings);

    QScopedValueRollback<bool> applying(m_applyingTrackTableViewSettings, true);
    if (source == m_musicExplorerView) {
        m_trackTable->applyViewSettingsJson(settings);
    } else if (m_musicExplorerView != nullptr) {
        m_musicExplorerView->applyTrackTableViewSettingsJson(settings);
    }
    applySharedTableSettings();
}

void MainWindow::saveAlbumGridViewSettings()
{
    const QString settings = m_albumGrid->viewSettingsJson();
    m_state->setSetting(QStringLiteral("albumGrid.view"), settings);
    if (m_musicExplorerView != nullptr) {
        m_musicExplorerView->applyAlbumGridViewSettingsJson(settings);
    }
}

void MainWindow::saveMusicExplorerAlbumGridViewSettings()
{
    if (m_musicExplorerView == nullptr) {
        return;
    }
    const QString settings = m_musicExplorerView->albumGridViewSettingsJson();
    m_state->setSetting(QStringLiteral("albumGrid.view"), settings);
    m_albumGrid->applyViewSettingsJson(settings);
}

void MainWindow::saveArtistSidebarViewSettings()
{
    m_state->setSetting(QStringLiteral("artistSidebar.view"), m_artistSidebar->viewSettingsJson());
}

void MainWindow::saveRightSidebarViewSettings()
{
    if (m_loadingViewSettings) {
        return;
    }
    m_state->setSetting(QStringLiteral("rightSidebar.view"), m_rightSidebar->viewSettingsJson());
    applySharedTableSettings();
}

void MainWindow::saveQueueScreenViewSettings()
{
    if (m_queueScreen == nullptr) {
        return;
    }
    m_state->setSetting(QStringLiteral("queueScreen.view"), m_queueScreen->viewSettingsJson());
}

void MainWindow::savePlaylistViewSettings()
{
    if (m_playlistView == nullptr) {
        return;
    }
    m_state->setSetting(QStringLiteral("playlistView.view"), m_playlistView->viewSettingsJson());
    applySharedTableSettings();
}

void MainWindow::saveMainWindowViewSettings(bool captureSplitterSizes)
{
    if (m_loadingViewSettings) {
        return;
    }

    QJsonObject root = QJsonDocument::fromJson(m_state->setting(QStringLiteral("mainWindow.view")).toUtf8()).object();
    root.insert(QStringLiteral("geometry"), QString::fromLatin1(saveGeometry().toBase64()));
    if (captureSplitterSizes) {
        const QList<int> rootSizes = m_rootSplitter->sizes();
        if (SplitterPersistence::splitterSizesAreStable(rootSizes,
                                   {kArtistSidebarMinimumWidth, kCenterPaneMinimumWidth, kRightSidebarMinimumWidth},
                                   kRootSplitterMinimumTotal)) {
            root.insert(QStringLiteral("rootSplitter"), SplitterPersistence::splitterSizesToJson(rootSizes));
        }
        const QList<int> centerSizes = m_centerSplitter->sizes();
        if (SplitterPersistence::splitterSizesAreStable(centerSizes,
                                   {kPanelMinimumHeight, kPanelMinimumHeight},
                                   kCenterSplitterMinimumTotal)) {
            root.insert(QStringLiteral("centerSplitter"), SplitterPersistence::splitterSizesToJson(centerSizes));
        }
    }
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
    if (m_queueScreen != nullptr) {
        saveQueueScreenViewSettings();
    }
    if (m_playlistView != nullptr) {
        savePlaylistViewSettings();
    }
    saveMainWindowViewSettings();
}

void MainWindow::resetViewPreferences()
{
    const QStringList keys = {
        QStringLiteral("trackTable.view"),
        QStringLiteral("rightSidebar.view"),
        QStringLiteral("queueScreen.view"),
        QStringLiteral("playlistView.view"),
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
    if (m_musicExplorerView != nullptr) {
        m_musicExplorerView->applyTrackTableViewSettingsJson(m_trackTable->viewSettingsJson());
    }
    m_albumGrid->resetViewSettings();
    if (m_musicExplorerView != nullptr) {
        m_musicExplorerView->applyAlbumGridViewSettingsJson(m_albumGrid->viewSettingsJson());
    }
    m_artistSidebar->resetViewSettings();
    m_rightSidebar->resetViewSettings();
    if (m_queueScreen != nullptr) {
        m_queueScreen->resetViewSettings();
    }
    if (m_playlistView != nullptr) {
        m_playlistView->resetViewSettings();
    }

    m_rootSplitter->setSizes({197, 1296, 298});
    m_centerSplitter->setSizes({540, 430});
    m_playerBar->setCompactMenu(false);
    m_playerBar->setTrackInfoPaneVisible(true);
    m_playerBar->setListUnsupportedFiles(false);

    const QString explorerProfile = defaultKeyBindingProfiles().isEmpty()
        ? QStringLiteral("vim")
        : defaultKeyBindingProfiles().first().name;
    if (m_libraryFileExplorer != nullptr) {
        m_libraryFileExplorer->setKeyBindingProfileName(explorerProfile);
        m_libraryFileExplorer->setKeyHintBarVisible(false);
        m_libraryFileExplorer->setShowUnsupportedFiles(false);
        m_libraryFileExplorer->setRowHeight(18);
        m_libraryFileExplorer->setSort(MusicSort::SortField::FileName, false, false);
    }
    if (m_freeRoamFileExplorer != nullptr) {
        m_freeRoamFileExplorer->setKeyBindingProfileName(explorerProfile);
        m_freeRoamFileExplorer->setKeyHintBarVisible(false);
        m_freeRoamFileExplorer->setShowUnsupportedFiles(false);
        m_freeRoamFileExplorer->setStartDirectory(QString());
        m_freeRoamFileExplorer->setRowHeight(18);
        m_freeRoamFileExplorer->setSort(MusicSort::SortField::FileName, false, false);
    }

    if (m_queueScreen != nullptr) {
        m_queueScreen->setKeyBindingProfileName(defaultQueueKeyBindingProfileName());
    }
    if (m_panelSearch != nullptr) {
        m_panelSearch->setKeyBindingProfileName(defaultMainPanelKeyBindingProfileName());
        m_panelSearch->setFocusOrder(defaultMainPanelFocusOrder());
        m_panelSearch->setActivePanel(MainPanelId::Artists, false);
    }
    const int defaultScrollPadding = TableNavigationScroll::kDefaultPaddingRows;
    m_rightSidebar->setNavigationScrollPadding(defaultScrollPadding);
    if (m_queueScreen != nullptr) {
        m_queueScreen->setNavigationScrollPadding(defaultScrollPadding);
    }
    m_artistSidebar->setNavigationScrollPadding(defaultScrollPadding);
    m_trackTable->setNavigationScrollPadding(defaultScrollPadding);

    saveAllViewSettings();
    statusBar()->showMessage(QStringLiteral("View preferences reset to defaults"), 4000);
}

void MainWindow::resetPanelOrder()
{
    if (m_panelSearch == nullptr) {
        return;
    }
    m_panelSearch->setFocusOrder(defaultMainPanelFocusOrder());
    m_state->removeSetting(QStringLiteral("mainPanel.focusOrder"));
    statusBar()->showMessage(QStringLiteral("Panel order reset to Artist > Album > Track > Queue"), 4000);
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

QueueScreen *MainWindow::ensureQueueScreen()
{
    if (m_queueScreen != nullptr) {
        return m_queueScreen;
    }

    m_queueScreen = new QueueScreen(m_mainStack);
    m_queueScreen->setQueueStore(m_queueStore);
    m_queueScreen->setPickReasonResolver([this](const QString &path) {
        return m_core->radioPickReason(path);
    });
    m_queueScreen->setTrackFlagResolver([this](const Track &track, const QString &flag) {
        return m_core->trackFlag(track.path, flag);
    });
    m_queueScreen->applyViewSettingsJson(m_state->setting(QStringLiteral("queueScreen.view")));
    m_queueScreen->setKeyBindingProfileName(m_state->setting(QStringLiteral("queueScreen.keyBindingProfile"),
                                                             defaultQueueKeyBindingProfileName()));
    const int mainPanelScrollPadding = std::clamp(m_state->setting(QStringLiteral("mainPanel.scrollPadding"),
                                                                   QString::number(TableNavigationScroll::kDefaultPaddingRows)).toInt(),
                                                  0, 20);
    m_queueScreen->setNavigationScrollPadding(mainPanelScrollPadding);
    m_queueScreen->setQueueIsPlaylistSourced(queueIsPlaylistSourced());

    connect(m_queueScreen, &QueueScreen::queueTrackActivated, this, [this](int index) {
        playQueueIndex(index, /*notifyScrobbler=*/true, /*startPaused=*/false, /*explicitJump=*/true);
    });
    connect(m_queueScreen, &QueueScreen::queueTrackRatingChanged, this, [this](const Track &track, int rating) {
        applyTrackRating(track, rating, QStringLiteral("queue"));
    });
    connect(m_queueScreen, &QueueScreen::queueRowsMoveRequested, this, &MainWindow::moveQueueRows);
    connect(m_queueScreen, &QueueScreen::queueRowsRemoveRequested, this, &MainWindow::removeQueueRows);
    connect(m_queueScreen, &QueueScreen::removeAllMissingTracksRequested, this, &MainWindow::removeMissingTracks);
    connect(m_queueScreen, &QueueScreen::queueClearRequested, this, &MainWindow::clearQueue);
    connect(m_queueScreen, &QueueScreen::clearPlayNextPriorityRequested, this, &MainWindow::clearPlayNextPriority);
    connect(m_queueScreen, &QueueScreen::findFileRequested, this, &MainWindow::findTrackFile);
    connect(m_queueScreen, &QueueScreen::propertiesRequested, this, &MainWindow::showTrackProperties);
    connect(m_queueScreen, &QueueScreen::startRadioRequested, this, [this](const Track &track) {
        startRadioFromSeed(track.path);
    });
    connect(m_queueScreen, &QueueScreen::trackFlagChanged, this, &MainWindow::applyTrackFlag);
    connect(m_queueScreen, &QueueScreen::addToPlaylistRequested, this, &MainWindow::openAddToPlaylistDialog);
    connect(m_queueScreen, &QueueScreen::saveQueueAsRequested, this, &MainWindow::saveCurrentQueueAs);
    connect(m_queueScreen, &QueueScreen::restorePreviousQueueRequested, this, &MainWindow::restorePreviousQueue);
    connect(m_queueScreen, &QueueScreen::unlinkQueueFromPlaylistRequested, this, &MainWindow::unlinkQueueFromPlaylist);
    connect(m_queueScreen, &QueueScreen::trackLibraryRequested, this, &MainWindow::revealTrackInLibrary);
    connect(m_queueScreen, &QueueScreen::viewSettingsChanged, this, &MainWindow::saveQueueScreenViewSettings);

    m_mainStack->addWidget(m_queueScreen);
    return m_queueScreen;
}

SearchView *MainWindow::ensureSearchView()
{
    if (m_searchView != nullptr) {
        return m_searchView;
    }

    m_searchView = new SearchView(m_mainStack, idleReleaseMs());
    m_searchView->setRankConfig(Search::RankConfig::fromJsonString(m_database->setting(QStringLiteral("search.ranking"))));
    m_searchView->setQueueIsPlaylistSourced(queueIsPlaylistSourced());
    m_searchView->setTrackFlagResolver([this](const Track &track, const QString &flag) {
        return m_core->trackFlag(track.path, flag);
    });

    connect(m_searchView, &SearchView::addToQueueRequested, this, [this](const QVector<Track> &tracks) {
        enqueueTracksFromMenu(tracks, QueueAddMode::Append, false);
    });
    connect(m_searchView, &SearchView::playNextRequested, this, [this](const QVector<Track> &tracks) {
        enqueueTracksFromMenu(tracks, QueueAddMode::PlayNext, false);
    });
    connect(m_searchView, &SearchView::addToQueueTemporaryRequested, this, [this](const QVector<Track> &tracks) {
        enqueueTracksFromMenu(tracks, QueueAddMode::Append, true);
    });
    connect(m_searchView, &SearchView::playNextTemporaryRequested, this, [this](const QVector<Track> &tracks) {
        enqueueTracksFromMenu(tracks, QueueAddMode::PlayNext, true);
    });
    connect(m_searchView, &SearchView::playNowRequested, this, [this](const QVector<Track> &tracks) {
        if (tracks.isEmpty()) return;
        addTracksToQueue(tracks);
        playQueueIndex(static_cast<int>(m_player->queue().size()) - static_cast<int>(tracks.size()));
    });
    connect(m_searchView, &SearchView::findInLibraryRequested, this, &MainWindow::revealTrackInLibrary);
    connect(m_searchView, &SearchView::findFileRequested, this, &MainWindow::findTrackFile);
    connect(m_searchView, &SearchView::propertiesRequested, this, &MainWindow::showTrackProperties);
    connect(m_searchView, &SearchView::addToPlaylistRequested, this, &MainWindow::openAddToPlaylistDialog);
    connect(m_searchView, &SearchView::searchRankingRequested, this, &MainWindow::configureSearchRanking);
    connect(m_searchView, &SearchView::startRadioRequested, this, [this](const Track &track) {
        startRadioFromSeed(track.path);
    });
    connect(m_searchView, &SearchView::trackFlagChanged, this, &MainWindow::applyTrackFlag);

    m_mainStack->addWidget(m_searchView);
    return m_searchView;
}

void MainWindow::ensureFileExplorers()
{
    if (m_libraryFileExplorer != nullptr && m_freeRoamFileExplorer != nullptr) {
        return;
    }

    const int idleMs = idleReleaseMs();
    m_libraryFileExplorer = new FileExplorerView(m_mainStack, idleMs);
    m_libraryFileExplorer->setMode(FileExplorerMode::Library);
    m_libraryFileExplorer->setModeTitle(QStringLiteral("Library Explorer"));
    m_libraryFileExplorer->setQueueIsPlaylistSourced(queueIsPlaylistSourced());

    m_freeRoamFileExplorer = new FileExplorerView(m_mainStack, idleMs);
    m_freeRoamFileExplorer->setMode(FileExplorerMode::FreeRoam);
    m_freeRoamFileExplorer->setRootPath(m_freeRoamDirectory.isEmpty() ? QDir::homePath() : m_freeRoamDirectory);
    m_freeRoamFileExplorer->setModeTitle(QStringLiteral("File System Explorer"));
    m_freeRoamFileExplorer->setQueueIsPlaylistSourced(queueIsPlaylistSourced());

    const auto trackResolver = [this](const QString &path) { return m_database->trackForPath(path); };
    m_libraryFileExplorer->setTrackResolver(trackResolver);
    m_freeRoamFileExplorer->setTrackResolver(trackResolver);

    const QString keyProfile = m_state->setting(QStringLiteral("fileExplorer.keyBindingProfile"));
    if (!keyProfile.isEmpty()) {
        m_libraryFileExplorer->setKeyBindingProfileName(keyProfile);
        m_freeRoamFileExplorer->setKeyBindingProfileName(keyProfile);
    }
    const bool hintsVisible = m_state->setting(QStringLiteral("fileExplorer.showKeyHints")) == QStringLiteral("true");
    m_libraryFileExplorer->setKeyHintBarVisible(hintsVisible);
    m_freeRoamFileExplorer->setKeyHintBarVisible(hintsVisible);

    const bool showUnsupported = m_state->setting(QStringLiteral("fileExplorer.showUnsupported")) == QStringLiteral("true");
    m_libraryFileExplorer->setShowUnsupportedFiles(showUnsupported);
    m_freeRoamFileExplorer->setShowUnsupportedFiles(showUnsupported);
    m_freeRoamFileExplorer->setStartDirectory(m_state->setting(QStringLiteral("fileExplorer.startDirectory")));

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

    connect(m_libraryFileExplorer, &FileExplorerView::directoryRequested, this, &MainWindow::setLibraryExplorerDirectory);
    connect(m_libraryFileExplorer, &FileExplorerView::trackActivated, this, &MainWindow::appendAndPlayTrack);
    connect(m_libraryFileExplorer, &FileExplorerView::playNextRequested, this, [this](const QVector<Track> &tracks) {
        enqueueTracksFromMenu(tracks, QueueAddMode::PlayNext, false);
    });
    connect(m_libraryFileExplorer, &FileExplorerView::addToQueueRequested, this, [this](const QVector<Track> &tracks) {
        enqueueTracksFromMenu(tracks, QueueAddMode::Append, false);
    });
    connect(m_libraryFileExplorer, &FileExplorerView::playNextTemporaryRequested, this, [this](const QVector<Track> &tracks) {
        enqueueTracksFromMenu(tracks, QueueAddMode::PlayNext, true);
    });
    connect(m_libraryFileExplorer, &FileExplorerView::addToQueueTemporaryRequested, this, [this](const QVector<Track> &tracks) {
        enqueueTracksFromMenu(tracks, QueueAddMode::Append, true);
    });
    connect(m_libraryFileExplorer, &FileExplorerView::importDirectoryRequested, this, [this](const QString &path) {
        startScan(path);
    });
    connect(m_libraryFileExplorer, &FileExplorerView::findFileRequested, this, &MainWindow::findTrackFile);
    connect(m_libraryFileExplorer, &FileExplorerView::propertiesRequested, this, &MainWindow::showTrackProperties);
    connect(m_libraryFileExplorer, &FileExplorerView::startRadioRequested, this, [this](const Track &track) {
        startRadioFromSeed(track.path);
    });
    connect(m_libraryFileExplorer, &FileExplorerView::addToPlaylistRequested, this, &MainWindow::openAddToPlaylistDialog);
    connect(m_freeRoamFileExplorer, &FileExplorerView::directoryRequested, this, &MainWindow::setFreeRoamDirectory);
    connect(m_freeRoamFileExplorer, &FileExplorerView::trackActivated, this, &MainWindow::appendAndPlayTrack);
    connect(m_freeRoamFileExplorer, &FileExplorerView::playNextRequested, this, [this](const QVector<Track> &tracks) {
        enqueueTracksFromMenu(tracks, QueueAddMode::PlayNext, false);
    });
    connect(m_freeRoamFileExplorer, &FileExplorerView::addToQueueRequested, this, [this](const QVector<Track> &tracks) {
        enqueueTracksFromMenu(tracks, QueueAddMode::Append, false);
    });
    connect(m_freeRoamFileExplorer, &FileExplorerView::playNextTemporaryRequested, this, [this](const QVector<Track> &tracks) {
        enqueueTracksFromMenu(tracks, QueueAddMode::PlayNext, true);
    });
    connect(m_freeRoamFileExplorer, &FileExplorerView::addToQueueTemporaryRequested, this, [this](const QVector<Track> &tracks) {
        enqueueTracksFromMenu(tracks, QueueAddMode::Append, true);
    });
    connect(m_freeRoamFileExplorer, &FileExplorerView::importDirectoryRequested, this, [this](const QString &path) {
        startScan(path);
    });
    connect(m_freeRoamFileExplorer, &FileExplorerView::findFileRequested, this, &MainWindow::findTrackFile);
    connect(m_freeRoamFileExplorer, &FileExplorerView::propertiesRequested, this, &MainWindow::showTrackProperties);
    connect(m_freeRoamFileExplorer, &FileExplorerView::startRadioRequested, this, [this](const Track &track) {
        startRadioFromSeed(track.path);
    });
    connect(m_freeRoamFileExplorer, &FileExplorerView::addToPlaylistRequested, this, &MainWindow::openAddToPlaylistDialog);
    connect(m_libraryFileExplorer, &FileExplorerView::trackRatingChangeRequested, this, [this](const Track &track, int rating) {
        applyTrackRating(track, rating, QStringLiteral("library_file_explorer"));
    });
    connect(m_freeRoamFileExplorer, &FileExplorerView::trackRatingChangeRequested, this, [this](const Track &track, int rating) {
        applyTrackRating(track, rating, QStringLiteral("free_roam_file_explorer"));
    });

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

    m_mainStack->addWidget(m_libraryFileExplorer);
    m_mainStack->addWidget(m_freeRoamFileExplorer);
}

FileExplorerView *MainWindow::ensureLibraryFileExplorer()
{
    ensureFileExplorers();
    return m_libraryFileExplorer;
}

FileExplorerView *MainWindow::ensureFreeRoamFileExplorer()
{
    ensureFileExplorers();
    return m_freeRoamFileExplorer;
}

MusicExplorerView *MainWindow::ensureMusicExplorerView()
{
    if (m_musicExplorerView != nullptr) {
        return m_musicExplorerView;
    }

    m_musicExplorerView = new MusicExplorerView(m_libraryCenterStack);
    m_musicExplorerView->setArtworkCache(m_artworkCache);
    m_musicExplorerView->setQueueIsPlaylistSourced(queueIsPlaylistSourced());
    m_musicExplorerView->setTrackFlagResolver([this](const Track &track, const QString &flag) {
        return m_core->trackFlag(track.path, flag);
    });
    m_musicExplorerView->applyAlbumGridViewSettingsJson(m_albumGrid->viewSettingsJson());
    m_musicExplorerView->applyTrackTableViewSettingsJson(m_state->setting(QStringLiteral("trackTable.view")));
    m_musicExplorerView->setTrackProvider([this](const Album &album) {
        const QString albumArtist = album.albumArtistName.isEmpty() ? m_currentArtist : album.albumArtistName;
        return m_librarySource == LibrarySource::Mpd
            ? m_database->mpdTracksForArtist(albumArtist, mpdMusicDirectory(), album.title)
            : m_database->tracksForArtist(albumArtist, album.title);
    });

    connect(m_musicExplorerView, &MusicExplorerView::albumPlayNextRequested, this, &MainWindow::playNextAlbum);
    connect(m_musicExplorerView, &MusicExplorerView::albumPlayReplaceRequested, this, &MainWindow::playAlbumsReplacingQueue);
    connect(m_musicExplorerView, &MusicExplorerView::albumAddToQueueRequested, this, &MainWindow::addAlbumToQueue);
    connect(m_musicExplorerView, &MusicExplorerView::albumPlayNextTemporaryRequested, this, &MainWindow::playNextAlbumTemporary);
    connect(m_musicExplorerView, &MusicExplorerView::albumAddToQueueTemporaryRequested, this, &MainWindow::addAlbumToQueueTemporary);
    connect(m_musicExplorerView, &MusicExplorerView::albumStartRadioRequested, this, &MainWindow::startRadioFromAlbum);
    connect(m_musicExplorerView, &MusicExplorerView::albumAddToPlaylistRequested, this, [this](const QStringList &titles) {
        QVector<Track> tracks;
        for (const QString &title : titles) {
            const QVector<Track> albumTracks = m_librarySource == LibrarySource::Mpd
                ? m_database->mpdTracksForArtist(m_currentArtist, mpdMusicDirectory(), title)
                : m_database->tracksForArtist(m_currentArtist, title);
            tracks.append(albumTracks);
        }
        openAddToPlaylistDialog(tracks);
    });
    connect(m_musicExplorerView, &MusicExplorerView::albumRatingChanged, this, &MainWindow::applyAlbumRating);
    connect(m_musicExplorerView, &MusicExplorerView::trackActivated, this, &MainWindow::appendAndPlayTrack);
    connect(m_musicExplorerView, &MusicExplorerView::trackPlayNextRequested, this, [this](const QVector<Track> &tracks) {
        enqueueTracksFromMenu(tracks, QueueAddMode::PlayNext, false);
    });
    connect(m_musicExplorerView, &MusicExplorerView::trackAddToQueueRequested, this, [this](const QVector<Track> &tracks) {
        enqueueTracksFromMenu(tracks, QueueAddMode::Append, false);
    });
    connect(m_musicExplorerView, &MusicExplorerView::trackPlayNextTemporaryRequested, this, [this](const QVector<Track> &tracks) {
        enqueueTracksFromMenu(tracks, QueueAddMode::PlayNext, true);
    });
    connect(m_musicExplorerView, &MusicExplorerView::trackAddToQueueTemporaryRequested, this, [this](const QVector<Track> &tracks) {
        enqueueTracksFromMenu(tracks, QueueAddMode::Append, true);
    });
    connect(m_musicExplorerView, &MusicExplorerView::trackAddToPlaylistRequested, this, &MainWindow::openAddToPlaylistDialog);
    connect(m_musicExplorerView, &MusicExplorerView::findFileRequested, this, &MainWindow::findTrackFile);
    connect(m_musicExplorerView, &MusicExplorerView::propertiesRequested, this, &MainWindow::showTrackProperties);
    connect(m_musicExplorerView, &MusicExplorerView::startRadioRequested, this, [this](const Track &track) {
        startRadioFromSeed(track.path);
    });
    connect(m_musicExplorerView, &MusicExplorerView::startArtistRadioRequested, this, &MainWindow::startArtistRadio);
    connect(m_musicExplorerView, &MusicExplorerView::trackFlagChanged, this, &MainWindow::applyTrackFlag);
    connect(m_musicExplorerView, &MusicExplorerView::trackRatingChanged, this, [this](const Track &track, int rating) {
        applyTrackRating(track, rating, QStringLiteral("music_explorer"));
    });
    connect(m_musicExplorerView, &MusicExplorerView::albumGridViewSettingsChanged,
            this, &MainWindow::saveMusicExplorerAlbumGridViewSettings);
    connect(m_musicExplorerView, &MusicExplorerView::trackTableViewSettingsChanged, this, &MainWindow::saveTrackTableViewSettings);

    m_libraryCenterStack->addWidget(m_musicExplorerView);
    if (!m_currentArtist.isEmpty()) {
        m_musicExplorerView->setAlbums(m_librarySource == LibrarySource::Mpd
                                           ? m_database->mpdAlbumsForArtist(m_currentArtist, mpdMusicDirectory())
                                           : m_database->albumsForArtist(m_currentArtist));
    }
    return m_musicExplorerView;
}

MainPanelTarget MainWindow::libraryAlbumsPanelTarget()
{
    return {
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
        [this](bool explicitNavigation) {
            m_albumGrid->setRememberedOutlineVisible(false);
            // Open the highlighted artist only when the user navigated here on
            // purpose (l from the artist list); a passive focus (clicking the
            // grid) leaves the currently shown artist untouched.
            if (explicitNavigation) {
                m_artistSidebar->activateCurrentArtist();
            }
        },
        [this](int horizontal, int vertical) { m_albumGrid->moveCurrentByGrid(horizontal, vertical); },
        [this]() { clearAlbumFilter(); },
        [this]() { return m_albumGrid->searchDocuments(); },
        [this]() { m_albumGrid->addCurrentAlbumToPlaylist(); },
        [this]() { return m_albumGrid->narrowingPersistsOnReturn(); },
    };
}

MainPanelTarget MainWindow::libraryTracksPanelTarget()
{
    return {
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
        [this](bool explicitNavigation) {
            // Same as the album grid: only re-open the highlighted artist when the
            // user explicitly navigated into the tracklist, never on a stray focus.
            if (explicitNavigation) {
                m_artistSidebar->activateCurrentArtist();
            }
        },
        {},
        [this]() { clearAlbumFilter(); },
        [this]() { return m_trackTable->searchDocuments(); },
        [this]() { m_trackTable->addCurrentTrackToPlaylist(); },
    };
}

MainPanelTarget MainWindow::musicExplorerAlbumsPanelTarget(MusicExplorerView *musicExplorer)
{
    return {
        MainPanelId::Albums,
        QStringLiteral("Albums"),
        musicExplorer->albumNavigationWidget(),
        [musicExplorer]() { return musicExplorer->rowCount(); },
        [musicExplorer]() { return musicExplorer->currentRow(); },
        [musicExplorer](int row) { musicExplorer->setCurrentRow(row); },
        {},
        [musicExplorer]() { musicExplorer->activateCurrentAlbum(); },
        [this, musicExplorer]() { playAlbumsNow(musicExplorer->albumTitlesForAction()); },
        [musicExplorer]() { musicExplorer->addCurrentAlbumToQueue(); },
        [musicExplorer]() { musicExplorer->playNextCurrentAlbum(); },
        {},
        {},
        {},
        {},
        [this](bool explicitNavigation) {
            if (explicitNavigation) {
                m_artistSidebar->activateCurrentArtist();
            }
        },
        [musicExplorer](int horizontal, int vertical) { musicExplorer->moveCurrentByGrid(horizontal, vertical); },
        [musicExplorer]() { musicExplorer->collapseExpandedAlbum(); },
        [musicExplorer]() { return musicExplorer->albumSearchDocuments(); },
        [musicExplorer]() { musicExplorer->addCurrentAlbumToPlaylist(); },
        []() { return false; },
    };
}

MainPanelTarget MainWindow::musicExplorerTracksPanelTarget(MusicExplorerView *musicExplorer)
{
    TrackTable *trackTable = musicExplorer->trackNavigationWidget();
    return {
        MainPanelId::Tracks,
        QStringLiteral("Tracks"),
        trackTable,
        [musicExplorer]() { return musicExplorer->trackRowCount(); },
        [musicExplorer]() { return musicExplorer->currentTrackRow(); },
        [musicExplorer](int row) { musicExplorer->setCurrentTrackRow(row); },
        [musicExplorer](int row, int direction) { musicExplorer->setCurrentTrackRow(row, direction); },
        [trackTable]() { trackTable->activateCurrentTrack(); },
        [trackTable]() { trackTable->activateCurrentTrack(); },
        [trackTable]() { trackTable->addCurrentTrackToQueue(); },
        [trackTable]() { trackTable->playNextCurrentTrack(); },
        [trackTable]() { trackTable->markCurrentTrack(); },
        [trackTable]() { trackTable->markAllTracks(); },
        [trackTable]() { trackTable->unmarkCurrentTrack(); },
        [trackTable]() { trackTable->unmarkAllTracks(); },
        [this, musicExplorer](bool explicitNavigation) {
            if (explicitNavigation) {
                m_artistSidebar->activateCurrentArtist();
            }
            musicExplorer->expandCurrentAlbum(false);
        },
        {},
        [musicExplorer]() { musicExplorer->collapseExpandedAlbum(); },
        [musicExplorer]() { return musicExplorer->trackSearchDocuments(); },
        [trackTable]() { trackTable->addCurrentTrackToPlaylist(); },
    };
}

void MainWindow::installLibraryMainPanelTargets()
{
    if (m_panelSearch == nullptr) {
        return;
    }
    m_panelSearch->replaceTarget(libraryAlbumsPanelTarget());
    m_panelSearch->replaceTarget(libraryTracksPanelTarget());
}

void MainWindow::installMusicExplorerMainPanelTargets(MusicExplorerView *musicExplorer)
{
    if (m_panelSearch == nullptr || musicExplorer == nullptr) {
        return;
    }
    m_panelSearch->replaceTarget(musicExplorerAlbumsPanelTarget(musicExplorer));
    m_panelSearch->replaceTarget(musicExplorerTracksPanelTarget(musicExplorer));
}

PlaylistView *MainWindow::ensurePlaylistView()
{
    if (m_playlistView != nullptr) {
        return m_playlistView;
    }

    m_playlistView = new PlaylistView(m_mainStack, idleReleaseMs());
    m_playlistView->setDatabase(m_playlistDb);
    m_playlistView->setTrackResolver([this](const QString &path) {
        return m_database != nullptr ? m_database->trackForPath(path) : Track();
    });
    m_playlistView->setTrackFlagResolver([this](const QString &path, const QString &flag) {
        return m_core->trackFlag(path, flag);
    });
    m_playlistView->applyViewSettingsJson(m_state->setting(QStringLiteral("playlistView.view")));
    m_playlistView->setSavedQueueEntries(savedQueuePlaylistEntries());
    m_playlistView->setQueueIsPlaylistSourced(queueIsPlaylistSourced());
    if (!m_player->currentTrack().path.isEmpty()) {
        const qint64 sourceId = m_queueSourceKind == QStringLiteral("playlist") ? m_queueSourcePlaylistId : 0;
        m_playlistView->setNowPlaying(m_player->currentTrack().path, sourceId);
    }

    connect(m_playlistView, &PlaylistView::viewSettingsChanged, this, &MainWindow::savePlaylistViewSettings);
    connect(m_playerBar, &PlayerBar::playlistAddSongRequested, m_playlistView, &PlaylistView::addSongToCurrentPlaylist);
    connect(m_playerBar, &PlayerBar::playlistPlayRequested, m_playlistView, &PlaylistView::playCurrentPlaylist);
    connect(m_playerBar, &PlayerBar::playlistPlayNextRequested, m_playlistView, &PlaylistView::playNextCurrentPlaylist);
    connect(m_playerBar, &PlayerBar::playlistAddToQueueRequested, m_playlistView, &PlaylistView::addCurrentPlaylistToQueue);
    connect(m_playerBar, &PlayerBar::playlistRenameRequested, m_playlistView, &PlaylistView::renameCurrentPlaylist);
    connect(m_playerBar, &PlayerBar::playlistExportRequested, m_playlistView, &PlaylistView::exportCurrentPlaylist);
    connect(m_playerBar, &PlayerBar::playlistDeleteRequested, m_playlistView, &PlaylistView::deleteCurrentPlaylist);
    connect(m_playerBar, &PlayerBar::playlistMoveItemUpRequested, m_playlistView, &PlaylistView::moveCurrentItemUp);
    connect(m_playerBar, &PlayerBar::playlistMoveItemDownRequested, m_playlistView, &PlaylistView::moveCurrentItemDown);

    // Playlist view (key 5). Resolves stored paths against the live library;
    // playing a playlist replaces the queue (snapshotting the previous one).
    connect(m_playlistView, &PlaylistView::playPathsRequested, this, [this](const QStringList &paths, int startIndex) {
        const QVector<Track> tracks = tracksForPaths(paths);
        if (!tracks.isEmpty()) {
            const qint64 playlistId = m_playlistView->currentPlaylistId();
            const Playlist playlist = m_playlistDb != nullptr ? m_playlistDb->playlist(playlistId) : Playlist();
            replaceQueueWithTracks(tracks, startIndex,
                                   QStringLiteral("playlist"),
                                   playlistId,
                                   playlist.name);
        }
    });
    connect(m_playlistView, &PlaylistView::addPathsToQueueRequested, this, [this](const QStringList &paths) {
        enqueueTracksFromMenu(tracksForPaths(paths), QueueAddMode::Append, false);
    });
    connect(m_playlistView, &PlaylistView::playNextPathsRequested, this, [this](const QStringList &paths) {
        enqueueTracksFromMenu(tracksForPaths(paths), QueueAddMode::PlayNext, false);
    });
    connect(m_playlistView, &PlaylistView::addPathsToQueueTemporaryRequested, this, [this](const QStringList &paths) {
        enqueueTracksFromMenu(tracksForPaths(paths), QueueAddMode::Append, true);
    });
    connect(m_playlistView, &PlaylistView::playNextPathsTemporaryRequested, this, [this](const QStringList &paths) {
        enqueueTracksFromMenu(tracksForPaths(paths), QueueAddMode::PlayNext, true);
    });
    connect(m_playlistView, &PlaylistView::propertiesForPathRequested, this, [this](const QString &path) {
        showTrackProperties(m_database->trackForPath(path));
    });
    connect(m_playlistView, &PlaylistView::trackFlagChanged, this, [this](const QString &path, const QString &flag, bool on) {
        Track track;
        track.path = path;
        applyTrackFlag(track, flag, on);
    });
    connect(m_playlistView, &PlaylistView::startRadioRequested, this, &MainWindow::startRadioFromSeed);
    connect(m_playlistView, &PlaylistView::addSongRequested, this, &MainWindow::openPlaylistAddModal);
    connect(m_playlistView, &PlaylistView::importRequested, this, &MainWindow::openPlaylistImportDialog);
    connect(m_playlistView, &PlaylistView::importNewRequested, this, &MainWindow::importAsNewPlaylist);
    connect(m_playlistView, &PlaylistView::playlistFilesDropped, this, &MainWindow::importDroppedFiles);
    connect(m_playlistView, &PlaylistView::stopPlaylistImportRequested, this, &MainWindow::stopPlaylistImport);
    connect(m_playlistView, &PlaylistView::importInterruptRequested, this, &MainWindow::cancelDropImport);
    connect(m_playlistView, &PlaylistView::editItemRequested, this, &MainWindow::openPlaylistEditModal);
    connect(m_playlistView, &PlaylistView::resolveMultiMatchesRequested, this, &MainWindow::resolvePlaylistMultiMatches);
    connect(m_playlistView, &PlaylistView::playlistItemsChanged, this, &MainWindow::syncPlaylistBackedQueue);
    connect(m_playlistView, &PlaylistView::addToPlaylistRequested, this, [this](const QStringList &paths) {
        openAddToPlaylistDialog(tracksForPaths(paths));
    });
    connect(m_playlistView, &PlaylistView::removeAllMissingTracksRequested, this, &MainWindow::removeMissingTracks);
    connect(m_playlistView, &PlaylistView::playSavedQueueRequested, this, &MainWindow::playQueueSnapshotById);
    connect(m_playlistView, &PlaylistView::addSavedQueueToQueueRequested, this, &MainWindow::addQueueSnapshotByIdToQueue);
    connect(m_playlistView, &PlaylistView::playNextSavedQueueRequested, this, &MainWindow::playNextQueueSnapshotById);
    connect(m_playlistView, &PlaylistView::deleteSavedQueueRequested, this, &MainWindow::deleteQueueSnapshotById);
    connect(m_playlistView, &PlaylistView::trackRatingChanged, this, [this](const QString &path, int rating) {
        Track track = m_database != nullptr ? m_database->trackForPath(path) : Track();
        if (track.path.isEmpty()) {
            track.path = path;
        }
        applyTrackRating(track, rating, QStringLiteral("playlist"));
    });

    m_mainStack->addWidget(m_playlistView);
    applySharedTableSettings();
    return m_playlistView;
}

void MainWindow::switchMainView(MainView view)
{
    m_mainView = view;
    m_playerBar->setExplorerOptionsVisible(view == MainView::LibraryFileExplorer || view == MainView::FreeRoamFileExplorer);
    m_playerBar->setQueueViewLayoutActive(view == MainView::Queue);
    m_playerBar->setPlaylistViewActionsActive(view == MainView::Playlist);
    if (view == MainView::LibraryPanels) {
        m_libraryCenterStack->setCurrentWidget(m_centerSplitter);
        m_mainStack->setCurrentWidget(m_rootSplitter);
        if (m_panelSearch != nullptr) {
            installLibraryMainPanelTargets();
            m_panelSearch->activateForMainView();
        }
    } else if (view == MainView::LibraryMusicExplorer) {
        MusicExplorerView *musicExplorer = ensureMusicExplorerView();
        if (!m_currentArtist.isEmpty()) {
            musicExplorer->setAlbums(m_librarySource == LibrarySource::Mpd
                                         ? m_database->mpdAlbumsForArtist(m_currentArtist, mpdMusicDirectory())
                                         : m_database->albumsForArtist(m_currentArtist));
        }
        m_libraryCenterStack->setCurrentWidget(musicExplorer);
        m_mainStack->setCurrentWidget(m_rootSplitter);
        if (m_panelSearch != nullptr) {
            installMusicExplorerMainPanelTargets(musicExplorer);
            m_panelSearch->activateForMainView();
            m_panelSearch->setActivePanel(MainPanelId::Albums, false);
        }
        musicExplorer->albumNavigationWidget()->setFocus(Qt::OtherFocusReason);
    } else if (view == MainView::LibraryFileExplorer) {
        if (m_panelSearch != nullptr) {
            m_panelSearch->deactivateForNonMainView();
        }
        FileExplorerView *libraryFileExplorer = ensureLibraryFileExplorer();
        refreshLibraryFileExplorer();
        m_mainStack->setCurrentWidget(libraryFileExplorer);
    } else if (view == MainView::Search) {
        if (m_panelSearch != nullptr) {
            m_panelSearch->deactivateForNonMainView();
        }
        SearchView *searchView = ensureSearchView();
        m_mainStack->setCurrentWidget(searchView);
        searchView->ensureIndexLoaded(databasePath());
        searchView->focusSearchBox();
    } else if (view == MainView::Queue) {
        if (m_panelSearch != nullptr) {
            m_panelSearch->deactivateForNonMainView();
        }
        QueueScreen *queueScreen = ensureQueueScreen();
        m_mainStack->setCurrentWidget(queueScreen);
        queueScreen->focusQueue();
    } else if (view == MainView::Playlist) {
        if (m_panelSearch != nullptr) {
            m_panelSearch->deactivateForNonMainView();
        }
        PlaylistView *playlistView = ensurePlaylistView();
        playlistView->reloadPlaylists();
        m_mainStack->setCurrentWidget(playlistView);
        playlistView->focusPlaylistList();
    } else {
        if (m_panelSearch != nullptr) {
            m_panelSearch->deactivateForNonMainView();
        }
        FileExplorerView *freeRoamFileExplorer = ensureFreeRoamFileExplorer();
        freeRoamFileExplorer->setRootPath(m_freeRoamDirectory.isEmpty() ? QDir::homePath() : m_freeRoamDirectory);
        m_mainStack->setCurrentWidget(freeRoamFileExplorer);
    }
    saveMainWindowViewSettings();
}

bool MainWindow::showDemoArtist(const QString &artistName)
{
    if (artistName.trimmed().isEmpty()) {
        return false;
    }
    switchMainView(MainView::LibraryPanels);
    if (!m_artistSidebar->selectArtist(artistName)) {
        return false;
    }
    showArtist(artistName, /*forceReload=*/true, /*clearAlbumSelectionOnArtistChange=*/true);
    return true;
}

bool MainWindow::showDemoAlbum(const QString &artistName, const QString &albumTitle, QString *error)
{
    if (!showDemoArtist(artistName)) {
        if (error != nullptr) {
            *error = QStringLiteral("artist not found for demo capture: %1").arg(artistName);
        }
        return false;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    if (!m_albumGrid->selectAlbumTitleForDemo(albumTitle)) {
        if (error != nullptr) {
            *error = QStringLiteral("album not found for demo capture: %1 - %2").arg(artistName, albumTitle);
        }
        return false;
    }
    m_trackTable->setFocus(Qt::OtherFocusReason);
    return true;
}

bool MainWindow::showDemoNowPlaying(const QString &query, bool playing, double positionRatio, QString *error)
{
    const QString needle = query.trimmed();
    if (needle.isEmpty()) {
        return true;
    }

    Track track = m_database->trackForPath(needle);
    if (track.path.isEmpty()) {
        const QVector<Track> matches = m_database->searchTracksLike(needle, 1);
        if (!matches.isEmpty()) {
            track = matches.first();
        }
    }
    if (track.path.isEmpty()) {
        const QStringList terms = needle.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (!terms.isEmpty()) {
            const QVector<Track> candidates = m_database->searchTracksLike(terms.first(), 100);
            for (const Track &candidate : candidates) {
                const QString haystack = QStringLiteral("%1 %2 %3 %4")
                                             .arg(candidate.title,
                                                  candidate.artistName,
                                                  candidate.albumArtistName,
                                                  candidate.albumTitle)
                                             .toCaseFolded();
                const bool allTermsMatch = std::ranges::all_of(terms, [&haystack](const QString &term) {
                    return haystack.contains(term.toCaseFolded());
                });
                if (allTermsMatch) {
                    track = candidate;
                    break;
                }
            }
        }
    }
    if (track.path.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("track not found for demo now-playing query: %1").arg(query);
        }
        return false;
    }

    m_player->presentTrack(track);
    const qint64 durationMs = std::max<qint64>(0, track.durationMs);
    const double safeRatio = std::clamp(positionRatio, 0.0, 1.0);
    m_playerBar->setPosition(static_cast<qint64>(std::llround(static_cast<double>(durationMs) * safeRatio)), durationMs);
    m_playerBar->setPlaying(playing);
    return true;
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
    if (m_player->currentTrack().path.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Nothing is playing"), 3000);
        return;
    }
    revealTrackInLibrary(m_player->currentTrack());
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
    case MainView::LibraryMusicExplorer: {
        const QString artist = !track.albumArtistName.trimmed().isEmpty()
            ? track.albumArtistName
            : track.artistName;
        if (!artist.isEmpty()) {
            m_artistSidebar->selectArtist(artist);
            showArtist(artist, false, true);
            MusicExplorerView *musicExplorer = ensureMusicExplorerView();
            musicExplorer->selectAlbumTitle(track.albumTitle, true);
            musicExplorer->selectTrackByPath(track.path);
        }
        break;
    }
    case MainView::LibraryFileExplorer:
        ensureLibraryFileExplorer()->revealFile(track.path);
        break;
    case MainView::FreeRoamFileExplorer: {
        const QString resolved = resolvedReadPathForTrack(track);
        ensureFreeRoamFileExplorer()->revealFile(resolved.isEmpty() ? track.path : resolved);
        break;
    }
    case MainView::Search:
        // Search view has no reveal concept; switch to library panels and reveal there
        switchMainView(MainView::LibraryPanels);
        revealTrackInLibrary(track);
        break;
    case MainView::Queue:
    case MainView::Playlist:
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
    // On access: if this directory still holds enumerated-only placeholders, jump
    // it to the front of the background fill so its tags are read now.
    ensureDirectoryScanned(m_libraryExplorerDirectory);
}

void MainWindow::setFreeRoamDirectory(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir()) {
        return;
    }
    m_freeRoamDirectory = cleanDirectoryPath(path);
    if (m_freeRoamFileExplorer != nullptr) {
        m_freeRoamFileExplorer->setRootPath(m_freeRoamDirectory);
    }
    saveMainWindowViewSettings();
}

void MainWindow::refreshLibraryFileExplorer()
{
    if (m_libraryFileExplorer == nullptr) {
        return;
    }
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
    const QJsonArray trackValues = root.value(QStringLiteral("tracks")).toArray();
    QVector<Track> tracks;
    tracks.reserve(trackValues.size());
    for (const QJsonValue &value : trackValues) {
        Track track = trackFromJson(value.toObject());
        if (!track.path.isEmpty()) {
            const Track refreshed = m_database->trackForPath(track.path);
            if (!refreshed.path.isEmpty()) {
                track = refreshed;
            }
            tracks.push_back(track);
        }
    }

    const int savedIndex = root.value(QStringLiteral("index")).toInt(-1);
    m_player->resetQueue(tracks, savedIndex,
                         root.value(QStringLiteral("playNextInsertIndex")).toInt(savedIndex + 1));
    m_queueId = root.value(QStringLiteral("queueId")).toString();
    m_queueSourceKind = normalizedQueueSourceKind(root.value(QStringLiteral("queueSourceKind")).toString(QStringLiteral("queue")));
    m_queueSourcePlaylistId = root.value(QStringLiteral("queueSourcePlaylistId")).toString().toLongLong();
    if (m_queueSourcePlaylistId <= 0) {
        m_queueSourcePlaylistId = static_cast<qint64>(root.value(QStringLiteral("queueSourcePlaylistId")).toDouble(0));
    }
    m_queueSourceName = root.value(QStringLiteral("queueSourceName")).toString();
    if (m_player->queue().isEmpty()) {
        m_queueId.clear();
        m_queueSourceKind = QStringLiteral("queue");
        m_queueSourcePlaylistId = 0;
        m_queueSourceName.clear();
    } else {
        ensureCurrentQueueIdentity();
    }
    // resetQueue() does not emit queueChanged, so push the source-dependent UI
    // (playlist-mirror items, merge gating) directly for the restored queue.
    refreshQueueSourceDependentUi();
    m_queueStore->setSnapshot(m_player->queue(), m_player->queueIndex(),
                              m_player->queueIndex() + 1, m_player->playNextInsertIndex());
    m_rightSidebar->setCurrentIndex(m_player->queueIndex(), /*reveal=*/true);
    refreshPlayNextRange();
    if (m_player->queueIndex() >= 0) {
        m_player->presentTrack(m_player->queue().at(m_player->queueIndex()));
    }
}

void MainWindow::saveQueueState()
{
    if (m_queueStateSaveTimer != nullptr) {
        m_queueStateSaveTimer->stop();
    }
    if (!m_player->queue().isEmpty()) {
        ensureCurrentQueueIdentity();
    }
    QJsonArray tracks;
    for (const Track &track : m_player->queue()) {
        tracks.append(trackToJson(track));
    }

    QJsonObject root;
    root.insert(QStringLiteral("tracks"), tracks);
    root.insert(QStringLiteral("index"), m_player->queueIndex());
    root.insert(QStringLiteral("playNextInsertIndex"), m_player->playNextInsertIndex());
    root.insert(QStringLiteral("queueId"), m_queueId);
    root.insert(QStringLiteral("queueSourceKind"), m_queueSourceKind);
    root.insert(QStringLiteral("queueSourcePlaylistId"), QString::number(m_queueSourcePlaylistId));
    root.insert(QStringLiteral("queueSourceName"), m_queueSourceName);
    m_state->setSetting(QStringLiteral("queue.state"), QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

void MainWindow::scheduleQueueStateSave(bool immediate)
{
    if (immediate) {
        saveQueueState();
        return;
    }
    if (m_queueStateSaveTimer != nullptr) {
        m_queueStateSaveTimer->start();
    }
}

QJsonObject MainWindow::queueSnapshotObject(const QString &name, const QString &source) const
{
    QJsonArray tracks;
    for (const Track &track : m_player->queue()) {
        tracks.append(trackToJson(track));
    }
    QJsonObject snapshot;
    snapshot.insert(QStringLiteral("id"), m_queueId);
    snapshot.insert(QStringLiteral("name"), name);
    snapshot.insert(QStringLiteral("savedAt"), QDateTime::currentSecsSinceEpoch());
    snapshot.insert(QStringLiteral("index"), m_player->queueIndex());
    snapshot.insert(QStringLiteral("playNextInsertIndex"), m_player->playNextInsertIndex());
    snapshot.insert(QStringLiteral("sourceKind"), m_queueSourceKind);
    snapshot.insert(QStringLiteral("sourcePlaylistId"), QString::number(m_queueSourcePlaylistId));
    snapshot.insert(QStringLiteral("sourceName"), m_queueSourceName);
    const QString trimmedSource = source.trimmed();
    if (!trimmedSource.isEmpty()) {
        snapshot.insert(QStringLiteral("source"), trimmedSource);
    }
    snapshot.insert(QStringLiteral("tracks"), tracks);
    return snapshot;
}

QVector<Track> MainWindow::tracksFromSnapshotObject(const QJsonObject &snapshot) const
{
    QVector<Track> tracks;
    const QJsonArray array = snapshot.value(QStringLiteral("tracks")).toArray();
    tracks.reserve(array.size());
    for (const QJsonValue &value : array) {
        Track track = trackFromJson(value.toObject());
        if (track.path.isEmpty()) {
            continue;
        }
        // Re-resolve against the live library so stale snapshot metadata (ratings,
        // technical fields) is refreshed; keep the snapshot copy if it is gone.
        const Track refreshed = m_database->trackForPath(track.path);
        tracks.push_back(refreshed.path.isEmpty() ? track : refreshed);
    }
    return tracks;
}

QJsonObject MainWindow::loadQueueSnapshotsRoot() const
{
    return QJsonDocument::fromJson(m_state->setting(QStringLiteral("queue.snapshots")).toUtf8()).object();
}

void MainWindow::saveQueueSnapshotsRoot(const QJsonObject &root)
{
    m_state->setSetting(QStringLiteral("queue.snapshots"),
                        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    refreshSavedQueuePlaylistEntries();
}

QVector<SavedQueuePlaylistEntry> MainWindow::savedQueuePlaylistEntries() const
{
    QVector<SavedQueuePlaylistEntry> entries;
    const QJsonObject root = loadQueueSnapshotsRoot();
    int ordinal = 1;
    const auto appendSnapshot = [this, &entries, &ordinal](const QJsonObject &snapshot) {
        const QString id = snapshot.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            return;
        }
        const QVector<Track> tracks = tracksFromSnapshotObject(snapshot);
        if (tracks.isEmpty()) {
            return;
        }
        SavedQueuePlaylistEntry entry;
        entry.id = id;
        entry.snapshotKey = queueSnapshotKey(snapshot);
        entry.savedAt = queueSnapshotSavedAt(snapshot);
        entry.name = queueSnapshotLabel(snapshot, defaultSavedQueueName(ordinal++));
        entry.meta = queueSnapshotTimestamp(entry.savedAt);
        entry.items.reserve(tracks.size());
        for (int i = 0; i < tracks.size(); ++i) {
            PlaylistItem item = playlistItemFromTrack(tracks.at(i), QString());
            item.ordinal = i;
            entry.items.push_back(item);
        }
        entries.push_back(entry);
    };

    for (const QJsonObject &snapshot : automaticQueueSnapshotsFromRoot(root)) {
        appendSnapshot(snapshot);
    }
    for (const QJsonValue &value : root.value(QStringLiteral("saved")).toArray()) {
        appendSnapshot(value.toObject());
    }
    return entries;
}

QJsonObject MainWindow::queueSnapshotByKey(const QString &keyOrId) const
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

void MainWindow::refreshSavedQueuePlaylistEntries()
{
    if (m_playlistView != nullptr) {
        m_playlistView->setSavedQueueEntries(savedQueuePlaylistEntries());
    }
}

void MainWindow::playQueueSnapshotById(const QString &id, int startIndex)
{
    const QJsonObject snapshot = queueSnapshotByKey(id);
    const QVector<Track> tracks = tracksFromSnapshotObject(snapshot);
    if (tracks.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Saved queue is empty"), 3000);
        return;
    }
    pushCurrentQueueToBacklog(QString());
    QJsonObject root = loadQueueSnapshotsRoot();
    removeQueueSnapshotFromRoot(&root, queueSnapshotKey(snapshot));
    saveQueueSnapshotsRoot(root);
    adoptQueueSnapshot(snapshot, tracks, std::clamp(startIndex, 0, static_cast<int>(tracks.size()) - 1));
}

void MainWindow::addQueueSnapshotByIdToQueue(const QString &id)
{
    const QVector<Track> tracks = tracksFromSnapshotObject(queueSnapshotByKey(id));
    if (tracks.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Saved queue is empty"), 3000);
        return;
    }
    addTracksToQueue(tracks);
}

void MainWindow::playNextQueueSnapshotById(const QString &id)
{
    const QVector<Track> tracks = tracksFromSnapshotObject(queueSnapshotByKey(id));
    if (tracks.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Saved queue is empty"), 3000);
        return;
    }
    playNextTracks(tracks);
}

void MainWindow::deleteQueueSnapshotById(const QString &id)
{
    if (id.isEmpty()) {
        return;
    }
    QJsonObject root = loadQueueSnapshotsRoot();
    const QJsonObject snapshot = queueSnapshotByKey(id);
    const QString name = queueSnapshotLabel(snapshot, QStringLiteral("saved queue"));
    const QString meta = queueSnapshotTimestamp(queueSnapshotSavedAt(snapshot));
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Delete saved queue"));
    box.setText(QStringLiteral("Delete \"%1\"?").arg(name));
    if (!meta.isEmpty()) {
        box.setInformativeText(QStringLiteral("Saved at %1. This only removes the saved queue entry; it does not change the current queue.").arg(meta));
    } else {
        box.setInformativeText(QStringLiteral("This only removes the saved queue entry; it does not change the current queue."));
    }
    QPushButton *deleteButton = box.addButton(QStringLiteral("Delete"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != deleteButton) {
        return;
    }

    removeQueueSnapshotFromRoot(&root, id);
    saveQueueSnapshotsRoot(root);
    statusBar()->showMessage(QStringLiteral("Deleted \"%1\"").arg(name), 4000);
}

int MainWindow::savedQueueLimitSetting() const
{
    return savedQueueUnlimitedSetting() ? 0 : kAutomaticSavedQueueLimit;
}

int MainWindow::radioSavedQueueLimitSetting() const
{
    return radioSavedQueueUnlimitedSetting() ? 0 : kAutomaticSavedQueueLimit;
}

bool MainWindow::savedQueueUnlimitedSetting() const
{
    const QString value = m_state->setting(QStringLiteral("queue.savedQueueUnlimited")).trimmed();
    return value == QStringLiteral("1") || value == QStringLiteral("true");
}

bool MainWindow::radioSavedQueueUnlimitedSetting() const
{
    const QString value = m_state->setting(QStringLiteral("queue.radioSavedQueueUnlimited")).trimmed();
    return value == QStringLiteral("1") || value == QStringLiteral("true");
}

void MainWindow::configureSavedQueueLimit()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Saved queue limits"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *regularLimit = new QCheckBox(
        QStringLiteral("Limit regular automatic saved queues to %1").arg(kAutomaticSavedQueueLimit),
        &dialog);
    regularLimit->setChecked(!savedQueueUnlimitedSetting());
    auto *radioLimit = new QCheckBox(
        QStringLiteral("Limit radio session saved queues to %1").arg(kAutomaticSavedQueueLimit),
        &dialog);
    radioLimit->setChecked(!radioSavedQueueUnlimitedSetting());
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(regularLimit);
    layout->addWidget(radioLimit);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    m_state->setSetting(QStringLiteral("queue.savedQueueUnlimited"), regularLimit->isChecked() ? QStringLiteral("0") : QStringLiteral("1"));
    m_state->setSetting(QStringLiteral("queue.radioSavedQueueUnlimited"), radioLimit->isChecked() ? QStringLiteral("0") : QStringLiteral("1"));
    QJsonObject root = loadQueueSnapshotsRoot();
    QJsonArray backlog = queueBacklogFromRoot(root);
    QJsonArray radioBacklog = radioQueueBacklogFromRoot(root);
    trimQueueBacklog(&backlog, savedQueueLimitSetting());
    trimQueueBacklog(&radioBacklog, radioSavedQueueLimitSetting());
    root.remove(QStringLiteral("previous"));
    root.insert(QStringLiteral("backlog"), backlog);
    root.insert(QStringLiteral("radioBacklog"), radioBacklog);
    saveQueueSnapshotsRoot(root);
    statusBar()->showMessage(QStringLiteral("Saved queue limits updated"), 4000);
}

void MainWindow::ensureCurrentQueueIdentity()
{
    if (m_queueId.isEmpty()) {
        m_queueId = newQueueIdentity();
    }
    m_queueSourceKind = normalizedQueueSourceKind(m_queueSourceKind);
    if (m_queueSourceKind != QStringLiteral("playlist")) {
        m_queueSourcePlaylistId = 0;
    }
}

bool MainWindow::currentQueueBacklogEligible() const
{
    return !m_player->queue().isEmpty() && m_queueSourceKind == QStringLiteral("queue");
}

void MainWindow::pushCurrentQueueToBacklog(const QString &name, const QString &source)
{
    if (!currentQueueBacklogEligible()) {
        return;
    }
    ensureCurrentQueueIdentity();
    QJsonObject root = loadQueueSnapshotsRoot();
    const QString snapshotId = m_queueId;
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

void MainWindow::snapshotCurrentQueueAsPrevious(const QString &source)
{
    pushCurrentQueueToBacklog(QString(), source);
}

void MainWindow::markQueueAsSpontaneous(const QString &id)
{
    m_queueId = id.isEmpty() ? newQueueIdentity() : id;
    m_queueSourceKind = QStringLiteral("queue");
    m_queueSourcePlaylistId = 0;
    m_queueSourceName.clear();
}

void MainWindow::appendTracksToCurrentPlaylist(const QVector<Track> &tracks)
{
    if (m_playlistDb == nullptr || m_queueSourcePlaylistId <= 0 || tracks.isEmpty()) {
        return;
    }
    QSet<QString> existingPaths;
    for (const PlaylistItem &item : m_playlistDb->items(m_queueSourcePlaylistId)) {
        if (!item.trackPath.isEmpty()) {
            existingPaths.insert(item.trackPath);
        }
    }
    bool changed = false;
    for (const Track &track : tracks) {
        if (track.path.isEmpty() || existingPaths.contains(track.path)) {
            continue;
        }
        changed = m_playlistDb->addItem(m_queueSourcePlaylistId, playlistItemFromTrack(track, QString())) > 0 || changed;
        existingPaths.insert(track.path);
    }
    if (changed && m_playlistView != nullptr) {
        m_playlistView->reloadItems();
        m_playlistView->reloadPlaylists();
    }
}

void MainWindow::prepareQueueForTrackAddition(const QVector<Track> &tracks)
{
    if (tracks.isEmpty()) {
        return;
    }
    if (m_queueSourceKind == QStringLiteral("playlist")) {
        if (!m_suppressPlaylistMirror) {
            appendTracksToCurrentPlaylist(tracks);
        }
        return;
    }
    if (m_queueSourceKind == QStringLiteral("album")) {
        markQueueAsSpontaneous();
        return;
    }
    ensureCurrentQueueIdentity();
}

void MainWindow::prepareQueueForLibraryInjection(const Track &track)
{
    if (track.path.isEmpty()) {
        return;
    }
    // A library-shuffle injection is the player appending a fresh track it chose
    // itself, never a user edit. For a playlist-backed queue the track joins the
    // live queue only — it is deliberately NOT mirrored into the playlist, since
    // it was never part of that playlist. The playlist link stays intact so the
    // user's own subsequent adds still mirror (and still prompt) as usual.
    if (m_queueSourceKind == QStringLiteral("playlist")) {
        return;
    }
    // For album / standalone queues the injected foreign track means the queue is
    // no longer a faithful copy of its source, so apply the same identity policy a
    // user add would: drop the album identity, or anchor a standalone queue id.
    if (m_queueSourceKind == QStringLiteral("album")) {
        markQueueAsSpontaneous();
        return;
    }
    ensureCurrentQueueIdentity();
}

bool MainWindow::queueIsPlaylistSourced() const
{
    return m_queueSourceKind == QStringLiteral("playlist") && m_queueSourcePlaylistId > 0;
}

void MainWindow::unlinkQueueFromPlaylist()
{
    if (!queueIsPlaylistSourced()) {
        return;
    }
    // Keep the tracks and the same queue identity; just sever the playlist link so
    // later edits stop mirroring (and stop prompting). The playlist itself is left
    // untouched — this only affects the live queue.
    m_queueSourceKind = QStringLiteral("queue");
    m_queueSourcePlaylistId = 0;
    m_queueSourceName.clear();
    // The "don't ask again" memory was keyed to that playlist; drop it so a future
    // playlist-backed queue starts fresh.
    m_mirrorPromptSuppressedForPlaylist = 0;
    m_rememberedMirrorChoice = PlaylistMirrorChoice::AddToPlaylist;
    refreshQueueSourceDependentUi();
    scheduleQueueStateSave();
}

PlaylistMirrorChoice MainWindow::promptPlaylistMirror(int trackCount)
{
    // "Don't ask again" stays in effect only while the same playlist backs the
    // queue, and replays whichever button the user ticked it on — so "Queue only"
    // keeps queueing and "Add to playlist" keeps saving, instead of always saving.
    if (m_mirrorPromptSuppressedForPlaylist == m_queueSourcePlaylistId) {
        return m_rememberedMirrorChoice;
    }

    const QString playlist = m_queueSourceName.isEmpty()
        ? QStringLiteral("the current playlist")
        : QStringLiteral("“%1”").arg(m_queueSourceName);
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("Add to playlist queue"));
    box.setText(QStringLiteral("The queue is mirroring %1.").arg(playlist));
    box.setInformativeText(trackCount == 1
        ? QStringLiteral("Adding this track to the queue will also save it to the playlist. "
                         "Add it to the queue only instead?")
        : QStringLiteral("Adding these %1 tracks to the queue will also save them to the playlist. "
                         "Add them to the queue only instead?").arg(trackCount));
    QPushButton *addToPlaylist = box.addButton(QStringLiteral("Add to playlist"), QMessageBox::AcceptRole);
    QPushButton *queueOnly = box.addButton(QStringLiteral("Queue only (don't save)"), QMessageBox::ActionRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(addToPlaylist);
    auto *dontAsk = new QCheckBox(QStringLiteral("Don't ask again while this playlist is the queue"), &box);
    box.setCheckBox(dontAsk);
    box.exec();

    QAbstractButton *clicked = box.clickedButton();
    if (clicked == nullptr || (clicked != addToPlaylist && clicked != queueOnly)) {
        return PlaylistMirrorChoice::Cancel;
    }
    const PlaylistMirrorChoice choice = clicked == queueOnly
        ? PlaylistMirrorChoice::QueueOnly
        : PlaylistMirrorChoice::AddToPlaylist;
    if (dontAsk->isChecked()) {
        m_mirrorPromptSuppressedForPlaylist = m_queueSourcePlaylistId;
        m_rememberedMirrorChoice = choice;
    }
    return choice;
}

void MainWindow::enqueueTracksFromMenu(const QVector<Track> &tracks, QueueAddMode mode, bool temporary)
{
    if (tracks.isEmpty()) {
        return;
    }
    bool suppressMirror = temporary;
    if (!temporary && queueIsPlaylistSourced()) {
        switch (promptPlaylistMirror(static_cast<int>(tracks.size()))) {
        case PlaylistMirrorChoice::Cancel:
            return;
        case PlaylistMirrorChoice::QueueOnly:
            suppressMirror = true;
            break;
        case PlaylistMirrorChoice::AddToPlaylist:
            break;
        }
    }
    m_suppressPlaylistMirror = suppressMirror;
    if (mode == QueueAddMode::PlayNext) {
        playNextTracks(tracks);
    } else {
        addTracksToQueue(tracks);
    }
    m_suppressPlaylistMirror = false;
}

void MainWindow::adoptQueueSnapshot(const QJsonObject &snapshot, const QVector<Track> &tracks, int playIndex)
{
    if (tracks.isEmpty()) {
        return;
    }
    m_queueId = snapshot.value(QStringLiteral("id")).toString();
    m_queueSourceKind = normalizedQueueSourceKind(snapshot.value(QStringLiteral("sourceKind")).toString(QStringLiteral("queue")));
    m_queueSourcePlaylistId = snapshot.value(QStringLiteral("sourcePlaylistId")).toString().toLongLong();
    if (m_queueSourcePlaylistId <= 0) {
        m_queueSourcePlaylistId = static_cast<qint64>(snapshot.value(QStringLiteral("sourcePlaylistId")).toDouble(0));
    }
    m_queueSourceName = snapshot.value(QStringLiteral("sourceName")).toString();
    ensureCurrentQueueIdentity();
    m_player->resetQueue(tracks);
    m_queueStore->setTracks(m_player->queue());
    // resetQueue() does not emit queueChanged, so syncQueueState() won't run here;
    // push the source-dependent UI (playlist-mirror items, merge gating) directly.
    refreshQueueSourceDependentUi();
    const int start = std::clamp(playIndex, 0, static_cast<int>(m_player->queue().size()) - 1);
    playQueueIndex(start);
}

void MainWindow::replaceQueueWithTracks(const QVector<Track> &tracks, int playIndex,
                                        const QString &sourceKind,
                                        qint64 sourcePlaylistId,
                                        const QString &sourceName)
{
    if (tracks.isEmpty()) {
        return;
    }
    // Preserve only real spontaneous queues. Source-backed album/playlist
    // playback is reconstructible from its source until it is mutated.
    snapshotCurrentQueueAsPrevious();

    m_player->resetQueue(tracks);
    m_queueId = newQueueIdentity();
    m_queueSourceKind = normalizedQueueSourceKind(sourceKind);
    m_queueSourcePlaylistId = m_queueSourceKind == QStringLiteral("playlist") ? sourcePlaylistId : 0;
    m_queueSourceName = sourceName;
    m_queueStore->setTracks(m_player->queue());
    // resetQueue() does not emit queueChanged, so syncQueueState() won't run here;
    // push the source-dependent UI (playlist-mirror items, merge gating) directly.
    refreshQueueSourceDependentUi();
    const int start = std::clamp(playIndex, 0, static_cast<int>(m_player->queue().size()) - 1);
    playQueueIndex(start);
}

void MainWindow::restorePreviousQueue()
{
    const QJsonObject root = loadQueueSnapshotsRoot();
    const QVector<QueueSnapshotChoice> choices = queueSnapshotChoicesFromRoot(root);
    if (choices.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("No saved queues to restore"), 3000);
        return;
    }
    const int index = chooseQueueSnapshot(this,
                                          QStringLiteral("Restore queue"),
                                          QStringLiteral("Restore saved queue:"),
                                          choices);
    if (index < 0) {
        return;
    }
    const QueueSnapshotChoice choice = choices.at(index);
    const QJsonObject snapshot = choice.snapshot;
    const QVector<Track> tracks = tracksFromSnapshotObject(snapshot);
    if (tracks.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Saved queue is empty"), 3000);
        return;
    }
    // The queue being displaced returns to the backlog under its existing id, so
    // restore can toggle without manufacturing duplicate queue identities.
    pushCurrentQueueToBacklog(QString());
    QJsonObject updatedRoot = loadQueueSnapshotsRoot();
    removeQueueSnapshotFromRoot(&updatedRoot, choice.key);
    saveQueueSnapshotsRoot(updatedRoot);
    const int restoreIndex = std::clamp(snapshot.value(QStringLiteral("index")).toInt(0),
                                        0, static_cast<int>(tracks.size()) - 1);
    adoptQueueSnapshot(snapshot, tracks, restoreIndex);
    statusBar()->showMessage(QStringLiteral("Restored \"%1\" (%2 tracks)").arg(choice.name).arg(tracks.size()), 4000);
}

void MainWindow::saveCurrentQueueAs()
{
    if (m_player->queue().isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Queue is empty"), 3000);
        return;
    }
    QJsonObject root = loadQueueSnapshotsRoot();
    const QString defaultName = nextSavedQueueName(root);
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Save queue as"),
                                               QStringLiteral("Queue-playlist name:"),
                                               QLineEdit::Normal, defaultName, &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    QJsonArray saved = root.value(QStringLiteral("saved")).toArray();
    // Replace any existing snapshot with the same name, else append.
    ensureCurrentQueueIdentity();
    QJsonObject snapshot = queueSnapshotObject(name);
    snapshot.insert(QStringLiteral("id"), newQueueIdentity());
    bool replaced = false;
    for (qsizetype i = 0; i < saved.size(); ++i) {
        if (saved.at(i).toObject().value(QStringLiteral("name")).toString() == name) {
            saved.replace(i, snapshot);
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        saved.append(snapshot);
    }
    root.insert(QStringLiteral("saved"), saved);
    saveQueueSnapshotsRoot(root);
    statusBar()->showMessage(QStringLiteral("Saved queue \"%1\"").arg(name), 4000);
}

void MainWindow::mergeSavedQueueViaPlayNext()
{
    const QJsonObject root = loadQueueSnapshotsRoot();
    const QVector<QueueSnapshotChoice> choices = queueSnapshotChoicesFromRoot(root);
    if (choices.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("No saved queues"), 3000);
        return;
    }
    const int index = chooseQueueSnapshot(this,
                                          QStringLiteral("Merge saved queue"),
                                          QStringLiteral("Insert via play-next:"),
                                          choices);
    if (index < 0) {
        return;
    }
    const QueueSnapshotChoice choice = choices.at(index);
    const QVector<Track> tracks = tracksFromSnapshotObject(choice.snapshot);
    if (tracks.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Saved queue is empty"), 3000);
        return;
    }
    playNextTracks(tracks);
    statusBar()->showMessage(QStringLiteral("Merged \"%1\" (%2 tracks)").arg(choice.name).arg(tracks.size()), 4000);
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
    const QJsonObject playlistSettings = QJsonDocument::fromJson(m_state->setting(QStringLiteral("playlistView.view")).toUtf8()).object();
    const int headerHeight = sharedSettings.value(QStringLiteral("headerHeight")).toInt(trackSettings.value(QStringLiteral("headerHeight")).toInt(sidebarSettings.value(QStringLiteral("headerHeight")).toInt(playlistSettings.value(QStringLiteral("headerHeight")).toInt(20))));
    m_trackTable->setHeaderHeight(headerHeight);
    if (m_musicExplorerView != nullptr) {
        QScopedValueRollback<bool> applying(m_applyingTrackTableViewSettings, true);
        m_musicExplorerView->applyTrackTableViewSettingsJson(m_state->setting(QStringLiteral("trackTable.view")));
    }
    m_rightSidebar->setHeaderHeight(headerHeight);
    if (m_playlistView != nullptr) {
        m_playlistView->setHeaderHeight(headerHeight);
    }

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
    // The persisted hw:N can drift across reboots / device launch order; recover
    // the current index from the stable device id when we have one.
    if (m_playbackProfile.mode == QStringLiteral("bit-perfect")
        && !m_playbackProfile.deviceId.isEmpty()) {
        if (const auto dev = AudioDeviceControl::findByStableId(m_playbackProfile.deviceId);
            dev && !dev->hwPath.isEmpty()) {
            m_playbackProfile.device = dev->hwPath;
        }
    }
    m_playback->setProfile(m_playbackProfile);
    m_playerBar->setVolumeControlEnabled(m_playbackProfile.softwareVolume);
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
    const bool trackChanged = m_player->currentTrack().path != m_lastSavedPlaybackTrackPath;
    if (!force && !meaningfulPositionChange && !stateChanged && !trackChanged) {
        return;
    }

    QJsonObject root;
    root.insert(QStringLiteral("queueIndex"), m_player->queueIndex());
    root.insert(QStringLiteral("trackPath"), m_player->currentTrack().path);
    root.insert(QStringLiteral("positionMs"), QString::number(positionMs));
    root.insert(QStringLiteral("state"), stateName);
    m_state->setSetting(QStringLiteral("playback.state"), QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    m_lastSavedPlaybackPositionMs = positionMs;
    m_lastSavedPlaybackTrackPath = m_player->currentTrack().path;
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
    if (queueIndex < 0 || queueIndex >= m_player->queue().size() || trackPath.isEmpty()
        || m_player->queue().at(queueIndex).path != trackPath) {
        return;
    }
    if (state != QStringLiteral("playing") && state != QStringLiteral("paused")) {
        return;
    }

    resumePlaybackAt(queueIndex, positionMs, /*playing=*/state == QStringLiteral("playing"),
                     /*settleDelayMs=*/250);
}

void MainWindow::resumePlaybackAt(int queueIndex, qint64 positionMs, bool playing, int settleDelayMs)
{
    // For a paused resume, load the source directly into PAUSED state so the
    // audio device is never opened and no audio blip occurs. For a playing
    // resume, start normally; the backend will be in Playing state by the time
    // the settle timer fires.
    // Either way, skip scrobbler notification here — the backend transiently
    // reports states while starting up. Notify only when the session is
    // genuinely playing (checked in the settle timer below).
    playQueueIndex(queueIndex, /*notifyScrobbler=*/false, /*startPaused=*/!playing);
    QTimer::singleShot(settleDelayMs, this, [this, positionMs, playing]() {
        if (positionMs > 0) {
            m_playback->seek(positionMs);
            m_playerBar->setPosition(positionMs, std::max<qint64>(m_playback->duration(), m_player->currentTrack().durationMs));
            m_mpris->setPositionMs(positionMs);
        }
        // Resume the scrobble session at the restored position rather than
        // starting a fresh one (which would lose already-elapsed time and
        // re-scrobble tracks that crossed the threshold before the restart).
        // For a paused resume, set up the session now so that when the user
        // later hits play, playbackStateChanged() continues the session
        // (including sending a rate-limited "now playing") without waiting
        // for the next track.
        const bool actuallyPlaying = playing
            && m_playback->state() == PlaybackBackend::State::Playing;
        m_core->resumeScrobblers(m_player->currentTrack(), std::max<qint64>(0, positionMs), actuallyPlaying);
        const bool restoredIntent = actuallyPlaying
            || (!playing && m_playback->state() == PlaybackBackend::State::Paused);
        if (restoredIntent && m_profileTakeoverResumePending
            && m_player->currentTrack().path == m_profileTakeoverTrackPath) {
            m_profileTakeoverResumePending = false;
        }
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
    const PlaybackProfile previous = m_playbackProfile;
    applyProfileTransition(previous, dialog.profile(),
                           dialog.releasedDeviceHw(), dialog.releasedSinkNodeId());
}

void MainWindow::applyProfileTransition(const PlaybackProfile &previous, const PlaybackProfile &next,
                                        const QString &manuallyReleasedHw, int manualSinkNodeId)
{
    // The single hand-off path shared by every output transition — the profile
    // dialog, the "Release device" action, and the idle auto-release timer. It
    // frees any exclusively-held card before the new sink is built and resumes the
    // current track through it. Keeping it in one place is what makes the delicate
    // bit-perfect→shared PipeWire hand-back behave identically everywhere.
    m_playbackProfile = next;
    savePlaybackProfile();
    m_playerBar->setVolumeControlEnabled(m_playbackProfile.softwareVolume);

    // Capture what to resume after the output is rebuilt: the same track at the
    // same elapsed position and play/pause state it had before the switch.
    const PlaybackBackend::State priorState = m_playback->state();
    const int queueIndex = m_player->queueIndex();
    const qint64 positionMs = m_playback->position();
    const bool wasActive = queueIndex >= 0
        && (priorState == PlaybackBackend::State::Playing
            || priorState == PlaybackBackend::State::Paused);
    const bool wasPlaying = priorState == PlaybackBackend::State::Playing;
    if (next.mode == QStringLiteral("bit-perfect") && wasActive) {
        m_profileTakeoverResumePending = true;
        m_profileTakeoverTrackPath = m_player->currentTrack().path;
        m_profileTakeoverPositionMs = positionMs;
        m_profileTakeoverWasPlaying = wasPlaying;
    }

    // Free a card we were holding exclusively *before* the new sink is built, so
    // a shared sink targeting that same device isn't left silent (and a stale
    // bit-perfect/DSD takeover isn't stranded).
    int previousSinkNodeId = manualSinkNodeId;
    QString releasedHw = releaseDeviceForProfileSwitch(previous, next, &previousSinkNodeId);
    if (releasedHw.isEmpty() && next.mode == QStringLiteral("shared")
        && !manuallyReleasedHw.isEmpty() && manuallyReleasedHw == previous.device) {
        // A caller (the dialog) can hand the card back before arriving here. Its
        // saved pre-release node id lets the hand-off wait distinguish the old sink
        // from the one WirePlumber will create after the change settles.
        releasedHw = manuallyReleasedHw;
    }
    applyOutputProfile(next, releasedHw, previousSinkNodeId,
                       queueIndex, positionMs, wasActive, wasPlaying);
}

void MainWindow::applyOutputProfile(const PlaybackProfile &next, const QString &releasedHw,
                                    int previousSinkNodeId, int queueIndex, qint64 positionMs,
                                    bool wasActive, bool wasPlaying)
{
    PlaybackProfile applied = next;
    if (!releasedHw.isEmpty() && applied.mode == QStringLiteral("shared")) {
        // Keep the physical release separate from the persisted shared profile:
        // this one rebuilt sink must be pinned to the card we just returned.
        // Otherwise WirePlumber can select the first available loopback during
        // the device's delayed graph rebuild, leaving the new stream silent.
        applied.device = releasedHw;
    }
    const auto apply = [this, applied, queueIndex, positionMs, wasActive, wasPlaying]() {
        m_playback->setProfile(applied);
        if (wasActive) {
            // Re-open the current track at the captured position; the settle
            // delay lets the freshly-built sink preroll before the seek.
            resumePlaybackAt(queueIndex, positionMs, wasPlaying, /*settleDelayMs=*/600);
        }
        statusBar()->showMessage(QStringLiteral("Playback output updated"), 3000);
    };

    // Figure out which device-ownership change (if any) must settle first:
    //  - released a card (→ shared): wait for PipeWire to reclaim it (held=true).
    //  - switching to bit-perfect: wait for its device to be free/taken-over.
    QString waitHw;
    bool wantHeld = false;
    if (!releasedHw.isEmpty()) {
        waitHw = releasedHw;
        wantHeld = true;
    } else if (next.mode == QStringLiteral("bit-perfect") && !next.device.isEmpty()) {
        // No takeover has happened yet: open immediately and let the existing
        // busy-device error offer the explicit Take over action. Waiting here
        // cannot free a card PipeWire still owns; it only delays that action.
        const auto dev = AudioDeviceControl::findByHwPath(next.device);
        if (dev && !dev->heldByPipeWire()) {
            waitHw = next.device;
            wantHeld = false;
        }
    }

    if (waitHw.isEmpty()) {
        apply();
        return;
    }
    statusBar()->showMessage(QStringLiteral("Switching output…"), 10000);
    waitForDeviceOwnership(waitHw, wantHeld, previousSinkNodeId, apply);
}

void MainWindow::waitForDeviceOwnership(const QString &hw, bool wantHeld, int previousSinkNodeId,
                                        std::function<void()> done)
{
    const auto ready = [hw, wantHeld, previousSinkNodeId]() {
        if (wantHeld) {
            // We handed the card back to PipeWire: the profile flips instantly,
            // but the sink node (and any loopbacks re-linking off it) lag a few
            // seconds. Wait for the node so shared playback isn't rebuilt onto a
            // not-yet-present device and left silent.
            const int sinkNodeId = AudioDeviceControl::sinkNodeId(hw);
            return sinkNodeId >= 0 && (previousSinkNodeId < 0 || sinkNodeId != previousSinkNodeId);
        }
        // A profile can report Off before the old Audio/Sink is actually gone.
        // Wait for both conditions before opening the ALSA device directly.
        const auto dev = AudioDeviceControl::findByHwPath(hw);
        return !dev || (!dev->heldByPipeWire() && !AudioDeviceControl::sinkNodeReady(hw));
    };
    if (ready()) {
        done();
        return;
    }
    // Poll once a second; the card usually changes hands within 2–5 s. Give up
    // after ~10 s and apply anyway (best effort — a stuck device then surfaces
    // its own error/takeover affordance rather than hanging the switch).
    auto *timer = new QTimer(this);
    timer->setInterval(1000);
    auto ticksLeft = std::make_shared<int>(10);
    connect(timer, &QTimer::timeout, this, [timer, ready, done, ticksLeft]() {
        if (ready() || --(*ticksLeft) <= 0) {
            timer->stop();
            timer->deleteLater();
            done();
        }
    });
    timer->start();
}

QString MainWindow::releaseDeviceForProfileSwitch(const PlaybackProfile &previous,
                                                  const PlaybackProfile &next,
                                                  int *previousSinkNodeId)
{
    // Which card, if any, were we holding exclusively under the old profile?
    // A tracked DSD takeover takes precedence; otherwise a bit-perfect profile
    // implies its configured hw device is (or will be) held Off.
    QString heldHw = m_takenOverDsdDevice;
    int restoreProfile = m_takenOverDsdRestoreProfile;
    if (heldHw.isEmpty() && previous.mode == QStringLiteral("bit-perfect")) {
        heldHw = previous.device;
        restoreProfile = -1;  // restore to the best audio profile
    }
    if (heldHw.isEmpty()) {
        return {};
    }
    // Still bit-perfect on the very same card: keep the exclusive hold.
    if (next.mode == QStringLiteral("bit-perfect") && next.device == heldHw) {
        return {};
    }
    bool released = false;
    bool alreadyReleased = false;
    const auto dev = AudioDeviceControl::findByHwPath(heldHw);
    if (!dev || dev->heldByPipeWire()) {
        // Already owned by PipeWire (nothing to hand back) — just drop our
        // bookkeeping below if it pointed here.
        alreadyReleased = dev && next.mode == QStringLiteral("shared");
        if (alreadyReleased
            && (m_playback->state() == PlaybackBackend::State::Playing
                || m_playback->state() == PlaybackBackend::State::Paused)) {
            // The dialog returned this card while the old direct pipeline was
            // still active. Stop it before waiting for the replacement sink.
            m_playback->stop();
        }
    } else {
        // Do not let the old direct ALSA pipeline continue while PipeWire is
        // reclaiming this card. Apart from making the transition sound as if it
        // has not started, that leaves two owners racing the same hardware and
        // can delay WirePlumber's sink recreation. The caller captured the
        // queue position/state before arriving here and will reopen it only
        // after the replacement shared sink is usable.
        if (m_playback->state() == PlaybackBackend::State::Playing
            || m_playback->state() == PlaybackBackend::State::Paused) {
            m_playback->stop();
        }
        if (previousSinkNodeId != nullptr && *previousSinkNodeId < 0) {
            *previousSinkNodeId = AudioDeviceControl::sinkNodeId(heldHw);
        }
        QString error;
        if (AudioDeviceControl::release(*dev, restoreProfile, &error)) {
            statusBar()->showMessage(
                QStringLiteral("Released %1 back to PipeWire").arg(dev->description), 3000);
            released = true;
        } else {
            statusBar()->showMessage(
                QStringLiteral("Could not release %1: %2").arg(heldHw, error), 6000);
            return {};
        }
    }
    if (m_takenOverDsdDevice == heldHw) {
        m_takenOverDsdReleaseTimer->stop();
        m_takenOverDsdDevice.clear();
        m_takenOverDsdRestoreProfile = -1;
        m_playerBar->setReleaseDeviceVisible(false);
    }
    // A shared target needs the freshly recreated sink even if the dialog
    // released it just before OK. A BP→different-BP change instead waits for
    // the new direct card in applyOutputProfile().
    return next.mode == QStringLiteral("shared") && (released || alreadyReleased)
        ? heldHw : QString();
}

void MainWindow::attemptDeviceTakeover()
{
    const auto dev = AudioDeviceControl::findByHwPath(m_playbackProfile.device);
    if (!dev) {
        statusBar()->showMessage(QStringLiteral("That device is no longer available"), 5000);
        m_takeOverDeviceButton->setVisible(false);
        return;
    }

    m_takeOverDeviceButton->setEnabled(false);
    QString error;
    if (!AudioDeviceControl::takeOver(*dev, &error)) {
        statusBar()->showMessage(QStringLiteral("Takeover failed: %1").arg(error), 8000);
        m_takeOverDeviceButton->setEnabled(true);
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("Took over %1 — retrying playback").arg(dev->description), 4000);
    m_takeOverDeviceButton->setVisible(false);

    // Reopen only once PipeWire has removed the old Audio/Sink. A profile flip
    // to Off is visible before its kernel device is fully released.
    const int index = m_player->queueIndex();
    // Prefer the exact profile-switch snapshot. The failed direct pipeline can
    // transiently report position 0 before entering Error, so the generic
    // "last healthy" sample is not reliable for this recovery.
    const bool profileSwitchTrack = m_profileTakeoverResumePending
        && index >= 0 && index < m_player->queue().size()
        && m_player->queue().at(index).path == m_profileTakeoverTrackPath;
    const bool sameTrack = index >= 0 && index < m_player->queue().size()
        && m_player->queue().at(index).path == m_lastHealthyTrackPath;
    const qint64 resumePositionMs = profileSwitchTrack ? m_profileTakeoverPositionMs
        : (sameTrack ? m_lastHealthyPositionMs : 0);
    const bool resumePlaying = profileSwitchTrack ? m_profileTakeoverWasPlaying : true;
    waitForDeviceOwnership(dev->hwPath, /*wantHeld=*/false, /*previousSinkNodeId=*/-1,
                           [this, index, resumePositionMs, resumePlaying]() {
        if (index >= 0 && index < m_player->queue().size())
            resumePlaybackAt(index, resumePositionMs, resumePlaying, /*settleDelayMs=*/250);
    });
}

void MainWindow::showDsdTakeoverPrompt(const Track &track, const QString &device)
{
    Q_UNUSED(track);
    if (device.isEmpty()) {
        m_player->resolveDsdTakeover(false);
        return;
    }
    m_dsdTakeoverPromptActive = true;
    m_pendingDsdTakeoverDevice = device;
    m_dsdTakeoverSecondsRemaining = 10;
    updateDsdTakeoverPromptText();
    m_dsdTakeoverPromptTimer->start();
}

void MainWindow::updateDsdTakeoverPromptText()
{
    if (!m_dsdTakeoverPromptActive) {
        return;
    }
    const auto dev = AudioDeviceControl::findByHwPath(m_pendingDsdTakeoverDevice);
    const QString name = dev ? dev->description : m_pendingDsdTakeoverDevice;
    const QString text = QStringLiteral("DSD needs %1 — take over for bit-perfect playback? Skipping in %2s")
                             .arg(name)
                             .arg(m_dsdTakeoverSecondsRemaining);
    statusBar()->showMessage(text);
    m_takeOverDeviceButton->setText(QStringLiteral("Take over DSD (%1s)").arg(m_dsdTakeoverSecondsRemaining));
    m_takeOverDeviceButton->setToolTip(text);
    m_takeOverDeviceButton->setEnabled(true);
    m_takeOverDeviceButton->setVisible(true);
}

void MainWindow::resolveDsdTakeoverPrompt(bool accepted)
{
    if (!m_dsdTakeoverPromptActive) {
        return;
    }
    m_dsdTakeoverPromptTimer->stop();
    m_dsdTakeoverPromptActive = false;
    const QString devicePath = m_pendingDsdTakeoverDevice;
    m_pendingDsdTakeoverDevice.clear();
    m_takeOverDeviceButton->setVisible(false);
    m_takeOverDeviceButton->setText(QStringLiteral("Take over device"));
    m_takeOverDeviceButton->setToolTip(
        QStringLiteral("Free the audio device from PipeWire and retry bit-perfect playback"));

    if (!accepted) {
        m_player->resolveDsdTakeover(false);
        return;
    }

    const auto dev = AudioDeviceControl::findByHwPath(devicePath);
    if (!dev) {
        statusBar()->showMessage(QStringLiteral("The requested DSD device is no longer available"), 8000);
        m_player->resolveDsdTakeover(false);
        return;
    }

    bool tookOver = false;
    if (dev->heldByPipeWire()) {
        QString error;
        if (!AudioDeviceControl::takeOver(*dev, &error)) {
            statusBar()->showMessage(QStringLiteral("DSD takeover failed: %1").arg(error), 8000);
            m_player->resolveDsdTakeover(false);
            return;
        }
        tookOver = true;
        m_takenOverDsdDevice = dev->hwPath;
        m_takenOverDsdRestoreProfile = dev->currentProfileIndex;
        m_playerBar->setReleaseDeviceVisible(true);
        statusBar()->showMessage(QStringLiteral("Took over %1 for native DSD playback").arg(dev->description), 4000);

        // A confirmed DSD takeover is an explicit switch to exclusive output.
        // Keep subsequent PCM tracks on the already-free ALSA device instead of
        // immediately racing PipeWire to restore shared routing after this DSD
        // track. Switching back to Shared remains explicit and releases the card.
        if (m_playbackProfile.mode != QStringLiteral("bit-perfect")) {
            m_playbackProfile.mode = QStringLiteral("bit-perfect");
            m_playbackProfile.sink = QStringLiteral("alsa");
            m_playbackProfile.device = dev->hwPath;
            m_playbackProfile.deviceId = dev->stableId;
            m_playbackProfile.softwareVolume = false;
            m_playbackProfile.allowResample = false;
            m_playbackProfile.releaseSinkOnPause = true;
            savePlaybackProfile();
            m_playback->setProfile(m_playbackProfile);
            m_playerBar->setVolumeControlEnabled(m_playbackProfile.softwareVolume);
        }
    }

    // PipeWire needs a brief moment to drop the card after switching its profile
    // to Off. The PlayerCore request remains pending during this delay and safely
    // becomes a no-op if the user selects another track in the meantime.
    const auto start = [this]() { m_player->resolveDsdTakeover(true); };
    if (tookOver) {
        QTimer::singleShot(400, this, start);
    } else {
        start();
    }
}

void MainWindow::scheduleHeldDeviceRelease(int delayMs)
{
    if (!canReleaseOutputDevice()) {
        return;
    }
    m_takenOverDsdReleaseTimer->start(delayMs);
}

void MainWindow::releaseTakenOverDsdDevice()
{
    if (m_takenOverDsdDevice.isEmpty()) {
        return;
    }
    m_takenOverDsdReleaseTimer->stop();
    const auto dev = AudioDeviceControl::findByHwPath(m_takenOverDsdDevice);
    if (!dev) {
        statusBar()->showMessage(QStringLiteral("The taken-over DSD device is no longer available"), 8000);
        m_takenOverDsdDevice.clear();
        m_takenOverDsdRestoreProfile = -1;
        m_playerBar->setReleaseDeviceVisible(false);
        if (isHidden() && m_core != nullptr && !m_core->isQuitting()) {
            m_core->releaseWindow();
        }
        return;
    }

    QString error;
    if (!AudioDeviceControl::release(*dev, m_takenOverDsdRestoreProfile, &error)) {
        statusBar()->showMessage(QStringLiteral("Could not release %1: %2").arg(dev->description, error), 8000);
        m_playerBar->setReleaseDeviceVisible(true);
        return;
    }
    statusBar()->showMessage(QStringLiteral("Released %1 back to PipeWire").arg(dev->description), 4000);
    m_takenOverDsdDevice.clear();
    m_takenOverDsdRestoreProfile = -1;
    m_playerBar->setReleaseDeviceVisible(false);
    // A tray-hidden window was retained solely to own this takeover and its
    // timers. Once the card is restored, return to the usual memory-saving
    // headless state.
    if (isHidden() && m_core != nullptr && !m_core->isQuitting()) {
        m_core->releaseWindow();
    }
}

bool MainWindow::canReleaseOutputDevice() const
{
    // A DSD takeover is always releasable; otherwise a bit-perfect profile holds
    // (or will hold) its configured card exclusively.
    return !m_takenOverDsdDevice.isEmpty()
        || (m_playbackProfile.mode == QStringLiteral("bit-perfect") && !m_playbackProfile.device.isEmpty());
}

void MainWindow::releaseHeldOutputDevice()
{
    // A tracked DSD takeover has its own restore-the-prior-profile path.
    if (!m_takenOverDsdDevice.isEmpty()) {
        releaseTakenOverDsdDevice();
        return;
    }
    if (m_playbackProfile.mode != QStringLiteral("bit-perfect")) {
        return;
    }
    // Hand the exclusive card back by switching the output to shared via the shared
    // transition path. Materialize the full shared profile (not just mode=shared)
    // so the persisted output has no bit-perfect leftovers — sink="alsa"/hw device
    // behind mode="shared" would be a "ghost" that keeps opening the card directly.
    const PlaybackProfile previous = m_playbackProfile;
    const PlaybackProfile next = m_playbackProfile.toSharedMode();
    applyProfileTransition(previous, next);
}

void MainWindow::releaseDsdDeviceForPcmTrack(const Track &track)
{
    // Only relevant when we hold a card off for native DSD and the now-current
    // track is ordinary PCM played through the shared graph — which can't reach a
    // card that's switched off in PipeWire. Bit-perfect plays PCM straight to the
    // ALSA device, and a DSD follow-up keeps using the takeover, so both no-op.
    if (m_takenOverDsdDevice.isEmpty()
        || isDsdTrack(track)
        || m_playbackProfile.mode == QStringLiteral("bit-perfect")) {
        return;
    }

    const QString hw = m_takenOverDsdDevice;
    const int queueIndex = m_player->queueIndex();
    const qint64 positionMs = m_playback->position();
    const bool wasPlaying = m_playback->state() == PlaybackBackend::State::Playing;

    // Hand the card back, then wait for PipeWire to rebuild its sink before
    // restarting the track — otherwise the shared sink binds to the absent device
    // and stays silent. (The just-started silent play continues until the restart.)
    releaseTakenOverDsdDevice();
    statusBar()->showMessage(QStringLiteral("Returning the device for shared playback…"), 10000);
    waitForDeviceOwnership(hw, /*wantHeld=*/true, /*previousSinkNodeId=*/-1,
                           [this, queueIndex, positionMs, wasPlaying]() {
        if (queueIndex >= 0) {
            resumePlaybackAt(queueIndex, positionMs, wasPlaying, /*settleDelayMs=*/600);
        }
    });
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
    if (m_searchView != nullptr) {
        m_searchView->setRankConfig(Search::RankConfig::fromJsonString(json));
    }
}

int MainWindow::idleReleaseMs() const
{
    const int seconds = std::clamp(m_state->setting(QStringLiteral("memory.idleReleaseSeconds"),
                                                    QString::number(kDefaultIdleReleaseSeconds)).toInt(),
                                   0,
                                   kMaxMemoryReleaseSeconds);
    return seconds * 1000;
}

int MainWindow::deepReleaseMs() const
{
    const int seconds = std::clamp(m_state->setting(QStringLiteral("memory.deepReleaseSeconds"),
                                                    QString::number(kDefaultDeepReleaseSeconds)).toInt(),
                                   0,
                                   kMaxMemoryReleaseSeconds);
    return seconds * 1000;
}

void MainWindow::loadPlaybackModes()
{
    const RepeatMode repeat = repeatModeFromString(m_state->setting(QStringLiteral("playback.repeatMode")));
    const ShuffleMode shuffle = shuffleModeFromString(m_state->setting(QStringLiteral("playback.shuffleMode")));
    const int libraryPercent = std::clamp(
        m_state->setting(QStringLiteral("playback.libraryShufflePercent"), QStringLiteral("20")).toInt(), 0, 100);
    const int radioPercent = std::clamp(
        m_state->setting(QStringLiteral("playback.radioShufflePercent"), QStringLiteral("80")).toInt(), 0, 100);
    // Percents first so restored shuffle modes use their saved chances. The
    // PlayerCore setters only emit on a change, so push the player bar
    // explicitly to cover restoring the default (off) modes too.
    m_player->setLibraryShufflePercent(libraryPercent);
    m_player->setRadioShufflePercent(radioPercent);
    m_player->setRepeatMode(repeat);
    m_player->setShuffleMode(shuffle);
    m_playerBar->setRepeatMode(repeat);
    m_playerBar->setShuffleMode(shuffle);
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
    if (m_searchView != nullptr) {
        m_searchView->setRankConfig(updated);
    }
    statusBar()->showMessage(QStringLiteral("Search ranking updated"), 3000);
}

void MainWindow::configureMemoryReclaim()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Memory reclaim"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;

    auto *idleSeconds = new QSpinBox(&dialog);
    idleSeconds->setRange(0, kMaxMemoryReleaseSeconds);
    idleSeconds->setSuffix(QStringLiteral(" seconds"));
    idleSeconds->setSpecialValueText(QStringLiteral("Disabled"));
    idleSeconds->setValue(std::clamp(m_state->setting(QStringLiteral("memory.idleReleaseSeconds"),
                                                      QString::number(kDefaultIdleReleaseSeconds)).toInt(),
                                     0,
                                     kMaxMemoryReleaseSeconds));
    form->addRow(QStringLiteral("Hidden screen release:"), idleSeconds);

    auto *deepSeconds = new QSpinBox(&dialog);
    deepSeconds->setRange(0, kMaxMemoryReleaseSeconds);
    deepSeconds->setSuffix(QStringLiteral(" seconds"));
    deepSeconds->setSpecialValueText(QStringLiteral("Disabled"));
    deepSeconds->setValue(std::clamp(m_state->setting(QStringLiteral("memory.deepReleaseSeconds"),
                                                      QString::number(kDefaultDeepReleaseSeconds)).toInt(),
                                     0,
                                     kMaxMemoryReleaseSeconds));
    form->addRow(QStringLiteral("Artwork cache release:"), deepSeconds);

    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    m_state->setSetting(QStringLiteral("memory.idleReleaseSeconds"), QString::number(idleSeconds->value()));
    m_state->setSetting(QStringLiteral("memory.deepReleaseSeconds"), QString::number(deepSeconds->value()));
    statusBar()->showMessage(QStringLiteral("Memory reclaim settings updated"), 3000);
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

void MainWindow::showTrackProperties(const Track &track)
{
    if (track.path.isEmpty()) {
        return;
    }
    // The source Track may be a search/queue snapshot missing promoted fields,
    // so prefer the fully-populated DB record and fall back to the passed copy.
    Track full = m_database->trackForPath(track.path);
    if (full.path.isEmpty()) {
        full = track;
    }
    const MetadataBlob::FullMetadata metadata = m_database->fullMetadata(track.path);
    auto *dialog = new TrackPropertiesDialog(full, metadata, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
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

void MainWindow::configurePlaylistMetadataDisplay()
{
    if (m_playlistView == nullptr) {
        return;
    }
    m_playlistView->configureMetadataDisplay(this);
    savePlaylistViewSettings();
}

void MainWindow::configureKeybindings()
{
    KeybindingsDialog dialog(this);
    if (m_panelSearch != nullptr) {
        dialog.setMainPanelProfileName(m_panelSearch->keyBindingProfileName());
    }
    dialog.setFileExplorerProfileName(m_libraryFileExplorer != nullptr
                                          ? m_libraryFileExplorer->keyBindingProfileName()
                                          : m_state->setting(QStringLiteral("fileExplorer.keyBindingProfile"),
                                                             defaultKeyBindingProfiles().isEmpty()
                                                                 ? QStringLiteral("vim")
                                                                 : defaultKeyBindingProfiles().first().name));
    dialog.setQueueProfileName(m_queueScreen != nullptr
                                   ? m_queueScreen->keyBindingProfileName()
                                   : m_state->setting(QStringLiteral("queueScreen.keyBindingProfile"),
                                                      defaultQueueKeyBindingProfileName()));
    dialog.setFileExplorerKeyHintsVisible(m_libraryFileExplorer != nullptr
                                              ? m_libraryFileExplorer->isKeyHintBarVisible()
                                              : m_state->setting(QStringLiteral("fileExplorer.showKeyHints")) == QStringLiteral("true"));

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    if (m_panelSearch != nullptr) {
        m_panelSearch->setKeyBindingProfileName(dialog.mainPanelProfileName());
        saveMainWindowViewSettings();
    }
    if (m_libraryFileExplorer != nullptr) {
        m_libraryFileExplorer->setKeyBindingProfileName(dialog.fileExplorerProfileName());
    }
    if (m_freeRoamFileExplorer != nullptr) {
        m_freeRoamFileExplorer->setKeyBindingProfileName(dialog.fileExplorerProfileName());
    }
    if (m_queueScreen != nullptr) {
        m_queueScreen->setKeyBindingProfileName(dialog.queueProfileName());
    }
    if (m_libraryFileExplorer != nullptr) {
        m_libraryFileExplorer->setKeyHintBarVisible(dialog.fileExplorerKeyHintsVisible());
    }
    if (m_freeRoamFileExplorer != nullptr) {
        m_freeRoamFileExplorer->setKeyHintBarVisible(dialog.fileExplorerKeyHintsVisible());
    }
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

void MainWindow::showGenreCuration()
{
    GenreCurationDialog dialog(m_database, this);
    dialog.exec();
}

void MainWindow::showScoringWeights()
{
    ScoringWeightsDialog dialog(m_database, listenHistoryPath(), this);
    dialog.exec();
}

void MainWindow::showAnalysisStatus()
{
    AudioAnalysisStatusDialog dialog(m_core->featuresPath(),
                                     [this]() { return audioAnalysisLiveStatus(); },
                                     this);
    dialog.exec();
}

void MainWindow::showDuplicateCopies()
{
    DuplicateCopiesDialog dialog(m_database, m_core->featuresPath(), this);
    dialog.exec();
}

void MainWindow::startAudioAnalysis()
{
    if (m_audioAnalysisProcess != nullptr) {
        statusBar()->showMessage(QStringLiteral("Audio analysis is already running"), 5000);
        return;
    }

    const QString binary = resolveMuzaitenIndexBinary();
    if (binary.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("muzaiten-features was not found next to the app or on PATH"), 8000);
        return;
    }

    m_audioAnalysisStdout.clear();
    m_audioAnalysisStderr.clear();
    m_audioAnalysisStderrBuffer.clear();
    m_audioAnalysisCancelRequested = false;
    m_audioAnalysisRunState = {};

    auto *process = new QProcess(this);
    m_audioAnalysisProcess = process;
    connect(process, &QProcess::readyReadStandardOutput, this, &MainWindow::readAudioAnalysisStdout);
    connect(process, &QProcess::readyReadStandardError, this, &MainWindow::readAudioAnalysisStderr);
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &MainWindow::finishAudioAnalysis);
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (process != m_audioAnalysisProcess || error != QProcess::FailedToStart) {
            return;
        }
        const QString detail = process->errorString();
        m_audioAnalysisProcess = nullptr;
        m_audioAnalysisRunState = {};
        m_audioAnalysisRestartPending = false;
        m_playerBar->setAudioAnalysisRunStatus(false, QString());
        process->deleteLater();
        statusBar()->showMessage(QStringLiteral("Audio analysis failed to start: %1").arg(detail), 10000);
    });

    process->setProgram(binary);
    static const char *const analysisPowerNames[3] = {"background", "balanced", "turbo"};
    static const char *const analysisPowerLabels[3] = {"Background", "Balanced", "Turbo"};
    const int analysisPower = analysisPowerSetting();
    const int boundedAnalysisPower = std::clamp(analysisPower, 0, 2);
    process->setArguments({
        QStringLiteral("scan"),
        QStringLiteral("--library"),
        databasePath(),
        QStringLiteral("--features"),
        m_core->featuresPath(),
        QStringLiteral("--power"),
        QString::fromLatin1(analysisPowerNames[boundedAnalysisPower]),
        QStringLiteral("--json"),
        QStringLiteral("--progress"),
    });

    m_audioAnalysisRunState.running = true;
    m_audioAnalysisRunState.phase = AudioAnalysisData::LiveStatus::Phase::AnalyzingFiles;
    m_audioAnalysisRunState.power = QString::fromLatin1(analysisPowerLabels[boundedAnalysisPower]);

    m_playerBar->setAudioAnalysisRunStatus(true, QStringLiteral("Analyzing…"));
    statusBar()->showMessage(QStringLiteral("Starting audio analysis..."));
    process->start();
}

void MainWindow::cancelAudioAnalysis()
{
    if (m_audioAnalysisProcess == nullptr) {
        return;
    }
    QProcess *process = m_audioAnalysisProcess;
    m_audioAnalysisCancelRequested = true;
    m_playerBar->setAudioAnalysisRunStatus(true, QStringLiteral("Stopping analysis..."));
    if (!m_audioAnalysisRestartPending) {
        statusBar()->showMessage(QStringLiteral("Stopping audio analysis..."), 4000);
    }
    process->terminate();
    // Feature fill is serial per representative: a long decode can legitimately
    // take far longer than the file-phase 3s backstop. Use a long phase-aware
    // grace so SIGTERM can produce canceled=true before SIGKILL.
    constexpr int kFilePhaseKillGraceMs = 3000;
    constexpr int kFeaturesPhaseKillGraceMs = 120000;
    const int killGraceMs =
        m_audioAnalysisRunState.phase == AudioAnalysisData::LiveStatus::Phase::WritingFeatures
            ? kFeaturesPhaseKillGraceMs
            : kFilePhaseKillGraceMs;
    QTimer::singleShot(killGraceMs, this, [this, process]() {
        if (m_audioAnalysisProcess == process && process->state() != QProcess::NotRunning) {
            process->kill();
        }
    });
}

void MainWindow::readAudioAnalysisStdout()
{
    if (m_audioAnalysisProcess != nullptr) {
        m_audioAnalysisStdout.append(m_audioAnalysisProcess->readAllStandardOutput());
    }
}

void MainWindow::readAudioAnalysisStderr()
{
    if (m_audioAnalysisProcess == nullptr) {
        return;
    }
    m_audioAnalysisStderrBuffer.append(m_audioAnalysisProcess->readAllStandardError());
    qsizetype newline = -1;
    while ((newline = m_audioAnalysisStderrBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_audioAnalysisStderrBuffer.left(newline);
        m_audioAnalysisStderrBuffer.remove(0, newline + 1);
        handleAudioAnalysisProgressLine(QString::fromUtf8(line).trimmed());
    }
}

void MainWindow::finishAudioAnalysis(int exitCode, QProcess::ExitStatus exitStatus)
{
    QProcess *process = m_audioAnalysisProcess;
    if (process == nullptr) {
        return;
    }

    readAudioAnalysisStdout();
    readAudioAnalysisStderr();
    if (!m_audioAnalysisStderrBuffer.isEmpty()) {
        handleAudioAnalysisProgressLine(QString::fromUtf8(m_audioAnalysisStderrBuffer).trimmed());
        m_audioAnalysisStderrBuffer.clear();
    }

    m_audioAnalysisProcess = nullptr;
    m_playerBar->setAudioAnalysisRunStatus(false, QString());

    // A power change mid-run stops the scan and restarts it below; only a
    // user-requested stop earns the "run again to resume" message.
    const bool restartRequested = m_audioAnalysisRestartPending && m_audioAnalysisCancelRequested;
    m_audioAnalysisRestartPending = false;

    if (m_audioAnalysisCancelRequested) {
        if (!restartRequested) {
            statusBar()->showMessage(
                QStringLiteral("Audio analysis stopped — completed work is saved; run again to resume"), 8000);
        }
    } else if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        QString detail = QString::fromUtf8(m_audioAnalysisStderr).trimmed();
        if (detail.isEmpty()) {
            detail = process->errorString();
        }
        if (detail.isEmpty()) {
            detail = QStringLiteral("exit code %1").arg(exitCode);
        }
        statusBar()->showMessage(QStringLiteral("Audio analysis failed: %1").arg(detail.left(240)), 10000);
    } else {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(m_audioAnalysisStdout, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            statusBar()->showMessage(QStringLiteral("Audio analysis finished but returned invalid JSON: %1")
                                         .arg(parseError.errorString()),
                                     10000);
        } else {
            const QJsonObject object = document.object();
            if (object.value(QStringLiteral("canceled")).toBool()) {
                statusBar()->showMessage(
                    QStringLiteral("Audio analysis stopped — completed work is saved; run again to resume"), 8000);
            } else {
                const int featuresWritten = object.value(QStringLiteral("features_written")).toInt(-1);
                statusBar()->showMessage(
                    AudioAnalysisData::finalSummary(object.value(QStringLiteral("scanned")).toInt(),
                                                    object.value(QStringLiteral("skipped")).toInt(),
                                                    object.value(QStringLiteral("failed")).toInt(),
                                                    object.value(QStringLiteral("groups")).toInt(),
                                                    object.value(QStringLiteral("elapsed_secs")).toDouble(),
                                                    featuresWritten),
                    10000);
            }
        }
    }

    m_audioAnalysisStdout.clear();
    m_audioAnalysisStderr.clear();
    m_audioAnalysisStderrBuffer.clear();
    m_audioAnalysisCancelRequested = false;
    // Full reset (not just running=false) so a reopened status dialog shows
    // Idle instead of the last run's frozen phase and counters.
    m_audioAnalysisRunState = {};
    process->deleteLater();

    if (restartRequested) {
        startAudioAnalysis();
    }
}

void MainWindow::handleAudioAnalysisProgressLine(const QString &line)
{
    if (line.isEmpty()) {
        return;
    }
    static const QRegularExpression progressPattern(
        QStringLiteral("^progress\\s+(\\d+)/(\\d+)(?:\\s+elapsed=([0-9.]+)\\s+rate=([0-9.]+|-)\\s+eta=(\\d+|-))?$"));
    const QRegularExpressionMatch match = progressPattern.match(line);
    if (match.hasMatch()) {
        m_audioAnalysisRunState.running = true;
        if (m_audioAnalysisRunState.phase == AudioAnalysisData::LiveStatus::Phase::Idle) {
            m_audioAnalysisRunState.phase = AudioAnalysisData::LiveStatus::Phase::AnalyzingFiles;
        }
        m_audioAnalysisRunState.analyzed = match.captured(1).toInt();
        m_audioAnalysisRunState.total = match.captured(2).toInt();
        if (!match.captured(3).isEmpty()) {
            m_audioAnalysisRunState.elapsedSecs = match.captured(3).toDouble();
        }
        if (!match.captured(4).isEmpty()) {
            if (match.captured(4) == QLatin1String("-")) {
                m_audioAnalysisRunState.rate = -1.0;
            } else {
                m_audioAnalysisRunState.rate = match.captured(4).toDouble();
            }
        }
        if (!match.captured(5).isEmpty() && match.captured(5) != QLatin1String("-")) {
            m_audioAnalysisRunState.etaSecs = match.captured(5).toLongLong();
        } else {
            m_audioAnalysisRunState.etaSecs.reset();
        }
        m_playerBar->setAudioAnalysisRunStatus(true, AudioAnalysisData::progressLabel(m_audioAnalysisRunState));
        return;
    }
    if (line == QLatin1String("phase grouping")
        || line == QLatin1String("phase features")) {
        m_audioAnalysisRunState.running = true;
        m_audioAnalysisRunState.analyzed = 0;
        m_audioAnalysisRunState.total = 0;
        m_audioAnalysisRunState.rate = -1.0;
        m_audioAnalysisRunState.etaSecs.reset();
        if (line == QLatin1String("phase grouping")) {
            m_audioAnalysisRunState.phase = AudioAnalysisData::LiveStatus::Phase::Grouping;
            m_playerBar->setAudioAnalysisRunStatus(true, QStringLiteral("Grouping tracks..."));
        } else {
            m_audioAnalysisRunState.phase = AudioAnalysisData::LiveStatus::Phase::WritingFeatures;
            m_playerBar->setAudioAnalysisRunStatus(true, QStringLiteral("Writing features..."));
        }
        return;
    }
    m_audioAnalysisStderr.append(line.toUtf8());
    m_audioAnalysisStderr.append('\n');
}

AudioAnalysisData::LiveStatus MainWindow::audioAnalysisLiveStatus() const
{
    return m_audioAnalysisRunState;
}

QString MainWindow::resolveMuzaitenIndexBinary() const
{
    const QString sibling = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("muzaiten-features"));
    const QFileInfo siblingInfo(sibling);
    if (siblingInfo.exists() && siblingInfo.isExecutable()) {
        return siblingInfo.absoluteFilePath();
    }
    return QStandardPaths::findExecutable(QStringLiteral("muzaiten-features"));
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
            // Rebuild the search index to include the newly imported MPD tracks.
            if (m_searchView != nullptr) {
                m_searchView->invalidateIndex(databasePath());
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
        if (m_musicExplorerView != nullptr) {
            m_musicExplorerView->setAlbums({});
        }
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
                              Q_ARG(bool, !scrobbleOffline()),
                              Q_ARG(QString, token),
                              Q_ARG(QString, listenHistoryPath()));
}

void MainWindow::showListeningHistory()
{
    if (m_listenHistory == nullptr || !m_listenHistory->isOpen()) {
        QMessageBox::warning(this, QStringLiteral("Listening history"), QStringLiteral("Listening history is unavailable."));
        return;
    }

    ListeningHistoryDialog dialog(m_listenHistory, this);
    if (m_state != nullptr) {
        bool ok = false;
        const int saved = m_state->setting(QStringLiteral("listeningHistory.rowHeight")).toInt(&ok);
        if (ok && saved > 0) {
            dialog.setRowHeight(saved);
        }
    }
    connect(&dialog, &ListeningHistoryDialog::rowHeightChanged, this, [this](int height) {
        if (m_state != nullptr) {
            m_state->setSetting(QStringLiteral("listeningHistory.rowHeight"), QString::number(height));
        }
    });
    connect(&dialog, &ListeningHistoryDialog::backlogChanged, this, [this](const QString &service, int changedCount) {
        updateScrobbleBacklogActions();
        if (changedCount > 0) {
            triggerScrobbleUpload(service);
        }
    });
    connect(&dialog, &ListeningHistoryDialog::statusMessageRequested, this, [this](const QString &message, int timeoutMs) {
        statusBar()->showMessage(message, timeoutMs);
    });
    connect(&dialog, &ListeningHistoryDialog::forgetBehaviorRequested, this,
            [this](const Track &track, bool includeImportedListens) {
                const int removed = m_core->forgetTrackBehaviorForSong(track.path, includeImportedListens);
                const QString title = track.title.trimmed().isEmpty() ? track.filename : track.title.trimmed();
                statusBar()->showMessage(QStringLiteral("Forgot %1 listening behavior rows for \"%2\"").arg(removed).arg(title),
                                         5000);
            });
    dialog.exec();
}

void MainWindow::clearScrobbleBacklog(const QString &service)
{
    if (m_listenHistory == nullptr || !m_listenHistory->isOpen()) {
        statusBar()->showMessage(QStringLiteral("Listening history is unavailable"), 4000);
        return;
    }
    const int pending = m_listenHistory->pendingCount(service);
    if (pending <= 0) {
        statusBar()->showMessage(QStringLiteral("No pending %1 scrobbles")
                                     .arg(service == ListenHistoryStore::LastFm ? QStringLiteral("Last.fm") : QStringLiteral("ListenBrainz")),
                                 4000);
        return;
    }
    const QString serviceName = service == ListenHistoryStore::LastFm ? QStringLiteral("Last.fm") : QStringLiteral("ListenBrainz");
    const auto choice = QMessageBox::question(this,
                                              QStringLiteral("Clear scrobble backlog"),
                                              QStringLiteral("Clear %1 pending %2 scrobbles? Permanent listening history will stay saved.")
                                                  .arg(pending)
                                                  .arg(serviceName));
    if (choice != QMessageBox::Yes) {
        return;
    }
    const int cleared = m_listenHistory->clearPending(service);
    statusBar()->showMessage(QStringLiteral("Cleared %1 pending %2 scrobbles").arg(cleared).arg(serviceName), 5000);
    updateScrobbleBacklogActions();
}

void MainWindow::triggerScrobbleUpload(const QString &service)
{
    if (service == ListenHistoryStore::LastFm) {
        QMetaObject::invokeMethod(m_lastFmScrobbler, "uploadBacklog", Qt::QueuedConnection);
    } else if (service == ListenHistoryStore::ListenBrainz) {
        QMetaObject::invokeMethod(m_listenBrainzScrobbler, "uploadBacklog", Qt::QueuedConnection);
    }
}

void MainWindow::updateScrobbleBacklogActions()
{
    if (m_listenHistory == nullptr || !m_listenHistory->isOpen()) {
        m_playerBar->setScrobbleBacklogCounts(0, 0);
    } else {
        m_playerBar->setScrobbleBacklogCounts(m_listenHistory->pendingCount(ListenHistoryStore::LastFm),
                                              m_listenHistory->pendingCount(ListenHistoryStore::ListenBrainz));
    }
    updateBackfillStatusDisplay();
}

void MainWindow::updateBackfillStatusDisplay()
{
    const AppCore::BackfillStatus status = m_core->backfillStatus();
    const bool isListenBrainz = status.service == ListenHistoryStore::ListenBrainz;

    QString text;
    if (status.running) {
        const QLocale locale;
        // The cumulative DB count reads correctly across resume cycles; the
        // per-run inserted counter restarts at zero on every resume.
        QString stored = locale.toString(status.storedTotal > 0 ? status.storedTotal : status.inserted);
        if (status.totalListens > 0) {
            stored += QStringLiteral(" of ~%1").arg(locale.toString(status.totalListens));
        }
        text = QStringLiteral("%1: %2 processed · %3 stored")
                   .arg(isListenBrainz ? QStringLiteral("ListenBrainz import") : QStringLiteral("Last.fm sync"),
                        locale.toString(status.processed), stored);
        if (status.reachedTs > 0) {
            text += QStringLiteral(" · reached %1")
                        .arg(QDateTime::fromSecsSinceEpoch(status.reachedTs).toString(QStringLiteral("MMM yyyy")));
        }
    } else if (!status.lastMessage.isEmpty()) {
        text = status.lastMessage;
    }

    bool lbResumable = false;
    if (m_listenHistory != nullptr && m_listenHistory->isOpen()) {
        const QString cursor = m_listenHistory->metaValue(ScrobbleBackfill::OldestTsMetaKey);
        const QString canceled = m_listenHistory->metaValue(ScrobbleBackfill::CanceledMetaKey);
        lbResumable = !cursor.isEmpty() && canceled.isEmpty();
    }

    m_playerBar->setBackfillStatus(status.running, text, lbResumable);
}

bool MainWindow::scrobbleOffline() const
{
    return m_database->setting(QStringLiteral("scrobble.offline"), QStringLiteral("false")) == QStringLiteral("true");
}

QString MainWindow::listenHistoryPath() const
{
    // History is durable, user-owned data like the library itself, so it lives
    // in the data dir rather than the (resettable) state dir.
    return QDir(AppPaths::dataDir()).filePath(QStringLiteral("history.sqlite"));
}

void MainWindow::setScrobbleOffline(bool offline)
{
    m_database->setSetting(QStringLiteral("scrobble.offline"), offline ? QStringLiteral("true") : QStringLiteral("false"));
    // Reconfigure both services; leaving offline mode flushes the backlog.
    configureListenBrainz();
    configureLastFm();
    // Leaving offline mode: eagerly push a "now playing" for the current track,
    // the same way play/pause does, so the services reflect what is playing now
    // instead of waiting for the next track. The scrobbler-side rate limiter
    // keeps a rapid offline/online toggle from spamming the services. Queued so
    // the configure() above (also queued) applies credentials/uploadAllowed
    // first.
    if (!offline && !m_player->currentTrack().path.isEmpty()
        && m_playback->state() == PlaybackBackend::State::Playing) {
        QMetaObject::invokeMethod(m_listenBrainzScrobbler, "resendNowPlaying", Qt::QueuedConnection);
        QMetaObject::invokeMethod(m_lastFmScrobbler, "resendNowPlaying", Qt::QueuedConnection);
    }
    statusBar()->showMessage(offline ? QStringLiteral("Scrobble uploads paused — listens are buffered locally")
                                     : QStringLiteral("Scrobble uploads resumed — sending buffered listens"),
                             5000);
}

void MainWindow::setListenBrainzEnabled(bool enabled)
{
    m_database->setSetting(QStringLiteral("listenbrainz.enabled"), enabled ? QStringLiteral("true") : QStringLiteral("false"));
    configureListenBrainz();
    // If a track is already playing when the scrobbler is toggled on, catch up
    // so it doesn't miss the current track.  The queued configure() runs first
    // (Qt::QueuedConnection), so credentials are set before resumeTrack fires.
    if (enabled && !m_player->currentTrack().path.isEmpty() && m_playback->state() != PlaybackBackend::State::Stopped) {
        const qint64 elapsedMs = std::max<qint64>(0, m_playback->position());
        const bool playing = m_playback->state() == PlaybackBackend::State::Playing;
        QMetaObject::invokeMethod(m_listenBrainzScrobbler, "resumeTrack", Qt::QueuedConnection,
                                  Q_ARG(Track, m_player->currentTrack()), Q_ARG(qint64, elapsedMs), Q_ARG(bool, playing));
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
    if (!token.isEmpty()) {
        QMetaObject::invokeMethod(m_listenBrainzScrobbler, "validateToken", Qt::QueuedConnection, Q_ARG(QString, token));
    }
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
                              Q_ARG(bool, !scrobbleOffline()),
                              Q_ARG(QString, lastFmApiKey()),
                              Q_ARG(QString, lastFmSharedSecret()),
                              Q_ARG(QString, sessionKey),
                              Q_ARG(QString, listenHistoryPath()));
}

void MainWindow::setLastFmEnabled(bool enabled)
{
    m_database->setSetting(QStringLiteral("lastfm.enabled"), enabled ? QStringLiteral("true") : QStringLiteral("false"));
    configureLastFm();
    // If a track is already playing when the scrobbler is toggled on, catch up
    // so it doesn't miss the current track.  The queued configure() runs first
    // (Qt::QueuedConnection), so credentials are set before resumeTrack fires.
    if (enabled && !m_player->currentTrack().path.isEmpty() && m_playback->state() != PlaybackBackend::State::Stopped) {
        const qint64 elapsedMs = std::max<qint64>(0, m_playback->position());
        const bool playing = m_playback->state() == PlaybackBackend::State::Playing;
        QMetaObject::invokeMethod(m_lastFmScrobbler, "resumeTrack", Qt::QueuedConnection,
                                  Q_ARG(Track, m_player->currentTrack()), Q_ARG(qint64, elapsedMs), Q_ARG(bool, playing));
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

void MainWindow::presentTrack(const Track &track, bool notifyScrobbler)
{
    updateCurrentAlbumArt();
    const QString title = track.title.isEmpty() ? track.filename : track.title;
    QString subtitle = QStringLiteral("%1 - %2").arg(track.artistName, track.albumTitle);
    if (!track.date.isEmpty()) {
        subtitle += QStringLiteral(" (%1)").arg(track.date.left(4));
    }
    m_playerBar->setTrackInfo(title, subtitle, track.effectiveRating0To100);
    m_rightSidebar->setTrackInfo(track);
    m_playerBar->setPosition(0, track.durationMs);
    refreshPlaylistNowPlaying();
    // AppCore handles MPRIS mirroring and scrobble notification.
    if (notifyScrobbler) {
        statusBar()->showMessage(QStringLiteral("Playing %1").arg(title), 3000);
    }
}

// In-place metadata/rating refresh of the current track: update displays
// without resetting the position readout or scrobble state.
void MainWindow::presentCurrentTrackUpdate(const Track &track)
{
    updateCurrentAlbumArt();
    const QString title = track.title.isEmpty() ? track.filename : track.title;
    QString subtitle = QStringLiteral("%1 - %2").arg(track.artistName, track.albumTitle);
    if (!track.date.isEmpty()) {
        subtitle += QStringLiteral(" (%1)").arg(track.date.left(4));
    }
    m_playerBar->setTrackInfo(title, subtitle, track.effectiveRating0To100);
    m_rightSidebar->setTrackInfo(track);
    // AppCore handles MPRIS mirroring.
}

void MainWindow::clearPresentedTrack()
{
    m_playerBar->setTrackText({});
    m_playerBar->setAlbumArt(QString());
    m_rightSidebar->setTrackInfo({});
    refreshPlaylistNowPlaying();
    // AppCore handles MPRIS clearing.
    savePlaybackState(true);
}

void MainWindow::onPlayerIndexChanged(int index, bool userInitiated)
{
    m_queueStore->setCurrentIndex(index);
    if (userInitiated) {
        m_rightSidebar->setCurrentIndex(index, /*reveal=*/true);
        if (m_mainView == MainView::Queue) {
            ensureQueueScreen()->revealCurrentPlaying();
        }
    }
    scheduleQueueStateSave();
}

void MainWindow::appendAndPlayTrack(const Track &track)
{
    m_player->appendAndPlay(track);
}

void MainWindow::playNextTracks(const QVector<Track> &tracks)
{
    m_player->playTracksNext(tracks);
}

void MainWindow::addTracksToQueue(const QVector<Track> &tracks)
{
    m_player->appendTracks(tracks);
}

void MainWindow::moveQueueRows(const QVector<int> &rows, int destinationRow)
{
    m_player->moveRows(rows, destinationRow);
}

void MainWindow::removeQueueRows(const QVector<int> &rows)
{
    m_core->recordUserQueueRemovals(rows);
    m_player->removeRows(rows);
}

void MainWindow::clearQueue()
{
    if (m_player->queue().isEmpty()) {
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("Clear queue"),
                              QStringLiteral("Are you sure?\nThis action will wipe the current queue."))
        != QMessageBox::Yes) {
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("Clear queue"),
                              QStringLiteral("Are you really sure you want to wipe the current queue?"))
        != QMessageBox::Yes) {
        return;
    }

    const bool keepCurrent = m_player->queueIndex() >= 0
        && !m_player->queue().at(m_player->queueIndex()).path.isEmpty();
    pushCurrentQueueToBacklog(QString());

    if (keepCurrent) {
        markQueueAsSpontaneous();
        m_player->clearKeepingCurrent();
        return;
    }

    m_queueId.clear();
    m_queueSourceKind = QStringLiteral("queue");
    m_queueSourcePlaylistId = 0;
    m_queueSourceName.clear();
    m_player->clearAll();
}

void MainWindow::clearPlayNextPriority()
{
    m_player->collapsePlayNext();
    scheduleQueueStateSave();
}

void MainWindow::startRadioFromSeed(const QString &path)
{
    if (path.isEmpty()) {
        return;
    }
    // Same snapshot call Clear queue uses before wiping the queue, so a radio
    // start can be undone via "Restore previous queue".
    snapshotCurrentQueueAsPrevious(QStringLiteral("radio"));
    if (!m_core->startRadio(path)) {
        statusBar()->showMessage(QStringLiteral("Start Radio: track not found in library"), 4000);
    }
}

void MainWindow::startArtistRadio(const QString &artistName)
{
    const QString trimmedArtist = artistName.trimmed();
    if (trimmedArtist.isEmpty()) {
        return;
    }
    snapshotCurrentQueueAsPrevious(QStringLiteral("radio"));
    if (!m_core->startArtistRadio(trimmedArtist)) {
        statusBar()->showMessage(QStringLiteral("Start Artist Radio: artist not found in library"), 4000);
    }
}

void MainWindow::applyTrackFlag(const Track &track, const QString &flag, bool on)
{
    if (track.path.isEmpty()) {
        return;
    }
    if (!m_core->setTrackFlagForSong(track.path, flag, on)) {
        statusBar()->showMessage(QStringLiteral("Could not update taste control"), 4000);
        return;
    }

    const QString label = flag == QStringLiteral("never_radio")
        ? QStringLiteral("Never play on radio")
        : QStringLiteral("Don't learn from this");
    const QString title = track.title.trimmed().isEmpty() ? track.filename : track.title.trimmed();
    statusBar()->showMessage(QStringLiteral("%1 %2 for \"%3\"")
                                 .arg(label, on ? QStringLiteral("enabled") : QStringLiteral("disabled"), title),
                             4000);
}

void MainWindow::startMix(const QString &mode)
{
    snapshotCurrentQueueAsPrevious(QStringLiteral("radio"));
    if (!m_core->startMix(mode)) {
        statusBar()->showMessage(QStringLiteral("Not enough listening data for this mix yet"), 4000);
    }
}

void MainWindow::patchQueueTracksFromMetadata(const QVector<Track> &tracks)
{
    m_player->patchTracksFromMetadata(tracks);
}

void MainWindow::patchQueueRows(const QVector<int> &rows)
{
    for (int row : rows) {
        if (row < 0 || row >= m_player->queue().size()) {
            continue;
        }
        m_queueStore->updateTrack(row, m_player->queue().at(row));
    }
    if (!rows.isEmpty()) {
        if (m_panelSearch != nullptr) {
            m_panelSearch->refreshPanel(MainPanelId::Queue);
        }
        scheduleQueueStateSave();
    }
}

void MainWindow::refreshPlayNextRange()
{
    if (m_queueStore != nullptr) {
        m_queueStore->setPlayNextRange(m_player->queueIndex() + 1, m_player->playNextInsertIndex());
    }
}

void MainWindow::refreshQueueSourceDependentUi()
{
    // The "(don't save to playlist)" menu items and the playlist-mirror warning
    // only make sense while a playlist backs the queue; tell every view and gate
    // the "merge saved queue" action, which is never wanted in playlist mode.
    // Called from syncQueueState (queue mutations) and from the queue-*replace*
    // paths (play playlist/album, restore snapshot, startup restore), which use
    // resetQueue and so never emit queueChanged themselves.
    const bool playlistSourced = queueIsPlaylistSourced();
    if (m_trackTable != nullptr) m_trackTable->setQueueIsPlaylistSourced(playlistSourced);
    if (m_albumGrid != nullptr) m_albumGrid->setQueueIsPlaylistSourced(playlistSourced);
    if (m_musicExplorerView != nullptr) m_musicExplorerView->setQueueIsPlaylistSourced(playlistSourced);
    if (m_libraryFileExplorer != nullptr) m_libraryFileExplorer->setQueueIsPlaylistSourced(playlistSourced);
    if (m_freeRoamFileExplorer != nullptr) m_freeRoamFileExplorer->setQueueIsPlaylistSourced(playlistSourced);
    if (m_searchView != nullptr) m_searchView->setQueueIsPlaylistSourced(playlistSourced);
    if (m_playlistView != nullptr) m_playlistView->setQueueIsPlaylistSourced(playlistSourced);
    if (m_queueScreen != nullptr) m_queueScreen->setQueueIsPlaylistSourced(playlistSourced);
    if (m_rightSidebar != nullptr) m_rightSidebar->setQueueIsPlaylistSourced(playlistSourced);
    if (m_playerBar != nullptr) m_playerBar->setMergeSavedQueueEnabled(!playlistSourced);
    refreshPlaylistNowPlaying();
}

void MainWindow::refreshPlaylistNowPlaying()
{
    if (m_playlistView == nullptr) {
        return;
    }
    const qint64 sourceId = queueIsPlaylistSourced() ? m_queueSourcePlaylistId : 0;
    m_playlistView->setNowPlaying(m_player->currentTrack().path, sourceId);
}

void MainWindow::syncQueueState()
{
    // PlayerCore already clamped its indices and re-prepared the gapless
    // "next" track; push every derived view of the queue in lock-step.
    if (m_player->queue().isEmpty()) {
        m_queueId.clear();
        m_queueSourceKind = QStringLiteral("queue");
        m_queueSourcePlaylistId = 0;
        m_queueSourceName.clear();
    } else {
        ensureCurrentQueueIdentity();
    }

    m_queueStore->setSnapshot(m_player->queue(), m_player->queueIndex(),
                              m_player->queueIndex() + 1, m_player->playNextInsertIndex());
    refreshPlayNextRange();
    if (m_panelSearch != nullptr) {
        m_panelSearch->refreshPanel(MainPanelId::Queue);
    }
    refreshQueueSourceDependentUi();
    scheduleQueueStateSave();
}

void MainWindow::syncPlaylistBackedQueue(qint64 playlistId)
{
    if (!queueIsPlaylistSourced() || playlistId != m_queueSourcePlaylistId || m_playlistDb == nullptr) {
        return;
    }
    QVector<Track> tracks;
    for (const PlaylistItem &item : m_playlistDb->items(playlistId)) {
        if (item.trackPath.isEmpty()
            || (item.status != PlaylistItemStatus::Matched && item.status != PlaylistItemStatus::Approximate)) {
            continue;
        }
        const Track track = m_database->trackForPath(item.trackPath);
        if (!track.path.isEmpty()) {
            tracks.push_back(track);
        }
    }
    const QString currentPath = m_player->currentTrack().path;
    int currentIndex = -1;
    for (int i = 0; i < tracks.size(); ++i) {
        if (tracks.at(i).path == currentPath) {
            currentIndex = i;
            break;
        }
    }
    m_player->resetQueue(tracks, currentIndex);
    m_player->prepareNext();
    m_queueStore->setSnapshot(m_player->queue(), m_player->queueIndex(),
                              m_player->queueIndex() + 1, m_player->playNextInsertIndex());
    refreshPlayNextRange();
    refreshPlaylistNowPlaying();
    if (m_panelSearch != nullptr) {
        m_panelSearch->refreshPanel(MainPanelId::Queue);
    }
    scheduleQueueStateSave();
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

    const int startIndex = static_cast<int>(m_player->queue().size());
    addTracksToQueue(tracks);
    playQueueIndex(startIndex);
}

void MainWindow::playAlbumsReplacingQueue(const QStringList &albumTitles)
{
    if (m_currentArtist.isEmpty() || albumTitles.isEmpty()) {
        return;
    }
    QVector<Track> tracks;
    for (const QString &albumTitle : albumTitles) {
        if (albumTitle.isEmpty()) {
            continue;
        }
        tracks += m_librarySource == LibrarySource::Mpd
            ? m_database->mpdTracksForArtist(m_currentArtist, mpdMusicDirectory(), albumTitle)
            : m_database->tracksForArtist(m_currentArtist, albumTitle);
    }
    const QString sourceName = albumTitles.size() == 1
        ? QStringLiteral("%1 - %2").arg(m_currentArtist, albumTitles.first())
        : QStringLiteral("%1 albums by %2").arg(albumTitles.size()).arg(m_currentArtist);
    replaceQueueWithTracks(tracks, 0, QStringLiteral("album"), 0, sourceName);
}

void MainWindow::playArtistReplacingQueue(const QString &artistName)
{
    const QVector<Track> tracks = tracksForArtistName(artistName);
    replaceQueueWithTracks(tracks, 0, QStringLiteral("artist"), 0, artistName);
}

void MainWindow::addArtistToQueue(const QString &artistName)
{
    enqueueTracksFromMenu(tracksForArtistName(artistName), QueueAddMode::Append, false);
}

void MainWindow::startRadioFromAlbum(const QString &albumTitle)
{
    const QVector<Track> tracks = tracksForAlbumTitle(albumTitle);
    if (!tracks.isEmpty()) {
        startRadioFromSeed(tracks.first().path);
    }
}

QVector<Track> MainWindow::tracksForAlbumTitle(const QString &albumTitle) const
{
    if (m_currentArtist.isEmpty() || albumTitle.isEmpty()) {
        return {};
    }
    return m_librarySource == LibrarySource::Mpd
        ? m_database->mpdTracksForArtist(m_currentArtist, mpdMusicDirectory(), albumTitle)
        : m_database->tracksForArtist(m_currentArtist, albumTitle);
}

QVector<Track> MainWindow::tracksForArtistName(const QString &artistName) const
{
    const QString trimmed = artistName.trimmed();
    if (m_database == nullptr || trimmed.isEmpty()) {
        return {};
    }
    return m_librarySource == LibrarySource::Mpd
        ? m_database->mpdTracksForArtist(trimmed, mpdMusicDirectory())
        : m_database->tracksForArtist(trimmed);
}

void MainWindow::playNextAlbum(const QString &albumTitle)
{
    enqueueTracksFromMenu(tracksForAlbumTitle(albumTitle), QueueAddMode::PlayNext, false);
}

void MainWindow::addAlbumToQueue(const QString &albumTitle)
{
    enqueueTracksFromMenu(tracksForAlbumTitle(albumTitle), QueueAddMode::Append, false);
}

void MainWindow::playNextAlbumTemporary(const QString &albumTitle)
{
    enqueueTracksFromMenu(tracksForAlbumTitle(albumTitle), QueueAddMode::PlayNext, true);
}

void MainWindow::addAlbumToQueueTemporary(const QString &albumTitle)
{
    enqueueTracksFromMenu(tracksForAlbumTitle(albumTitle), QueueAddMode::Append, true);
}

void MainWindow::playQueueIndex(int index, bool notifyScrobbler, bool startPaused, bool explicitJump)
{
    m_player->playAt(index, notifyScrobbler, startPaused, explicitJump);
}

void MainWindow::playPreviousTrack()
{
    m_player->previous();
}

void MainWindow::playNextTrack()
{
    m_player->next();
}

void MainWindow::togglePlayback()
{
    m_player->togglePlayPause();
}

void MainWindow::playFromMpris()
{
    m_player->play();
}

void MainWindow::setVolumeFromMpris(double volume0To1)
{
    m_player->setVolume(volume0To1);
}

void MainWindow::applyPlayerVolume(double volume0To1)
{
    const int percent = std::clamp(static_cast<int>(std::lround(volume0To1 * 100.0)), 0, 100);
    if (m_playerBar != nullptr) {
        m_playerBar->setVolume(percent);
    }
    // AppCore handles MPRIS volume mirroring and state persistence.
}

void MainWindow::seekRelativeFromMpris(qint64 offsetMs)
{
    m_player->seekRelative(offsetMs);
}

void MainWindow::updatePlaybackPosition()
{
    const qint64 positionMs = std::max<qint64>(0, m_playback->position());
    const qint64 durationMs = m_playback->hasSource()
        ? std::max<qint64>(m_playback->duration(), m_player->currentTrack().durationMs)
        : std::max<qint64>(0, m_playback->duration());
    m_playerBar->setPosition(positionMs, durationMs);
    // AppCore handles MPRIS position/duration mirroring.
    if (m_playback->state() == PlaybackBackend::State::Playing || m_playback->state() == PlaybackBackend::State::Paused) {
        schedulePlaybackStateSave(false);
        m_lastHealthyTrackPath = m_player->currentTrack().path;
        m_lastHealthyPositionMs = positionMs;
    }
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

    const QString resolvedPath = resolvedReadPathForTrack(m_player->currentTrack());
    const QString directory = resolvedPath.isEmpty() ? m_player->currentTrack().parentDir : QFileInfo(resolvedPath).absolutePath();
    if (m_artworkCache != nullptr) {
        m_artworkCache->requestArtwork(QStringLiteral("current"), directory, resolvedPath, m_currentArtGeneration);
    }
}

QString MainWindow::databasePath() const
{
    return QDir(AppPaths::dataDir()).filePath(QStringLiteral("library.sqlite"));
}

QString MainWindow::playlistDatabasePath() const
{
    return QDir(AppPaths::dataDir()).filePath(QStringLiteral("playlists.sqlite"));
}

QVector<Track> MainWindow::tracksForPaths(const QStringList &paths) const
{
    QVector<Track> tracks;
    tracks.reserve(paths.size());
    for (const QString &path : paths) {
        if (path.isEmpty()) {
            continue;
        }
        const Track track = m_database->trackForPath(path);
        if (!track.path.isEmpty()) {
            tracks.push_back(track);
        }
    }
    return tracks;
}

static PlaylistItem playlistItemFromTrack(const Track &track, const QString &query)
{
    PlaylistItem item;
    item.trackPath = track.path;
    item.titleSnapshot = track.title.isEmpty() ? track.filename : track.title;
    item.artistSnapshot = track.artistName.isEmpty() ? track.albumArtistName : track.artistName;
    item.albumSnapshot = track.albumTitle;
    item.durationMs = track.durationMs;
    item.query = query;
    item.status = PlaylistItemStatus::Matched;
    item.effectiveRating0To100 = track.effectiveRating0To100;
    return item;
}

void MainWindow::openPlaylistAddModal(qint64 playlistId)
{
    if (m_playlistDb == nullptr || playlistId <= 0) {
        return;
    }
    const Playlist playlist = m_playlistDb->playlist(playlistId);
    auto *dialog = new PlaylistAddDialog(databasePath(), playlist.name, this);
    // Item ids added during this session, for C-/ undo (newest at the back).
    auto addedIds = std::make_shared<QVector<qint64>>();

    const auto refreshAdded = [this, dialog, playlistId]() {
        QSet<QString> paths;
        for (const PlaylistItem &item : m_playlistDb->items(playlistId)) {
            if (!item.trackPath.isEmpty()) {
                paths.insert(item.trackPath);
            }
        }
        dialog->setAddedPaths(paths);
    };
    refreshAdded();

    connect(dialog, &PlaylistAddDialog::itemChosen, this,
            [this, playlistId, addedIds, refreshAdded](const Track &track, const QString &query) {
                const qint64 id = m_playlistDb->addItem(playlistId, playlistItemFromTrack(track, query));
                if (id > 0) {
                    addedIds->push_back(id);
                    if (m_playlistView != nullptr) {
                        m_playlistView->reloadItems();
                        m_playlistView->reloadPlaylists();
                    }
                    syncPlaylistBackedQueue(playlistId);
                }
                refreshAdded();
            });
    connect(dialog, &PlaylistAddDialog::undoRequested, this,
            [this, playlistId, addedIds, refreshAdded, dialog]() {
                if (addedIds->isEmpty()) {
                    return;
                }
                const qint64 last = addedIds->takeLast();
                QString restoreQuery;
                for (const PlaylistItem &item : m_playlistDb->items(playlistId)) {
                    if (item.id == last) {
                        restoreQuery = item.query;
                        break;
                    }
                }
                m_playlistDb->removeItem(last);
                if (m_playlistView != nullptr) {
                    m_playlistView->reloadItems();
                    m_playlistView->reloadPlaylists();
                }
                syncPlaylistBackedQueue(playlistId);
                refreshAdded();
                dialog->setQueryText(restoreQuery);
            });

    dialog->exec();
    delete dialog;
    if (m_playlistView != nullptr) {
        m_playlistView->reloadItems();
        m_playlistView->reloadPlaylists();
    }
    syncPlaylistBackedQueue(playlistId);
}

void MainWindow::openPlaylistImportDialog(qint64 playlistId)
{
    if (m_playlistDb == nullptr || playlistId <= 0) {
        return;
    }
    const Playlist playlist = m_playlistDb->playlist(playlistId);
    PlaylistImportDialog dialog(databasePath(), playlist.name, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    commitImportResults(playlistId, dialog);
}

void MainWindow::commitImportResults(qint64 playlistId, const PlaylistImportDialog &dialog)
{
    commitImportItems(playlistId, dialog.results(), dialog.header(), dialog.resolvedPaths());
}

namespace {

// Fill a playlist item from a matcher outcome (the auto-resolved, no-triage path).
// Shared by the bulk commit and the live drop-import so a streamed item looks
// exactly like one the dialog would have produced.
void fillItemFromOutcome(PlaylistItem &item, const PlaylistImportMatch &match)
{
    const QString fallbackComment =
        match.entry.comment.isEmpty() ? match.entry.rawLine : match.entry.comment;
    item.query = match.outcome.queryUsed;
    switch (match.outcome.decision) {
    case PlaylistMatcher::Decision::Matched: {
        const Search::SearchRecord &rec = match.outcome.best;
        item.trackPath = rec.path;
        item.titleSnapshot = rec.title;
        item.artistSnapshot = rec.artistName;
        item.albumSnapshot = rec.albumTitle;
        item.durationMs = rec.durationMs;
        item.status = PlaylistItemStatus::Matched;
        item.comment = match.entry.comment;  // matched rows keep only an explicit note
        break;
    }
    case PlaylistMatcher::Decision::Approximate: {
        // Auto-pick the best guess but flag it; keep the alternatives for re-pick.
        const Search::SearchRecord &rec = match.outcome.best;
        item.trackPath = rec.path;
        item.titleSnapshot = rec.title;
        item.artistSnapshot = rec.artistName;
        item.albumSnapshot = rec.albumTitle;
        item.durationMs = rec.durationMs;
        item.status = PlaylistItemStatus::Approximate;
        item.candidatePaths = match.outcome.candidatePaths;
        item.comment = fallbackComment;
        break;
    }
    case PlaylistMatcher::Decision::MultiMatch:
        item.titleSnapshot = match.entry.title;
        item.artistSnapshot = match.entry.artist;
        item.albumSnapshot = match.entry.album;
        item.durationMs = match.entry.durationMs;
        item.status = PlaylistItemStatus::MultiMatch;
        item.candidatePaths = match.outcome.candidatePaths;
        item.comment = fallbackComment;
        break;
    case PlaylistMatcher::Decision::Pending:
        item.titleSnapshot = match.entry.title;
        item.artistSnapshot = match.entry.artist;
        item.albumSnapshot = match.entry.album;
        item.durationMs = match.entry.durationMs;
        item.status = PlaylistItemStatus::Pending;
        item.comment = fallbackComment;
        break;
    }
}

// The immutable provenance fields (kept even after a row is matched/edited).
void applyImportProvenance(PlaylistItem &item, const PlaylistImportMatch &match)
{
    item.externalId = match.entry.externalId;
    item.addedAt = match.entry.addedAt;
    QString sourceText = match.entry.artist.isEmpty()
        ? match.entry.title
        : QStringLiteral("%1 - %2").arg(match.entry.artist, match.entry.title);
    if (sourceText.isEmpty()) {
        sourceText = match.entry.rawLine;
    }
    if (!match.entry.album.isEmpty() && !sourceText.isEmpty()) {
        sourceText += QStringLiteral(" — %1").arg(match.entry.album);
    }
    item.sourceText = sourceText;
}

} // namespace

PlaylistItem MainWindow::playlistItemFromImportMatch(const PlaylistImportMatch &match)
{
    PlaylistItem item;
    fillItemFromOutcome(item, match);
    applyImportProvenance(item, match);
    return item;
}

void MainWindow::commitImportItems(qint64 playlistId, const QVector<PlaylistImportMatch> &matches,
                                   const PlaylistImport::ImportHeader &header,
                                   const QHash<int, QString> &resolved)
{
    const Playlist playlist = m_playlistDb->playlist(playlistId);

    // Existing import source ids in this playlist → skip re-importing the same item.
    QSet<QString> seenExternalIds;
    const QVector<PlaylistItem> existingItems = m_playlistDb->items(playlistId);
    for (const PlaylistItem &existing : existingItems) {
        if (!existing.externalId.isEmpty()) {
            seenExternalIds.insert(existing.externalId);
        }
    }

    // Honor a JSONL playlist header: name an as-yet-empty target and fill a blank
    // comment. Renaming a populated playlist would surprise the user, so guard on it.
    if (header.present) {
        if (existingItems.isEmpty() && !header.name.isEmpty() && header.name != playlist.name) {
            m_playlistDb->renamePlaylist(playlistId, header.name);
        }
        if (!header.comment.isEmpty() && playlist.comment.isEmpty()) {
            m_playlistDb->setPlaylistComment(playlistId, header.comment);
        }
    }

    int added = 0;
    int skipped = 0;
    for (int i = 0; i < matches.size(); ++i) {
        const PlaylistImportMatch &match = matches.at(i);
        const QString externalId = match.entry.externalId;
        if (!externalId.isEmpty() && seenExternalIds.contains(externalId)) {
            ++skipped;
            continue;
        }

        // Prefer an explicit (JSONL) comment; otherwise keep the raw source line on
        // unresolved rows so the user can see what failed to match.
        const QString fallbackComment =
            match.entry.comment.isEmpty() ? match.entry.rawLine : match.entry.comment;

        PlaylistItem item;
        // A triage pick in the preview resolves the row to a chosen candidate.
        bool resolvedByPick = false;
        const QString chosenPath = resolved.value(i);
        if (chosenPath == PlaylistImportDialog::noMatchMarker()) {
            // User rejected all candidates → land it empty (Pending), not MultiMatch.
            item.titleSnapshot = match.entry.title;
            item.artistSnapshot = match.entry.artist;
            item.albumSnapshot = match.entry.album;
            item.durationMs = match.entry.durationMs;
            item.status = PlaylistItemStatus::Pending;
            item.comment = fallbackComment;
            item.query = match.outcome.queryUsed;
            resolvedByPick = true;
        } else if (!chosenPath.isEmpty()) {
            const Track picked = m_database->trackForPath(chosenPath);
            if (!picked.path.isEmpty()) {
                item = playlistItemFromTrack(picked, match.outcome.queryUsed);
                item.comment = fallbackComment;
                resolvedByPick = true;
            }
        }
        if (!resolvedByPick) {
            fillItemFromOutcome(item, match);
        }
        applyImportProvenance(item, match);
        if (m_playlistDb->addItem(playlistId, item) > 0) {
            ++added;
            if (!externalId.isEmpty()) {
                seenExternalIds.insert(externalId);
            }
        }
    }
    if (m_playlistView != nullptr) {
        m_playlistView->reloadItems();
        m_playlistView->reloadPlaylists();
    }
    syncPlaylistBackedQueue(playlistId);
    const QString message = skipped > 0
        ? QStringLiteral("Imported %1 items into \"%2\" (%3 duplicates skipped)")
              .arg(added).arg(playlist.name).arg(skipped)
        : QStringLiteral("Imported %1 items into \"%2\"").arg(added).arg(playlist.name);
    statusBar()->showMessage(message, 5000);
}

void MainWindow::importAsNewPlaylist()
{
    if (m_playlistDb == nullptr) {
        return;
    }
    PlaylistImportDialog dialog(databasePath(), QStringLiteral("new playlist"), this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    // Name the new playlist from the JSONL header, else ask.
    const PlaylistImport::ImportHeader header = dialog.header();
    QString name = header.present ? header.name.trimmed() : QString();
    if (name.isEmpty()) {
        bool ok = false;
        name = QInputDialog::getText(this, QStringLiteral("Import playlist"),
                                     QStringLiteral("Playlist name:"), QLineEdit::Normal,
                                     QStringLiteral("Imported playlist"), &ok)
                   .trimmed();
        if (!ok || name.isEmpty()) {
            return;
        }
    }
    const qint64 id = m_playlistDb->createPlaylist(name, header.comment);
    if (id <= 0) {
        return;
    }
    commitImportResults(id, dialog);
}

QString MainWindow::uniquePlaylistName(const QString &base) const
{
    QSet<QString> taken;
    for (const Playlist &p : m_playlistDb->playlists()) {
        taken.insert(p.name);
    }
    if (!taken.contains(base)) {
        return base;
    }
    for (int n = 2;; ++n) {  // distinct, never silently merge two same-named files
        const QString candidate = QStringLiteral("%1 (%2)").arg(base).arg(n);
        if (!taken.contains(candidate)) {
            return candidate;
        }
    }
}

void MainWindow::importDroppedFiles(const QStringList &paths)
{
    if (m_playlistDb == nullptr || m_database == nullptr || paths.isEmpty()) {
        return;
    }
    // Only accept drops while the playlist view is the active surface — a drop on
    // any other view is a no-op (the placeholder/spinner UX only makes sense here).
    if (m_mainView != MainView::Playlist) {
        return;
    }
    // Parse on the UI thread (cheap) and create one empty placeholder playlist per
    // file *now*, so they appear immediately; the slow matching runs in the worker
    // and fills each placeholder live.
    QVector<DropImportJob> jobs;
    for (const QString &file : paths) {
        PlaylistImport::ImportHeader header;
        QString error;
        const QVector<PlaylistImport::ImportEntry> entries =
            PlaylistImport::parseFile(file, &error, &header);
        if (entries.isEmpty()) {
            continue;
        }
        QString name = (header.present && !header.name.trimmed().isEmpty())
            ? header.name.trimmed()
            : QFileInfo(file).completeBaseName();
        if (name.isEmpty()) {
            name = QStringLiteral("Imported playlist");
        }
        const qint64 id = m_playlistDb->createPlaylist(uniquePlaylistName(name), header.comment);
        if (id <= 0) {
            continue;
        }
        jobs.push_back({id, entries});
        m_dropImportPlaylists.insert(id);
    }
    if (jobs.isEmpty()) {
        return;
    }

    // Surface the placeholders (with their spinners) and drop the user onto the
    // first one so they watch it fill.
    if (m_playlistView != nullptr) {
        m_playlistView->reloadPlaylists();
        for (const DropImportJob &job : jobs) {
            m_playlistView->setPlaylistImporting(job.playlistId, true);
        }
        m_playlistView->selectPlaylist(jobs.first().playlistId);
    }

    // An import is already running — auto-matching is linear, so queue these new
    // placeholders onto it instead of rejecting the drop. finishDropImport drains
    // the queue into the same worker (reusing its cached match index).
    if (m_dropImportWorker != nullptr) {
        m_pendingDropJobs.append(jobs);
        statusBar()->showMessage(
            QStringLiteral("Queued %1 more playlist(s) onto the running import…").arg(jobs.size()),
            0);
        return;
    }

    statusBar()->showMessage(
        QStringLiteral("Importing %1 playlist(s)… matching in the background (Esc to interrupt)")
            .arg(jobs.size()),
        0);

    qRegisterMetaType<PlaylistImportMatch>();
    qRegisterMetaType<DropImportJob>();
    qRegisterMetaType<QVector<DropImportJob>>();
    m_dropImportThread = new QThread(this);
    m_dropImportWorker = new PlaylistDropImportWorker(databasePath());
    m_dropImportWorker->moveToThread(m_dropImportThread);
    connect(m_dropImportThread, &QThread::finished, m_dropImportWorker, &QObject::deleteLater);
    connect(m_dropImportWorker, &PlaylistDropImportWorker::itemMatched,
            this, &MainWindow::onDropImportItemMatched, Qt::QueuedConnection);
    connect(m_dropImportWorker, &PlaylistDropImportWorker::playlistFinished,
            this, &MainWindow::onDropImportPlaylistFinished, Qt::QueuedConnection);
    connect(m_dropImportWorker, &PlaylistDropImportWorker::finished,
            this, &MainWindow::finishDropImport, Qt::QueuedConnection);
    connect(m_dropImportWorker, &PlaylistDropImportWorker::error, this, [this](const QString &message) {
        statusBar()->showMessage(QStringLiteral("Import error: %1").arg(message), 6000);
    });
    m_dropImportThread->start();
    QMetaObject::invokeMethod(m_dropImportWorker, "run", Qt::QueuedConnection,
                              Q_ARG(QVector<DropImportJob>, jobs));
}

void MainWindow::onDropImportItemMatched(qint64 playlistId, const PlaylistImportMatch &match)
{
    if (m_playlistDb == nullptr) {
        return;
    }
    // Drop late matches for a playlist whose import was stopped or deleted — the
    // worker may emit a few more before it notices the skip flag.
    if (!m_dropImportPlaylists.contains(playlistId)) {
        return;
    }
    const PlaylistItem item = playlistItemFromImportMatch(match);
    m_playlistDb->addItem(playlistId, item);
    if (m_playlistView != nullptr) {
        m_playlistView->refreshImportingPlaylist(playlistId);
    }
    syncPlaylistBackedQueue(playlistId);
}

void MainWindow::stopPlaylistImport(qint64 playlistId)
{
    // Queued but not yet started: just drop its job so the worker never sees it.
    m_pendingDropJobs.erase(
        std::remove_if(m_pendingDropJobs.begin(), m_pendingDropJobs.end(),
                       [playlistId](const DropImportJob &job) { return job.playlistId == playlistId; }),
        m_pendingDropJobs.end());
    // In flight: ask the worker to stop filling just this one.
    if (m_dropImportWorker != nullptr) {
        m_dropImportWorker->skipPlaylist(playlistId);
    }
    m_dropImportPlaylists.remove(playlistId);
    if (m_playlistView != nullptr) {
        m_playlistView->setPlaylistImporting(playlistId, false);
    }
}

void MainWindow::onDropImportPlaylistFinished(qint64 playlistId)
{
    m_dropImportPlaylists.remove(playlistId);
    if (m_playlistView != nullptr) {
        m_playlistView->setPlaylistImporting(playlistId, false);
        m_playlistView->refreshImportingPlaylist(playlistId);
    }
}

void MainWindow::cancelDropImport()
{
    if (m_dropImportWorker != nullptr) {
        m_dropImportWorker->requestStop();
        statusBar()->showMessage(QStringLiteral("Interrupting import…"), 2000);
    }
}

void MainWindow::finishDropImport(bool interrupted)
{
    // Drops that landed while this run was matching were queued; feed them to the
    // same worker (its match index is already built) instead of tearing down.
    if (!interrupted && !m_pendingDropJobs.isEmpty() && m_dropImportWorker != nullptr) {
        QVector<DropImportJob> next = std::move(m_pendingDropJobs);
        m_pendingDropJobs.clear();
        statusBar()->showMessage(
            QStringLiteral("Importing %1 more playlist(s)… (Esc to interrupt)").arg(next.size()), 0);
        QMetaObject::invokeMethod(m_dropImportWorker, "run", Qt::QueuedConnection,
                                  Q_ARG(QVector<DropImportJob>, next));
        return;  // worker/thread stay alive
    }
    m_pendingDropJobs.clear();
    // Clear any spinners still showing (an interrupt skips the rest).
    if (m_playlistView != nullptr) {
        for (const qint64 id : m_dropImportPlaylists) {
            m_playlistView->setPlaylistImporting(id, false);
        }
    }
    m_dropImportPlaylists.clear();
    if (m_dropImportThread != nullptr) {
        m_dropImportThread->quit();
        m_dropImportThread->wait(3000);
        m_dropImportThread = nullptr;
        m_dropImportWorker = nullptr;  // deleteLater on thread finish
    }
    if (m_playlistView != nullptr) {
        m_playlistView->reloadPlaylists();
        m_playlistView->reloadItems();
    }
    statusBar()->showMessage(
        interrupted ? QStringLiteral("Import interrupted — partial playlists kept.")
                    : QStringLiteral("Import complete — resolve multi/pending via the 'e' edit modal."),
        6000);
}

void MainWindow::openPlaylistEditModal(qint64 playlistId, qint64 itemId, const QString &query)
{
    if (m_playlistDb == nullptr || playlistId <= 0 || itemId <= 0) {
        return;
    }
    const Playlist playlist = m_playlistDb->playlist(playlistId);
    auto *dialog = new PlaylistAddDialog(databasePath(), playlist.name, this);
    dialog->setEditMode(true);

    QSet<QString> paths;
    PlaylistItem editedItem;
    for (const PlaylistItem &item : m_playlistDb->items(playlistId)) {
        if (!item.trackPath.isEmpty()) {
            paths.insert(item.trackPath);
        }
        if (item.id == itemId) {
            editedItem = item;
        }
    }
    dialog->setAddedPaths(paths);
    dialog->setQueryText(query);
    // For MultiMatch imports, surface the stored candidate shortlist first.
    dialog->setPreferredPaths(editedItem.candidatePaths);

    // A one-line reference above the box: what this row expects. While an import
    // is unresolved it shows the immutable import string ("Imported as: …"); a
    // missing row shows the library track it had registered; otherwise it just
    // names the track being edited.
    const auto snapshotDisplay = [](const PlaylistItem &it) -> QString {
        QString name = it.artistSnapshot.isEmpty()
            ? it.titleSnapshot
            : QStringLiteral("%1 — %2").arg(it.artistSnapshot, it.titleSnapshot);
        if (!name.isEmpty() && !it.albumSnapshot.isEmpty()) {
            name += QStringLiteral(" · %1").arg(it.albumSnapshot);
        }
        return name;
    };
    const bool unresolvedImport = !editedItem.sourceText.isEmpty()
        && (editedItem.status == PlaylistItemStatus::Pending
            || editedItem.status == PlaylistItemStatus::MultiMatch
            || editedItem.status == PlaylistItemStatus::Approximate);
    QString context;
    if (unresolvedImport) {
        context = QStringLiteral("Imported as:  %1").arg(editedItem.sourceText);
    } else if (editedItem.status == PlaylistItemStatus::Missing) {
        const QString shown = snapshotDisplay(editedItem);
        const QString fallback = shown.isEmpty() ? editedItem.sourceText : shown;
        if (!fallback.isEmpty()) {
            context = QStringLiteral("Missing track was:  %1").arg(fallback);
        }
    } else if (const QString shown = snapshotDisplay(editedItem); !shown.isEmpty()) {
        context = QStringLiteral("Editing:  %1").arg(shown);
    }
    dialog->setEditContext(context);

    connect(dialog, &PlaylistAddDialog::itemChosen, this,
            [this, itemId, comment = editedItem.comment, sourceText = editedItem.sourceText](
                const Track &track, const QString &chosenQuery) {
                PlaylistItem item = playlistItemFromTrack(track, chosenQuery);
                item.id = itemId;
                item.comment = comment;        // a replacement pick must not drop the note
                item.sourceText = sourceText;  // nor the original imported string
                m_playlistDb->updateItem(item);
            });

    dialog->exec();
    delete dialog;
    if (m_playlistView != nullptr) {
        m_playlistView->reloadItems();
        m_playlistView->reloadPlaylists();
        // Keep keyboard focus on the row just edited rather than snapping to the top.
        m_playlistView->selectItemById(itemId);
    }
    syncPlaylistBackedQueue(playlistId);
}

void MainWindow::resolvePlaylistMultiMatches(qint64 playlistId)
{
    if (m_playlistDb == nullptr || m_database == nullptr || playlistId <= 0) {
        return;
    }
    int resolved = 0;
    int skipped = 0;
    for (const PlaylistItem &original : m_playlistDb->items(playlistId)) {
        if (original.status != PlaylistItemStatus::MultiMatch) {
            continue;
        }
        // The candidate list is the matcher's close set in ranked order, so the
        // first is the top guess (the one the edit modal hoists to the cursor).
        const QString topCandidate = original.candidatePaths.value(0);
        const Track track = topCandidate.isEmpty() ? Track() : m_database->trackForPath(topCandidate);
        if (track.path.isEmpty()) {   // no candidate, or it left the library
            ++skipped;
            continue;
        }
        PlaylistItem item = playlistItemFromTrack(track, original.query);
        item.id = original.id;
        // Auto-picked below the user's eye — flag Approximate so it stays marked
        // for a glance instead of passing as a confident match.
        item.status = PlaylistItemStatus::Approximate;
        item.comment = original.comment;             // keep the user's note,
        item.sourceText = original.sourceText;        // the immutable import string,
        item.externalId = original.externalId;        // the source id,
        item.addedAt = original.addedAt;              // and the original add time.
        item.candidatePaths = original.candidatePaths; // keep the shortlist for re-editing
        if (m_playlistDb->updateItem(item)) {
            ++resolved;
        }
    }
    if (m_playlistView != nullptr) {
        m_playlistView->reloadItems();
        m_playlistView->reloadPlaylists();
    }
    syncPlaylistBackedQueue(playlistId);
    if (resolved == 0 && skipped == 0) {
        return;
    }
    QString message = QStringLiteral("Resolved %1 multi-match%2 to best guess (Approximate — give them a glance).")
                          .arg(resolved)
                          .arg(resolved == 1 ? QString() : QStringLiteral("es"));
    if (skipped > 0) {
        message += QStringLiteral(" %1 skipped (no candidate in the library).").arg(skipped);
    }
    statusBar()->showMessage(message, 6000);
}

void MainWindow::openAddToPlaylistDialog(const QVector<Track> &tracks)
{
    if (m_playlistDb == nullptr || tracks.isEmpty()) {
        return;
    }
    const QVector<Playlist> playlists = m_playlistDb->playlists();
    QStringList names;
    names.reserve(playlists.size());
    for (const Playlist &p : playlists) {
        names << p.name;
    }
    bool ok = false;
    const QString chosen = QInputDialog::getItem(
        this, QStringLiteral("Add to playlist"), QStringLiteral("Playlist:"),
        names, 0, /*editable=*/true, &ok).trimmed();
    if (!ok || chosen.isEmpty()) {
        return;
    }
    qint64 id = 0;
    for (const Playlist &p : playlists) {
        if (p.name == chosen) {
            id = p.id;
            break;
        }
    }
    if (id <= 0) {
        id = m_playlistDb->createPlaylist(chosen);
    }
    if (id <= 0) {
        return;
    }
    for (const Track &track : tracks) {
        if (track.path.isEmpty()) {
            continue;
        }
        m_playlistDb->addItem(id, playlistItemFromTrack(track, QString()));
    }
    if (m_playlistView != nullptr) {
        m_playlistView->reloadItems();
        m_playlistView->reloadPlaylists();
    }
    syncPlaylistBackedQueue(id);
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
