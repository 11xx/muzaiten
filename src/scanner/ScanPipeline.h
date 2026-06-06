#pragma once

#include "core/Track.h"
#include "core/TrackFingerprint.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <string>
#include <vector>

// Coordinates a fast, multi-threaded, cancellable library scan:
//   enumerate (DirectoryWalker) -> diff against DB fingerprints (skip unchanged)
//   -> parallel tag read + metadata-archive encode -> batched DB writes.
// Lives on its own control thread; all DB writes happen on the receiver's
// thread via the batchReady signal (single connection to library.sqlite).
class ScanPipeline final : public QObject {
    Q_OBJECT

public:
    struct Options {
        int walkerThreads = 0;   // 0 => auto from disk type
        int tagThreads = 0;      // 0 => auto from disk type
        int batchSize = 256;
        bool lowPriority = true;
        bool forceFullRescan = false;  // ignore fingerprint skip, re-read everything
    };

    // fingerprints is path -> {mtime,size,metadataScanned} of known tracks under
    // root; placeholders (metadataScanned == false) are always re-read.
    ScanPipeline(QString rootPath, int scanRootId,
                 QHash<QString, TrackFingerprint> fingerprints,
                 Options options, QObject *parent = nullptr);

    int scanRootId() const { return m_scanRootId; }

public slots:
    void run();
    void cancel();

signals:
    void batchReady(QVector<Track> tracks);
    void progress(qint64 enumerated, qint64 toProcess, qint64 processed, QString phase);
    void missingReady(QStringList paths);
    void finished(qint64 enumerated, qint64 indexed, qint64 skipped, bool canceled);

private:
    void runScan();
    qint64 processPaths(const std::vector<std::string> &paths, qint64 enumerated, const QString &phase);

    QString m_rootPath;
    int m_scanRootId = 0;
    QHash<QString, TrackFingerprint> m_fingerprints;
    Options m_options;
    std::atomic_bool m_cancel = false;
};
