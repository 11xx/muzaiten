#pragma once

#include <QObject>
#include <QStringList>
#include <QVector>

#include "core/ScanRoot.h"
#include "core/Track.h"

class MainWindow;

class ScanController final : public QObject {
public:
    explicit ScanController(MainWindow &window);
    void startScan(const QString &rootPath);
    void startScan(const QString &rootPath, int scanRootId);
    void scanEnabledSourceDirectories();
    void forceRescanEnabledSourceDirectories();
    void scanSourceRoots(const QVector<ScanRoot> &roots);
    void startNextQueuedSourceScan();
    void cancelScan();
    void ingestScanBatch(const QVector<Track> &tracks);
    void ingestEnumeratedPlaceholders(const QVector<Track> &tracks);
    void scheduleIncrementalRefresh();
    void flushIncrementalRefresh();
    int scanProfileSetting() const;
    int analysisPowerSetting() const;
    bool guessedPlaceholdersEnabled() const;
    void ensureIngestSession();
    void endIngestSessionIfIdle();
    QStringList nextFillChunk();
    void pumpMetadataFill();
    void startMetadataFill(const QStringList &paths);
    void finishMetadataFill(qint64, qint64, qint64, bool);
    void ensureDirectoryScanned(const QString &directory);
    void finishScan(qint64, qint64, qint64, bool);
    void markScannedTracksMissing(const QStringList &paths);
private:
    MainWindow &m_window;
};
