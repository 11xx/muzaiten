#include "scanner/ScanPipeline.h"

#include "core/MetadataBlob.h"
#include "fs/MediaProbe.h"
#include "scanner/DirectoryWalker.h"
#include "scanner/PathMetadataGuesser.h"
#include "scanner/TagReader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QThread>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include <sys/resource.h>

Q_LOGGING_CATEGORY(scanPipelineLog, "muzaiten.scan")

namespace {

struct ThreadCounts {
    int walkers;
    int tags;
};

ThreadCounts resolveThreads(const QString &root, const ScanPipeline::Options &options)
{
    const int cores = std::max(2, QThread::idealThreadCount());
    int walkers = options.walkerThreads;
    int tags = options.tagThreads;
    if (walkers > 0 && tags > 0) {
        return {walkers, tags};
    }

    // Network mounts start as conservatively as spinning disks: neither
    // rewards concurrent seeks, and the runtime latency controller raises
    // the worker count again when the medium turns out to be fast.
    if (MediaProbe::seekSensitive(MediaProbe::classify(root))) {
        if (walkers <= 0) {
            walkers = 2;
        }
        if (tags <= 0) {
            tags = 2;
        }
    } else {
        if (walkers <= 0) {
            walkers = std::clamp(cores / 2, 2, 8);
        }
        if (tags <= 0) {
            tags = std::clamp(cores, 2, 12);
        }
    }
    return {walkers, tags};
}

} // namespace

ScanPipeline::ScanPipeline(QString rootPath, int scanRootId,
                           QHash<QString, TrackFingerprint> fingerprints,
                           Options options, QObject *parent)
    : QObject(parent)
    , m_rootPath(std::move(rootPath))
    , m_scanRootId(scanRootId)
    , m_fingerprints(std::move(fingerprints))
    , m_options(options)
{
}

ScanPipeline::ScanPipeline(QString rootHint, QStringList fillPaths,
                           Options options, QObject *parent)
    : QObject(parent)
    , m_mode(Mode::Fill)
    , m_rootPath(std::move(rootHint))
    , m_fillPaths(std::move(fillPaths))
    , m_options(options)
{
}

void ScanPipeline::run()
{
    if (m_mode == Mode::Fill) {
        runFill();
    } else {
        runScan();
    }
}

void ScanPipeline::cancel()
{
    m_cancel = true;
}

void ScanPipeline::runScan()
{
    const QFileInfo root(m_rootPath);
    if (!root.isDir() || root.isSymLink()) {
        emit finished(0, 0, 0, false);
        return;
    }

    emit progress(0, 0, 0, QStringLiteral("enumerating"));

    const ThreadCounts threads = resolveThreads(m_rootPath, m_options);
    DirectoryWalker walker({threads.walkers, m_options.lowPriority, &m_cancel});
    const std::vector<DirectoryWalker::Found> found = walker.enumerate(root.absoluteFilePath().toStdString());
    const qint64 enumerated = static_cast<qint64>(found.size());

    if (m_cancel) {
        emit finished(enumerated, 0, 0, true);
        return;
    }

    QSet<QString> seenPaths;
    seenPaths.reserve(static_cast<int>(found.size()));
    std::vector<std::string> toRead;
    toRead.reserve(found.size());
    qint64 skipped = 0;
    // Partition the enumeration: new files become directory-view placeholders now
    // and defer their tag read to the background fill ("lazy"); changed files are
    // re-read in the foreground (the fast incremental rescan); unchanged scanned
    // files are skipped; existing placeholders are left for the fill. Deferring new
    // files is what keeps a first scan fast — the foreground only does enumeration
    // plus a handful of changed reads.
    QVector<Track> placeholders;
    for (const DirectoryWalker::Found &item : found) {
        const QString path = QString::fromStdString(item.path);
        seenPaths.insert(path);
        const auto it = m_fingerprints.constFind(path);
        if (it == m_fingerprints.constEnd()) {
            const QFileInfo info(path);
            Track placeholder;
            placeholder.path = path;
            placeholder.parentDir = info.absolutePath();
            placeholder.filename = info.fileName();
            placeholder.title = info.completeBaseName();
            placeholder.fileSize = item.size;
            placeholder.fileMtime = item.mtime;
            if (m_options.guessPlaceholders) {
                // Best-effort path parse so the row can show in the artist/album
                // browse before its real tags are read (see PathMetadataGuesser).
                const GuessedMetadata guessed = PathMetadataGuesser::guess(path, m_rootPath);
                if (!guessed.title.isEmpty()) {
                    placeholder.title = guessed.title;
                }
                placeholder.artistName = guessed.artist;
                placeholder.albumArtistName = guessed.albumArtist;
                placeholder.albumTitle = guessed.album;
                placeholder.trackNumber = guessed.trackNumber;
            }
            placeholders.push_back(std::move(placeholder));
            continue;
        }
        if (!it->metadataScanned) {
            // Existing placeholder: already visible; the background fill reads it.
            continue;
        }
        if (m_options.forceFullRescan || it->mtime != item.mtime || it->size != item.size) {
            toRead.push_back(item.path);
        } else {
            ++skipped;
        }
    }
    if (!m_cancel && !placeholders.isEmpty()) {
        emit enumeratedReady(placeholders);
    }

    emit progress(enumerated, static_cast<qint64>(toRead.size()), 0, QStringLiteral("reading"));
    const qint64 indexed = processPaths(toRead, enumerated, QStringLiteral("reading"));

    if (!m_cancel) {
        QStringList missing;
        for (auto it = m_fingerprints.constBegin(); it != m_fingerprints.constEnd(); ++it) {
            if (!seenPaths.contains(it.key())) {
                missing.append(it.key());
            }
        }
        if (!missing.isEmpty()) {
            emit missingReady(missing);
        }
    }

    emit finished(enumerated, indexed, skipped, m_cancel);
}

void ScanPipeline::runFill()
{
    std::vector<std::string> paths;
    paths.reserve(static_cast<std::size_t>(m_fillPaths.size()));
    for (qsizetype i = 0; i < m_fillPaths.size(); ++i) {
        paths.push_back(m_fillPaths.at(i).toStdString());
    }
    const qint64 total = static_cast<qint64>(paths.size());
    emit progress(total, total, 0, QStringLiteral("filling"));
    const qint64 indexed = m_cancel ? 0 : processPaths(paths, total, QStringLiteral("filling"));
    emit finished(total, indexed, 0, m_cancel);
}

qint64 ScanPipeline::processPaths(const std::vector<std::string> &paths, qint64 enumerated, const QString &phase)
{
    const std::size_t total = paths.size();
    if (total == 0) {
        return 0;
    }

    // Profile-driven floor/ceiling for the adaptive burst engine. We over-provision
    // `ceiling` worker threads but gate how many actually consume work via
    // activeTarget: the controller raises it toward ceiling while reads are fast and
    // lowers it toward floor when per-file latency climbs (a slow/HDD/network mount
    // saturating). Starting at ceiling = burst by default; nice(10) keeps it gentle
    // so it never starves the rest of the system.
    const ThreadCounts threads = resolveThreads(m_rootPath, m_options);
    const int cores = std::max(2, QThread::idealThreadCount());
    int floorWorkers = 1;
    int desiredCeiling = std::max(1, threads.tags);
    switch (m_options.profile) {
    case Profile::Background:
        floorWorkers = 1;
        desiredCeiling = 2;
        break;
    case Profile::Balanced:
        floorWorkers = 2;
        desiredCeiling = std::max(2, threads.tags);
        break;
    case Profile::Turbo:
        floorWorkers = std::max(2, threads.tags);
        desiredCeiling = std::clamp(cores * 2, 4, 16);
        break;
    }
    const int ceilingWorkers = static_cast<int>(
        std::min<std::size_t>(static_cast<std::size_t>(std::max(1, desiredCeiling)), total));
    floorWorkers = std::clamp(floorWorkers, 1, ceilingWorkers);
    const int workerCount = ceilingWorkers;
    const std::size_t highWater = static_cast<std::size_t>(std::max(m_options.batchSize * 4, 1024));

    std::deque<Track> results;
    std::mutex mutex;
    std::condition_variable notEmpty;
    std::condition_variable notFull;
    std::atomic<std::size_t> nextIndex = 0;
    std::atomic<int> activeWorkers = workerCount;
    bool producersDone = false;

    // Adaptive gate + latency signal (separate mutex from the results deque, so the
    // two never nest).
    std::mutex gateMutex;
    std::condition_variable gateCv;
    std::atomic<int> activeTarget = ceilingWorkers;  // start at burst
    std::atomic<bool> workExhausted = false;
    std::atomic<double> avgReadMs = 0.0;

    const auto recordLatency = [&](double ms) {
        double prev = avgReadMs.load(std::memory_order_relaxed);
        double next;
        do {
            next = prev <= 0.0 ? ms : (prev * 0.8 + ms * 0.2);
        } while (!avgReadMs.compare_exchange_weak(prev, next, std::memory_order_relaxed));
    };

    const TagReader reader;

    const auto workerFn = [&](int ordinal) {
        if (m_options.lowPriority) {
            setpriority(PRIO_PROCESS, 0, 10);
        }
        for (;;) {
            if (m_cancel) {
                break;
            }
            // Park while this worker is above the active target. The 100ms timeout
            // re-checks even if a notify is missed, and the predicate also wakes for
            // cancel / exhaustion so parked workers always exit (no join() hang).
            if (ordinal >= activeTarget.load()) {
                std::unique_lock<std::mutex> lock(gateMutex);
                gateCv.wait_for(lock, std::chrono::milliseconds(100), [&]() {
                    return ordinal < activeTarget.load() || m_cancel || workExhausted.load();
                });
                if (m_cancel || workExhausted.load()) {
                    break;
                }
                if (ordinal >= activeTarget.load()) {
                    continue;
                }
            }

            const std::size_t i = nextIndex.fetch_add(1);
            if (i >= total) {
                workExhausted.store(true);
                gateCv.notify_all();
                break;
            }

            const auto start = std::chrono::steady_clock::now();
            MetadataBlob::FullMetadata full;
            Track track = reader.read(QString::fromStdString(paths[i]), &full);
            if (!MetadataBlob::isEmpty(full)) {
                const MetadataBlob::Encoded encoded = MetadataBlob::encode(full);
                track.fullMetadataBlob = encoded.data;
                track.fullMetadataRawSize = encoded.rawSize;
            }
            recordLatency(std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start)
                              .count());

            std::unique_lock<std::mutex> lock(mutex);
            // wait_for, not wait: cancel() flips the atomic m_cancel from another
            // thread but cannot notify this (function-local) condition variable,
            // so re-check the predicate periodically to keep cancellation prompt
            // even when the consumer has stopped draining. The highWater cap is
            // still honored — we only proceed once results.size() < highWater or
            // we are cancelling.
            while (!notFull.wait_for(lock, std::chrono::milliseconds(100),
                                     [&]() { return results.size() < highWater || m_cancel; })) {
            }
            results.push_back(std::move(track));
            notEmpty.notify_one();
        }

        if (activeWorkers.fetch_sub(1) == 1) {
            std::lock_guard<std::mutex> lock(mutex);
            producersDone = true;
            notEmpty.notify_all();
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(workerCount));
    for (int i = 0; i < workerCount; ++i) {
        workers.emplace_back(workerFn, i);
    }

    // Controller: nudge activeTarget from the measured read-latency EWMA. Read cost
    // includes the metadata-archive encode, so it reflects real per-file work.
    constexpr double kRampUpBelowMs = 25.0;    // fast reads -> add a worker
    constexpr double kBackOffAboveMs = 120.0;  // slow reads (disk saturating) -> drop one
    const auto adjustActiveTarget = [&]() {
        const double avg = avgReadMs.load(std::memory_order_relaxed);
        if (avg <= 0.0) {
            return;
        }
        const int target = activeTarget.load();
        if (avg > kBackOffAboveMs && target > floorWorkers) {
            activeTarget.store(target - 1);
        } else if (avg < kRampUpBelowMs && target < ceilingWorkers) {
            activeTarget.store(target + 1);
            gateCv.notify_all();
        }
    };

    qint64 processed = 0;
    QVector<Track> batch;
    batch.reserve(m_options.batchSize);
    for (;;) {
        Track track;
        {
            std::unique_lock<std::mutex> lock(mutex);
            notEmpty.wait(lock, [&]() { return !results.empty() || producersDone; });
            if (results.empty() && producersDone) {
                break;
            }
            track = std::move(results.front());
            results.pop_front();
            notFull.notify_one();
        }

        batch.push_back(std::move(track));
        ++processed;
        if (batch.size() >= m_options.batchSize) {
            emit batchReady(batch);
            batch.clear();
            emit progress(enumerated, static_cast<qint64>(total), processed, phase);
            adjustActiveTarget();
        }
    }

    if (!batch.isEmpty()) {
        emit batchReady(batch);
    }
    emit progress(enumerated, static_cast<qint64>(total), processed, phase);

    for (std::thread &worker : workers) {
        worker.join();
    }
    return processed;
}
