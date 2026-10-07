#pragma once
#include <QByteArray>
#include <QObject>
#include <QJsonObject>
#include <QVector>
class Database;
class PlayerCore;
class PlaylistDatabase;
class SettingsStore;
class QTimer;
struct Track;
class QueueSnapshotStore final : public QObject {
    Q_OBJECT
public:
    explicit QueueSnapshotStore(PlayerCore &player, Database &database, PlaylistDatabase &playlists,
                                SettingsStore &state, QObject *parent);
    QString queueId() const { return m_queueId; }
    QString sourceKind() const { return m_queueSourceKind; }
    qint64 sourcePlaylistId() const { return m_queueSourcePlaylistId; }
    QString sourceName() const { return m_queueSourceName; }
    void setQueueIdentity(const QString &id, const QString &kind = QStringLiteral("queue"),
                          qint64 playlistId = 0, const QString &name = {});
    void setPlaylistMirrorSuppressed(bool suppressed) { m_suppressPlaylistMirror = suppressed; }
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
signals:
    void sourceChanged();
    void snapshotsChanged();
    void playlistItemsChanged();
private:
    void syncQueueIdentity();
    void prepareQueueForTrackAddition(const QVector<Track> &tracks);
    void prepareQueueForLibraryInjection(const Track &track);
    QByteArray currentQueueStructureFingerprint() const;

    PlayerCore &m_player;
    Database &m_database;
    PlaylistDatabase &m_playlists;
    SettingsStore &m_state;
    QTimer *m_saveTimer;
    QString m_queueId;
    QString m_queueSourceKind = QStringLiteral("queue");
    qint64 m_queueSourcePlaylistId = 0;
    QString m_queueSourceName;
    bool m_restored = false;
    bool m_suppressPlaylistMirror = false;
    // Fingerprint of the queue structure this store last wrote to `queue.state`;
    // empty until the first structural save of the process, so that save always
    // writes the document.
    QByteArray m_savedStructureFingerprint;
};
