#pragma once

#include "core/Track.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>

class ScanWorker final : public QObject {
    Q_OBJECT

public:
    explicit ScanWorker(QString rootPath, int batchSize = 128);

public slots:
    void run();
    void cancel();

signals:
    void batchReady(QVector<Track> tracks);
    void progress(qint64 visitedFiles, qint64 indexedTracks, QString currentPath);
    void finished(qint64 visitedFiles, qint64 indexedTracks, bool canceled);

private:
    QString m_rootPath;
    int m_batchSize = 128;
    std::atomic_bool m_cancelRequested = false;
};
