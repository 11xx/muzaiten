#pragma once
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
    void pushCurrentQueueToBacklog(const QString &, const QString & = {});
    void snapshotCurrentQueueAsPrevious(const QString & = {});
    void markQueueAsSpontaneous(const QString & = {});
private:
    MainWindow &m_window;
};
