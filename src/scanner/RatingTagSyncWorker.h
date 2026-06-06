#pragma once

#include "core/Track.h"
#include "fs/LinkRoot.h"

#include <QObject>
#include <QMetaType>
#include <QString>
#include <QVector>

#include <atomic>

struct RatingTagSyncRequest {
    enum class Scope {
        Track,
        CurrentArtist,
        SavedRatedTracks,
        PendingWrites
    };

    Scope scope = Scope::Track;
    QVector<Track> tracks;
    QVector<LinkRoot> linkRoots;
};

// One per successfully-written file: lets the UI patch the affected rows in
// place instead of re-querying the library after a sync.
struct RatingTagSyncUpdate {
    QString path;
    int effectiveRating0To100 = -1;
};

struct RatingTagSyncSummary {
    int checked = 0;
    int written = 0;
    int noWritablePath = 0;
    int failed = 0;
    QVector<RatingTagSyncUpdate> updates;
};

Q_DECLARE_METATYPE(RatingTagSyncSummary)

class RatingTagSyncWorker final : public QObject {
    Q_OBJECT

public:
    explicit RatingTagSyncWorker(QString databasePath, RatingTagSyncRequest request);

public slots:
    void run();
    void cancel();

signals:
    void progress(int checked, int total, const QString &path);
    void finished(RatingTagSyncSummary summary, QString error);

private:
    QString m_databasePath;
    RatingTagSyncRequest m_request;
    // Written by cancel() (potentially from another thread) and read by run();
    // atomic to avoid a data race, matching ScanPipeline::m_cancel.
    std::atomic_bool m_cancel = false;
};
