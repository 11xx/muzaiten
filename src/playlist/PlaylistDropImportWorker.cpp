#include "playlist/PlaylistDropImportWorker.h"

#include "db/Database.h"
#include "playlist/PlaylistMatchIndex.h"

#include <QUuid>

PlaylistDropImportWorker::PlaylistDropImportWorker(QString dbPath, QObject *parent)
    : QObject(parent)
    , m_dbPath(std::move(dbPath))
{
}

PlaylistDropImportWorker::~PlaylistDropImportWorker()
{
    delete m_db;
}

bool PlaylistDropImportWorker::ensureIndex()
{
    if (m_indexBuilt) {
        return true;
    }
    if (m_db == nullptr) {
        const QString connName = QStringLiteral("playlist-drop-import-%1")
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
    emit indexBuilding();
    m_index = PlaylistImport::buildMatchIndex(*m_db);
    m_indexBuilt = true;
    return true;
}

void PlaylistDropImportWorker::skipPlaylist(qint64 playlistId)
{
    QMutexLocker lock(&m_skipMutex);
    m_skip.insert(playlistId);
}

bool PlaylistDropImportWorker::isSkipped(qint64 playlistId)
{
    QMutexLocker lock(&m_skipMutex);
    return m_skip.contains(playlistId);
}

void PlaylistDropImportWorker::run(QVector<DropImportJob> jobs)
{
    if (!ensureIndex()) {
        emit finished(/*interrupted=*/false);
        return;
    }
    bool interrupted = false;
    for (const DropImportJob &job : jobs) {
        if (m_stop.loadRelaxed() != 0) {
            interrupted = true;
            break;
        }
        if (!isSkipped(job.playlistId)) {
            for (const PlaylistImport::ImportEntry &entry : job.entries) {
                if (m_stop.loadRelaxed() != 0) {
                    interrupted = true;
                    break;
                }
                // Per-playlist stop: drop the rest of this playlist's entries but
                // keep the global run going for the other jobs.
                if (isSkipped(job.playlistId)) {
                    break;
                }
                PlaylistImportMatch match;
                match.entry = entry;
                match.outcome = PlaylistMatcher::match(m_index, entry);
                emit itemMatched(job.playlistId, match);
            }
        }
        emit playlistFinished(job.playlistId);
        if (interrupted) {
            break;
        }
    }
    emit finished(interrupted);
}
