#pragma once

// Background matcher for playlist imports. Lives on its own QThread (same
// pattern as Search::SearchWorker): builds the search index once from the
// library DB, then resolves a whole batch of ImportEntry rows through
// PlaylistMatcher, emitting progress along the way.

#include "playlist/PlaylistImport.h"
#include "playlist/PlaylistMatcher.h"
#include "search/SearchIndex.h"

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QVector>

class Database;

// One resolved import row: the source entry plus the matcher's verdict.
struct PlaylistImportMatch {
    PlaylistImport::ImportEntry entry;
    PlaylistMatcher::Outcome outcome;
};

class PlaylistImportWorker : public QObject {
    Q_OBJECT
public:
    explicit PlaylistImportWorker(QString dbPath, QObject *parent = nullptr);
    ~PlaylistImportWorker() override;

public slots:
    // Builds the index on first use (kept for the worker's lifetime), then
    // matches every entry. Emits progress(done, total) then finished(results).
    void matchEntries(QVector<PlaylistImport::ImportEntry> entries);

signals:
    void progress(int done, int total);
    void finished(QVector<PlaylistImportMatch> results);
    void error(QString message);

private:
    bool ensureIndex();

    QString m_dbPath;
    Search::SearchIndex m_index;
    bool m_indexBuilt = false;
    Database *m_db = nullptr;  // opened on the worker thread
};

Q_DECLARE_METATYPE(PlaylistImportMatch)
Q_DECLARE_METATYPE(QVector<PlaylistImportMatch>)
