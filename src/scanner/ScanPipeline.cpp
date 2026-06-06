#include "scanner/ScanPipeline.h"

#include "core/MetadataBlob.h"
#include "scanner/DirectoryWalker.h"
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
#include <sys/stat.h>
#include <sys/sysmacros.h>

Q_LOGGING_CATEGORY(scanPipelineLog, "muzaiten.scan")

namespace {

struct ThreadCounts {
    int walkers;
    int tags;
};

bool isRotational(const QString &path)
{
    struct stat st {};
    if (stat(path.toUtf8().constData(), &st) != 0) {
        return false;
    }
    const QString devLink = QStringLiteral("/sys/dev/block/%1:%2")
                                .arg(major(st.st_dev))
                                .arg(minor(st.st_dev));
    QString devDir = QFileInfo(devLink).canonicalFilePath();
    for (int level = 0; level < 2 && !devDir.isEmpty(); ++level) {
        QFile flag(devDir + QStringLiteral("/queue/rotational"));
        if (flag.exists() && flag.open(QIODevice::ReadOnly)) {
            return flag.readAll().trimmed() == "1";
        }
        devDir = QFileInfo(devDir).path(); // partition -> parent disk
    }
    return false;
}

ThreadCounts resolveThreads(const QString &root, const ScanPipeline::Options &options)
{
    const int cores = std::max(2, QThread::idealThreadCount());
    int walkers = options.walkerThreads;
    int tags = options.tagThreads;
    if (walkers > 0 && tags > 0) {
        return {walkers, tags};
    }

    if (isRotational(root)) {
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

    const ThreadCounts threads = resolveThreads(m_rootPath, m_options);
    const int workerCount = std::max(1, threads.tags);
    const std::size_t highWater = static_cast<std::size_t>(std::max(m_options.batchSize * 4, 1024));

    std::deque<Track> results;
    std::mutex mutex;
    std::condition_variable notEmpty;
    std::condition_variable notFull;
    std::atomic<std::size_t> nextIndex = 0;
    std::atomic<int> activeWorkers = workerCount;
    bool producersDone = false;

    const TagReader reader;

    const auto workerFn = [&]() {
        if (m_options.lowPriority) {
            setpriority(PRIO_PROCESS, 0, 10);
        }
        for (;;) {
            if (m_cancel) {
                break;
            }
            const std::size_t i = nextIndex.fetch_add(1);
            if (i >= total) {
                break;
            }

            MetadataBlob::FullMetadata full;
            Track track = reader.read(QString::fromStdString(paths[i]), &full);
            if (!MetadataBlob::isEmpty(full)) {
                const MetadataBlob::Encoded encoded = MetadataBlob::encode(full);
                track.fullMetadataBlob = encoded.data;
                track.fullMetadataRawSize = encoded.rawSize;
            }

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
        workers.emplace_back(workerFn);
    }

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
