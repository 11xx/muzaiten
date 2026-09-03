#pragma once
#include <QByteArray>
#include <QObject>
#include <QJsonObject>
class MainWindow;
class QueueSnapshotStore final : public QObject {
public:
    explicit QueueSnapshotStore(MainWindow &window);
    void loadQueueState();
    void saveQueueState();
    void scheduleQueueStateSave(bool immediate = false);
    QJsonObject queueSnapshotObject(const QString &, const QString & = {}) const;
    QJsonObject loadQueueSnapshotsRoot() const;
    void saveQueueSnapshotsRoot(const QJsonObject &);
    QJsonObject queueSnapshotByKey(const QString &) const;
    int savedQueueLimitSetting() const;
    int radioSavedQueueLimitSetting() const;
    bool savedQueueUnlimitedSetting() const;
    bool radioSavedQueueUnlimitedSetting() const;
    void ensureCurrentQueueIdentity();
    bool currentQueueBacklogEligible() const;
    QJsonObject captureCurrentQueueSnapshot(const QString &source = {});
    void pushQueueSnapshotToBacklog(const QJsonObject &snapshot);
    void pushCurrentQueueToBacklog(const QString &, const QString & = {});
    void snapshotCurrentQueueAsPrevious(const QString & = {});
    void markQueueAsSpontaneous(const QString & = {});
private:
    QByteArray currentQueueStructureFingerprint() const;

    MainWindow &m_window;
    // Fingerprint of the queue structure this store last wrote to `queue.state`;
    // empty until the first structural save of the process, so that save always
    // writes the document.
    QByteArray m_savedStructureFingerprint;
};
