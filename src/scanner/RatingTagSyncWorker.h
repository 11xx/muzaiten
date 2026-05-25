#pragma once

#include "core/Track.h"
#include "fs/LinkRoot.h"

#include <QObject>
#include <QMetaType>
#include <QString>
#include <QVector>

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

struct RatingTagSyncSummary {
    int checked = 0;
    int written = 0;
    int tagWon = 0;
    int noWritablePath = 0;
    int failed = 0;
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
    bool m_cancel = false;
};
