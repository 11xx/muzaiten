#pragma once

#include "features/FeatureStore.h"

#include <QVector>
#include <QString>
#include <QStringList>

class Database;

namespace AudioAnalysisData {

struct StatusSummary {
    QString path;
    bool found = false;
    bool open = false;
    int schemaVersion = -1;
    FeatureStore::Status status;
    QString message;
};

struct DuplicateCopy {
    qint64 groupId = -1;
    QString path;
    QString title;
    QString artist;
    QString album;
    QString codec;
    int bitDepth = 0;
    int sampleRateHz = 0;
    int bitrateKbps = 0;
    QStringList mediaTags;
    int qualityScore = 0;
    bool pinned = false;
    bool best = false;
    QString qualitySummary;
};

struct DuplicateGroup {
    qint64 groupId = -1;
    QString pinnedPath;
    QString bestPath;
    QVector<DuplicateCopy> copies;
};

StatusSummary loadStatus(const QString &featuresPath);
QVector<DuplicateGroup> loadDuplicateGroups(Database &db, const FeatureStore &features,
                                            int minSize = 2, int limit = 200);
QString copyDisplayTitle(const DuplicateCopy &copy);

} // namespace AudioAnalysisData
