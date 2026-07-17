#include "ui/ScanController.h"
#include "ui/MainWindow.h"
#include "db/Database.h"
#include "db/PlaylistDatabase.h"
#include "db/SettingsStore.h"
#include "player/PlayerCore.h"
#include "scanner/ScanPipeline.h"
#include "ui/PlaylistView.h"
#include "ui/SearchView.h"

#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStatusBar>
#include <QThread>
#include <QTimer>

Q_DECLARE_LOGGING_CATEGORY(uiLog)

namespace {
QString cleanDirectoryPath(const QString &path) { return QDir::cleanPath(QFileInfo(path).absoluteFilePath()); }
QVector<ScanRoot> deduplicatedScanRoots(QVector<ScanRoot> roots)
{
    QVector<ScanRoot> deduped;
    for (ScanRoot &root : roots) {
        root.path = cleanDirectoryPath(root.path);
        if (root.path.isEmpty()) continue;
        bool covered = false;
        for (const ScanRoot &existing : std::as_const(deduped)) {
            if (root.path == existing.path || root.path.startsWith(existing.path + QDir::separator())) {
                covered = true;
                break;
            }
        }
        if (!covered) deduped.push_back(root);
    }
    return deduped;
}
}

ScanController::ScanController(MainWindow &window) : QObject(&window), m_window(window) {}

void ScanController::startScan(const QString &rootPath)
{
    startScan(rootPath, 0);
}

void ScanController::startScan(const QString &rootPath, int scanRootId)
{
    if (m_window.m_scanThread != nullptr) {
        m_window.statusBar()->showMessage(QStringLiteral("A scan is already running"), 5000);
        return;
    }

    qCInfo(uiLog) << "starting scan" << rootPath;
    m_window.m_activeScanRootId = scanRootId;
    m_window.m_activeScanRootPath = cleanDirectoryPath(rootPath);
    m_window.statusBar()->showMessage(QStringLiteral("Scanning %1").arg(m_window.m_activeScanRootPath));
    m_window.m_scanProgress->setVisible(true);
    m_window.m_stopScanButton->setEnabled(true);
    m_window.m_stopScanButton->setVisible(true);
    ensureIngestSession();
    // A foreground scan supersedes the background fill; pause it (it resumes once
    // the scan finishes and re-pumps the placeholder backlog).
    if (m_window.m_fillPipeline != nullptr) {
        m_window.m_fillPipeline->cancel();
    }

    ScanPipeline::Options options;
    options.forceFullRescan = m_window.m_forceFullRescan;
    options.profile = static_cast<ScanPipeline::Profile>(scanProfileSetting());
    options.guessPlaceholders = guessedPlaceholdersEnabled();

    m_window.m_scanThread = new QThread(this);
    m_window.m_scanPipeline = new ScanPipeline(m_window.m_activeScanRootPath, scanRootId,
                                      m_window.m_database->trackFingerprints(m_window.m_activeScanRootPath), options);
    m_window.m_scanPipeline->moveToThread(m_window.m_scanThread);

    connect(m_window.m_scanThread, &QThread::started, m_window.m_scanPipeline, &ScanPipeline::run);
    connect(m_window.m_scanPipeline, &ScanPipeline::enumeratedReady, this, &ScanController::ingestEnumeratedPlaceholders);
    connect(m_window.m_scanPipeline, &ScanPipeline::batchReady, this, &ScanController::ingestScanBatch);
    connect(m_window.m_scanPipeline, &ScanPipeline::progress, this,
            [this](qint64 enumerated, qint64 toProcess, qint64 processed, const QString &phase) {
                // The foreground pass only enumerates and re-reads *changed* files;
                // new files are deferred to the background metadata fill.
                if (phase == QStringLiteral("enumerating")) {
                    m_window.statusBar()->showMessage(QStringLiteral("Scanning: enumerating files..."));
                } else if (toProcess > 0) {
                    m_window.statusBar()->showMessage(QStringLiteral("Scanning: re-read %1 of %2 changed (%3 found)")
                                                 .arg(processed).arg(toProcess).arg(enumerated));
                } else {
                    m_window.statusBar()->showMessage(QStringLiteral("Scanning: %1 files found").arg(enumerated));
                }
            });
    connect(m_window.m_scanPipeline, &ScanPipeline::missingReady, this, &ScanController::markScannedTracksMissing);
    connect(m_window.m_scanPipeline, &ScanPipeline::finished, this, &ScanController::finishScan);
    connect(m_window.m_scanPipeline, &ScanPipeline::finished, m_window.m_scanThread, &QThread::quit);
    connect(m_window.m_scanThread, &QThread::finished, m_window.m_scanPipeline, &QObject::deleteLater);
    connect(m_window.m_scanThread, &QThread::finished, m_window.m_scanThread, &QObject::deleteLater);
    connect(m_window.m_scanThread, &QThread::finished, this, [this]() {
        m_window.m_scanThread = nullptr;
        m_window.m_scanPipeline = nullptr;
        if (!m_window.m_pendingScanRoots.isEmpty()) {
            startNextQueuedSourceScan();
        } else {
            m_window.m_forceFullRescan = false;
            pumpMetadataFill();  // lazily tag-read the placeholders this scan created
        }
    });

    m_window.m_scanThread->start();
}

void ScanController::scanEnabledSourceDirectories()
{
    scanSourceRoots(m_window.m_database->enabledScanRoots());
}

void ScanController::forceRescanEnabledSourceDirectories()
{
    if (m_window.m_scanThread != nullptr) {
        m_window.statusBar()->showMessage(QStringLiteral("A scan is already running"), 5000);
        return;
    }
    m_window.m_forceFullRescan = true;
    scanSourceRoots(m_window.m_database->enabledScanRoots());
}

void ScanController::scanSourceRoots(const QVector<ScanRoot> &roots)
{
    if (m_window.m_scanThread != nullptr) {
        m_window.statusBar()->showMessage(QStringLiteral("A scan is already running"), 5000);
        return;
    }

    m_window.m_pendingScanRoots = deduplicatedScanRoots(roots);
    if (m_window.m_pendingScanRoots.isEmpty()) {
        m_window.statusBar()->showMessage(QStringLiteral("No scan-enabled source directories"), 5000);
        return;
    }
    startNextQueuedSourceScan();
}

void ScanController::startNextQueuedSourceScan()
{
    if (m_window.m_scanThread != nullptr || m_window.m_pendingScanRoots.isEmpty()) {
        return;
    }

    const ScanRoot root = m_window.m_pendingScanRoots.takeFirst();
    startScan(root.path, root.id);
}

void ScanController::cancelScan()
{
    if (m_window.m_scanPipeline == nullptr) {
        return;
    }

    m_window.m_scanPipeline->cancel();
    m_window.m_pendingScanRoots.clear();
    m_window.m_forceFullRescan = false;
    m_window.m_stopScanButton->setEnabled(false);
    m_window.statusBar()->showMessage(QStringLiteral("Canceling scan..."), 5000);
}

void ScanController::ingestScanBatch(const QVector<Track> &tracks)
{
    if (tracks.isEmpty()) {
        return;
    }

    if (!m_window.m_database->beginTransaction()) {
        QMessageBox::warning(&m_window, QStringLiteral("Scanner"), m_window.m_database->lastError());
        return;
    }
    for (const Track &track : tracks) {
        if (!m_window.m_database->upsertTrack(track)) {
            QMessageBox::warning(&m_window, QStringLiteral("Scanner"), m_window.m_database->lastError());
            break;
        }
    }
    if (!m_window.m_database->commitTransaction()) {
        QMessageBox::warning(&m_window, QStringLiteral("Scanner"), m_window.m_database->lastError());
        return;
    }

    m_window.patchQueueTracksFromMetadata(tracks);
    scheduleIncrementalRefresh();
}

void ScanController::ingestEnumeratedPlaceholders(const QVector<Track> &tracks)
{
    if (tracks.isEmpty()) {
        return;
    }
    if (!m_window.m_database->insertEnumeratedPlaceholders(tracks)) {
        QMessageBox::warning(&m_window, QStringLiteral("Scanner"), m_window.m_database->lastError());
        return;
    }
    // Placeholders only surface in the directory/file view; coalesce the refresh
    // with the rest of the ingest so a flood of new paths doesn't rebuild per chunk.
    scheduleIncrementalRefresh();
}

void ScanController::scheduleIncrementalRefresh()
{
    // Throttle (not debounce): during a continuous scan/fill, batches arrive faster
    // than the interval, so we refresh at most once per window while dirty rather
    // than never until the stream pauses. Keeps the browse/explorer filling in
    // light chunks without rebuilding on every batch.
    if (m_window.m_incrementalRefreshTimer == nullptr) {
        m_window.m_incrementalRefreshTimer = new QTimer(this);
        m_window.m_incrementalRefreshTimer->setSingleShot(true);
        connect(m_window.m_incrementalRefreshTimer, &QTimer::timeout, this, [this]() {
            if (m_window.m_incrementalRefreshDirty) {
                m_window.m_incrementalRefreshDirty = false;
                m_window.refreshArtists();
                m_window.refreshLibraryFileExplorer();
            }
        });
    }
    m_window.m_incrementalRefreshDirty = true;
    if (!m_window.m_incrementalRefreshTimer->isActive()) {
        m_window.m_incrementalRefreshTimer->start(1500);
    }
}

void ScanController::flushIncrementalRefresh()
{
    if (m_window.m_incrementalRefreshTimer != nullptr) {
        m_window.m_incrementalRefreshTimer->stop();
    }
    m_window.m_incrementalRefreshDirty = false;
    m_window.refreshArtists();
    m_window.refreshLibraryFileExplorer();
}

int ScanController::scanProfileSetting() const
{
    const QString value = m_window.m_state->setting(QStringLiteral("scan.profile"), QStringLiteral("balanced"));
    if (value == QStringLiteral("background")) {
        return 0;
    }
    if (value == QStringLiteral("turbo")) {
        return 2;
    }
    return 1;
}

int ScanController::analysisPowerSetting() const
{
    const QString value = m_window.m_state->setting(QStringLiteral("analysis.power"), QStringLiteral("background"));
    if (value == QStringLiteral("balanced")) {
        return 1;
    }
    if (value == QStringLiteral("turbo")) {
        return 2;
    }
    return 0;
}

bool ScanController::guessedPlaceholdersEnabled() const
{
    return m_window.m_state->setting(QStringLiteral("scan.guessedPlaceholders"), QStringLiteral("1")) != QStringLiteral("0");
}

void ScanController::ensureIngestSession()
{
    if (!m_window.m_ingestSessionActive) {
        m_window.m_database->beginScanSession();
        m_window.m_ingestSessionActive = true;
    }
}

void ScanController::endIngestSessionIfIdle()
{
    if (m_window.m_ingestSessionActive && m_window.m_scanThread == nullptr && m_window.m_fillThread == nullptr) {
        m_window.m_database->endScanSession();
        m_window.m_ingestSessionActive = false;
    }
}

QStringList ScanController::nextFillChunk()
{
    // Prefer the directory the user is looking at (on-access prioritization),
    // then drain the rest of the backlog in bounded chunks.
    if (!m_window.m_priorityFillDir.isEmpty()) {
        const QStringList dirPaths = m_window.m_database->enumeratedOnlyPaths(m_window.m_priorityFillDir, 256);
        if (!dirPaths.isEmpty()) {
            return dirPaths;
        }
        m_window.m_priorityFillDir.clear();
    }
    return m_window.m_database->enumeratedOnlyPaths({}, 512);
}

void ScanController::pumpMetadataFill()
{
    // One ingest worker at a time: never run a fill alongside a foreground scan or
    // another fill — that would double-read files and thrash a slow/HDD mount.
    if (m_window.m_scanThread != nullptr || m_window.m_fillThread != nullptr || m_window.m_librarySource != LibrarySource::Local) {
        return;
    }
    const QStringList chunk = nextFillChunk();
    if (chunk.isEmpty()) {
        endIngestSessionIfIdle();
        return;
    }
    startMetadataFill(chunk);
}

void ScanController::startMetadataFill(const QStringList &paths)
{
    if (paths.isEmpty() || m_window.m_fillThread != nullptr || m_window.m_scanThread != nullptr) {
        return;
    }
    ensureIngestSession();

    ScanPipeline::Options options;
    options.lowPriority = true;
    options.batchSize = 64;  // small batches keep the UI fill smooth
    options.profile = static_cast<ScanPipeline::Profile>(scanProfileSetting());

    const QString hint = QFileInfo(paths.first()).absolutePath();
    m_window.m_fillThread = new QThread(this);
    m_window.m_fillPipeline = new ScanPipeline(hint, paths, options);
    m_window.m_fillPipeline->moveToThread(m_window.m_fillThread);
    connect(m_window.m_fillThread, &QThread::started, m_window.m_fillPipeline, &ScanPipeline::run);
    connect(m_window.m_fillPipeline, &ScanPipeline::batchReady, this, &ScanController::ingestScanBatch);
    connect(m_window.m_fillPipeline, &ScanPipeline::progress, this,
            [this](qint64, qint64 toProcess, qint64 processed, const QString &phase) {
                Q_UNUSED(processed);
                if (phase == QStringLiteral("filling") && toProcess > 0) {
                    // Show the live backlog (this chunk + everything still queued),
                    // not just the chunk size — one cheap indexed COUNT per batch.
                    m_window.statusBar()->showMessage(
                        QStringLiteral("Filling metadata: %1 tracks remaining").arg(m_window.m_database->enumeratedOnlyCount()),
                        2000);
                }
            });
    connect(m_window.m_fillPipeline, &ScanPipeline::finished, this, &ScanController::finishMetadataFill);
    connect(m_window.m_fillPipeline, &ScanPipeline::finished, m_window.m_fillThread, &QThread::quit);
    connect(m_window.m_fillThread, &QThread::finished, m_window.m_fillPipeline, &QObject::deleteLater);
    connect(m_window.m_fillThread, &QThread::finished, m_window.m_fillThread, &QObject::deleteLater);
    connect(m_window.m_fillThread, &QThread::finished, this, [this]() {
        m_window.m_fillThread = nullptr;
        m_window.m_fillPipeline = nullptr;
        pumpMetadataFill();  // next chunk, or end the ingest session when drained
    });
    m_window.m_fillThread->start();
}

void ScanController::finishMetadataFill(qint64 enumerated, qint64 indexed, qint64 skipped, bool canceled)
{
    Q_UNUSED(enumerated);
    Q_UNUSED(indexed);
    Q_UNUSED(skipped);
    Q_UNUSED(canceled);
    // ingestScanBatch already refreshed the views incrementally during the chunk;
    // when the whole backlog is drained, do a final browse + search-index refresh.
    if (m_window.m_database->enumeratedOnlyPaths({}, 1).isEmpty()) {
        qCInfo(uiLog) << "background metadata fill complete";
        flushIncrementalRefresh();
        if (m_window.m_searchView != nullptr) {
            m_window.m_searchView->invalidateIndex(m_window.databasePath());
        }
        m_window.statusBar()->showMessage(QStringLiteral("Library metadata complete"), 4000);
    }
}

void ScanController::ensureDirectoryScanned(const QString &directory)
{
    if (directory.isEmpty() || m_window.m_librarySource != LibrarySource::Local) {
        return;
    }
    if (m_window.m_database->enumeratedOnlyPaths(directory, 1).isEmpty()) {
        return;  // nothing pending in this directory
    }
    // Jump this directory to the front of the fill so opening it reads its tags now.
    m_window.m_priorityFillDir = directory;
    pumpMetadataFill();
}

void ScanController::finishScan(qint64 enumerated, qint64 indexed, qint64 skipped, bool canceled)
{
    // This is the foreground pass finishing (enumerate + re-read changed files), not
    // the whole library: new files were turned into placeholders and their metadata
    // is read lazily by the background fill. Report both phases honestly.
    const int pendingFill = m_window.m_database->enumeratedOnlyCount();
    qCInfo(uiLog).nospace() << "scan pass finished: enumerated " << enumerated
                            << ", re-read " << indexed << " changed, " << skipped << " unchanged, "
                            << pendingFill << " queued for background metadata fill"
                            << (canceled ? " (canceled)" : "");
    const bool sourceScan = m_window.m_activeScanRootId > 0;
    const QString finishedRootPath = m_window.m_activeScanRootPath;
    if (sourceScan) {
        m_window.m_database->setScanRootLastScanned(m_window.m_activeScanRootId, canceled ? QStringLiteral("Canceled") : QString());
    }
    m_window.m_activeScanRootId = 0;
    m_window.m_activeScanRootPath.clear();
    m_window.m_scanProgress->setVisible(false);
    m_window.m_stopScanButton->setVisible(false);
    m_window.m_stopScanButton->setEnabled(false);
    QString summary;
    if (canceled) {
        summary = QStringLiteral("Scan canceled: %1 enumerated, %2 unchanged").arg(enumerated).arg(skipped);
    } else if (pendingFill > 0) {
        summary = QStringLiteral("Scan complete: %1 files (%2 changed, %3 unchanged), reading metadata for %4 in the background")
                      .arg(enumerated).arg(indexed).arg(skipped).arg(pendingFill);
    } else {
        summary = QStringLiteral("Scan complete: %1 files (%2 changed, %3 unchanged)")
                      .arg(enumerated).arg(indexed).arg(skipped);
    }
    m_window.statusBar()->showMessage(summary, 10000);
    flushIncrementalRefresh();
    // Rebuild the search index with fresh library data
    if (!canceled && m_window.m_searchView != nullptr) {
        m_window.m_searchView->invalidateIndex(m_window.databasePath());
    }
    if (!canceled && !m_window.m_pendingScanRoots.isEmpty()) {
        m_window.statusBar()->showMessage(QStringLiteral("Source scan complete: %1").arg(finishedRootPath), 3000);
    } else if (sourceScan && !canceled) {
        m_window.statusBar()->showMessage(QStringLiteral("Source scans complete"), 10000);
    }
}

void ScanController::markScannedTracksMissing(const QStringList &paths)
{
    if (paths.isEmpty()) {
        return;
    }
    if (!m_window.m_database->beginTransaction()) {
        return;
    }
    const int marked = m_window.m_database->markTracksMissing(paths);
    m_window.m_database->commitTransaction();
    if (marked > 0) {
        m_window.m_player->markTracksMissing(paths);
        if (m_window.m_playlistDb != nullptr && m_window.m_playlistDb->markItemsMissing(paths) > 0 && m_window.m_playlistView != nullptr) {
            m_window.m_playlistView->reloadItems();
            m_window.m_playlistView->reloadPlaylists();
        }
        qCInfo(uiLog) << "marked" << marked << "tracks missing";
    }
}
