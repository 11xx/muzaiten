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
    // Burst posture for the adaptive tag-reader. Background stays gentle; Balanced
    // ramps to the disk-appropriate thread count; Turbo over-provisions for SSDs.
    // All three back off automatically when per-file read latency climbs.
    enum class Profile { Background, Balanced, Turbo };

    struct Options {
        int walkerThreads = 0;   // 0 => auto from disk type
        int tagThreads = 0;      // 0 => auto from disk type
        int batchSize = 256;
        bool lowPriority = true;
        bool forceFullRescan = false;  // ignore fingerprint skip, re-read everything
        Profile profile = Profile::Balanced;
    };

    // fingerprints is path -> {mtime,size,metadataScanned} of known tracks under
    // root; placeholders (metadataScanned == false) are always re-read.
    ScanPipeline(QString rootPath, int scanRootId,
                 QHash<QString, TrackFingerprint> fingerprints,
                 Options options, QObject *parent = nullptr);

    // Fill-only mode: read tags for an explicit set of paths (no enumeration, no
    // missing detection). Used to lazily fill placeholder rows in the background.
    // rootHint is used only for disk-type detection / thread sizing.
    ScanPipeline(QString rootHint, QStringList fillPaths,
                 Options options, QObject *parent = nullptr);

    int scanRootId() const { return m_scanRootId; }

public slots:
    void run();
    void cancel();

signals:
    // Path-derived rows for brand-new files, emitted after enumeration and before
    // the tag read, so the directory/file view can show them immediately.
    void enumeratedReady(QVector<Track> placeholders);
    void batchReady(QVector<Track> tracks);
    void progress(qint64 enumerated, qint64 toProcess, qint64 processed, QString phase);
    void missingReady(QStringList paths);
    void finished(qint64 enumerated, qint64 indexed, qint64 skipped, bool canceled);

private:
    enum class Mode { Scan, Fill };

    void runScan();
    void runFill();
    qint64 processPaths(const std::vector<std::string> &paths, qint64 enumerated, const QString &phase);

    Mode m_mode = Mode::Scan;
    QString m_rootPath;
    int m_scanRootId = 0;
    QHash<QString, TrackFingerprint> m_fingerprints;
    QStringList m_fillPaths;
    Options m_options;
    std::atomic_bool m_cancel = false;
};
