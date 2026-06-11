#include "playlist/PlaylistImportWorker.h"

#include "db/Database.h"

#include <QUuid>

PlaylistImportWorker::PlaylistImportWorker(QString dbPath, QObject *parent)
    : QObject(parent)
    , m_dbPath(std::move(dbPath))
{
}

PlaylistImportWorker::~PlaylistImportWorker()
{
    delete m_db;
}

bool PlaylistImportWorker::ensureIndex()
{
    if (m_indexBuilt) {
        return true;
    }
    if (m_db == nullptr) {
        const QString connName = QStringLiteral("playlist-import-%1")
                                     .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
        m_db = new Database(connName);
        if (!m_db->open(m_dbPath)) {
            const QString err = m_db->lastError();
            delete m_db;
            m_db = nullptr;
            emit error(err);
            return false;
        }
    }
    m_index.build(m_db->allTracksForSearch());
    m_indexBuilt = true;
    return true;
}

void PlaylistImportWorker::matchEntries(QVector<PlaylistImport::ImportEntry> entries)
{
    if (!ensureIndex()) {
        return;
    }
    QVector<PlaylistImportMatch> results;
    results.reserve(entries.size());
    const int total = static_cast<int>(entries.size());
    int done = 0;
    for (const PlaylistImport::ImportEntry &entry : entries) {
        PlaylistImportMatch match;
        match.entry = entry;
        match.outcome = PlaylistMatcher::match(m_index, entry);
        results.append(match);
        emit progress(++done, total);
    }
    emit finished(results);
}
