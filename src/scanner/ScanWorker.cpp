#include "scanner/ScanWorker.h"

#include "scanner/LibraryScanner.h"
#include "scanner/TagReader.h"

#include <QDirIterator>
#include <QFileInfo>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(scanLog, "muzaiten.scan")

ScanWorker::ScanWorker(QString rootPath, int batchSize)
    : m_rootPath(std::move(rootPath))
    , m_batchSize(batchSize)
{
}

void ScanWorker::run()
{
    QVector<Track> batch;
    batch.reserve(m_batchSize);

    qint64 visitedFiles = 0;
    qint64 indexedTracks = 0;
    const QFileInfo root(m_rootPath);
    if (!root.isDir() || root.isSymLink()) {
        emit finished(visitedFiles, indexedTracks, false);
        return;
    }

    TagReader reader;
    QDirIterator iterator(root.absoluteFilePath(), QDir::Files | QDir::Readable | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext() && !m_cancelRequested) {
        const QString path = iterator.next();
        ++visitedFiles;

        const QFileInfo info(path);
        if (info.isSymLink() || !LibraryScanner::isSupportedAudioFile(path)) {
            continue;
        }

        qCDebug(scanLog) << "scan" << path;
        batch.push_back(reader.read(path));
        ++indexedTracks;

        if (batch.size() >= m_batchSize) {
            emit batchReady(batch);
            batch.clear();
            emit progress(visitedFiles, indexedTracks, path);
        } else if ((visitedFiles % 512) == 0) {
            emit progress(visitedFiles, indexedTracks, path);
        }
    }

    if (!batch.isEmpty()) {
        emit batchReady(batch);
    }
    emit progress(visitedFiles, indexedTracks, {});
    emit finished(visitedFiles, indexedTracks, m_cancelRequested);
}

void ScanWorker::cancel()
{
    m_cancelRequested = true;
}

