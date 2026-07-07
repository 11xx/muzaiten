#pragma once

#include <QHash>
#include <QList>
#include <QPair>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QVector>

class FeatureStore final {
public:
    struct Scalars {
        double tempoBpm = -1.0;
        double loudness = 0.0;
        double energy = -1.0;
        double brightness = -1.0;
        bool valid = false;
    };

    struct Status {
        qint64 files = 0;
        qint64 ok = 0;
        qint64 failed = 0;
        qint64 groups = 0;
        qint64 featured = 0;
        QString dspVersion;
        qint64 embeddedGroups = 0;
        QString embeddingModel;
        QString embeddingVersion;
        qint64 neighborRows = 0;
    };

    explicit FeatureStore(const QString &path);
    ~FeatureStore();

    FeatureStore(const FeatureStore &) = delete;
    FeatureStore &operator=(const FeatureStore &) = delete;

    bool isOpen() const;
    int schemaVersion() const;
    qint64 contentGroupForPath(const QString &path) const;
    QHash<QString, qint64> contentGroupsForPaths(const QStringList &paths) const;
    QVector<qint64> contentGroupIds(int minSize = 1) const;
    QStringList pathsInGroup(qint64 groupId) const;
    Scalars scalarsForGroup(qint64 groupId) const;
    QHash<qint64, Scalars> scalarsForGroups(const QList<qint64> &groupIds) const;
    QVector<float> embeddingForGroup(qint64 groupId) const;
    QHash<qint64, QVector<float>> embeddingsForGroups(const QList<qint64> &groupIds) const;
    QList<QPair<qint64, double>> neighborsOfGroup(qint64 groupId, int limit) const;
    Status status() const;

private:
    void close();

    QString m_connectionName;
    QSqlDatabase m_db;
    int m_schemaVersion = -1;
    bool m_connectionAdded = false;
};
