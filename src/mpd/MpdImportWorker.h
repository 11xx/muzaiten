#pragma once

#include <QObject>
#include <QString>

#include <atomic>

class MpdImportWorker final : public QObject {
    Q_OBJECT

public:
    MpdImportWorker(QString databasePath,
                    QString configPath,
                    QString musicDirectory,
                    QString host,
                    quint16 port,
                    int timeoutMs);

    // Request cancellation. Thread-safe (atomic); the blocking run() loop and
    // the MpdClient waits observe it and return promptly.
    void cancel() { m_cancel.store(true); }

public slots:
    void run();

signals:
    void progress(int importedTracks, int totalTracks);
    void finished(int importedTracks, QString error);

private:
    QString m_databasePath;
    QString m_configPath;
    QString m_musicDirectory;
    QString m_host;
    quint16 m_port = 6600;
    int m_timeoutMs = 5000;
    std::atomic_bool m_cancel{false};
};
