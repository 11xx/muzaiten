#pragma once

// Background matcher for drag-and-dropped playlist files. Unlike the modal's
// PlaylistImportWorker (one batch → one preview), this fills several already-created
// placeholder playlists at once and streams each resolved item back so the view can
// fill live. It is interruptible: requestStop() ends matching after the current
// entry, leaving whatever was matched so far in place.

#include "playlist/PlaylistImport.h"
#include "playlist/PlaylistImportWorker.h"  // PlaylistImportMatch
#include "search/SearchIndex.h"

#include <QAtomicInt>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

class Database;

// One placeholder playlist plus the entries to resolve into it.
struct DropImportJob {
    qint64 playlistId = 0;
    QVector<PlaylistImport::ImportEntry> entries;
};

class PlaylistDropImportWorker : public QObject {
    Q_OBJECT
public:
    explicit PlaylistDropImportWorker(QString dbPath, QObject *parent = nullptr);
    ~PlaylistDropImportWorker() override;

    // Thread-safe: asks the run loop to stop after the current entry.
    void requestStop() { m_stop.storeRelaxed(1); }

public slots:
    // Builds the (exclude-filtered) index once, then resolves every job's entries
    // in order, emitting itemMatched per entry and playlistFinished per job.
    void run(QVector<DropImportJob> jobs);

signals:
    void indexBuilding();
    void itemMatched(qint64 playlistId, PlaylistImportMatch match);
    void playlistFinished(qint64 playlistId);
    void finished(bool interrupted);
    void error(QString message);

private:
    bool ensureIndex();

    QString m_dbPath;
    Search::SearchIndex m_index;
    bool m_indexBuilt = false;
    Database *m_db = nullptr;  // opened on the worker thread
    QAtomicInt m_stop{0};
};

Q_DECLARE_METATYPE(DropImportJob)
Q_DECLARE_METATYPE(QVector<DropImportJob>)
