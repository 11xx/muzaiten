#pragma once

#include "features/FeatureStore.h"

#include <QVector>
#include <QDateTime>
#include <QString>
#include <QStringList>

#include <optional>

class Database;

namespace AudioAnalysisData {

struct StatusSummary {
    QString path;
    bool found = false;
    bool open = false;
    int schemaVersion = -1;
    FeatureStore::Status status;
    QString message;
    struct LastRun {
        bool present = false;
        qint64 finishedAt = 0;
        double elapsedSecs = 0.0;
        int scanned = 0;
        int skipped = 0;
        int failed = 0;
        double meanMsPerTrack = 0.0;
        QString power;
    } lastRun;
};

struct LiveStatus {
    enum class Phase {
        Idle,
        AnalyzingFiles,
        Grouping,
        WritingFeatures,
        SemanticEmbeddings,
        SemanticNeighbors,
        ModelDownload,
    };

    bool running = false;
    Phase phase = Phase::Idle;
    int analyzed = 0;
    int total = 0;
    double rate = -1.0;
    std::optional<qint64> etaSecs;
    double elapsedSecs = 0.0;
    QString power;
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
QString compactDuration(qint64 seconds);
QString clockDuration(qint64 seconds);
QString spacedDuration(qint64 seconds);
QString progressLabel(const LiveStatus &status);
QString phaseLabel(LiveStatus::Phase phase);
QString finalSummary(int scanned, int skipped, int failed, int groups, double elapsedSecs,
                     int featuresWritten = -1);
QVector<DuplicateGroup> loadDuplicateGroups(Database &db, const FeatureStore &features,
                                            int minSize = 2, int limit = 200);
QString copyDisplayTitle(const DuplicateCopy &copy);

} // namespace AudioAnalysisData
