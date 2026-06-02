#include "mpd/MpdImportWorker.h"

#include "db/Database.h"
#include "mpd/MpdClient.h"

#include <QUuid>

#include <algorithm>
#include <limits>

MpdImportWorker::MpdImportWorker(QString databasePath,
                                 QString configPath,
                                 QString musicDirectory,
                                 QString host,
                                 quint16 port,
                                 int timeoutMs)
    : m_databasePath(std::move(databasePath))
    , m_configPath(std::move(configPath))
    , m_musicDirectory(std::move(musicDirectory))
    , m_host(std::move(host))
    , m_port(port)
    , m_timeoutMs(timeoutMs)
{
}

void MpdImportWorker::run()
{
    QString error;
    MpdClient client;
    client.setCancelFlag(&m_cancel);

    if (!client.connectToServer(m_host, m_port, m_timeoutMs, &error)) {
        emit finished(0, m_cancel.load() ? QString() : error);
        return;
    }

    const QVector<MpdTrack> tracks = client.listAllInfo(&error);
    if (m_cancel.load()) {
        emit finished(0, {});
        return;
    }
    if (!error.isEmpty()) {
        emit finished(0, error);
        return;
    }

    Database database(QStringLiteral("mpd-import-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!database.open(m_databasePath)) {
        emit finished(0, database.lastError());
        return;
    }

    const qint64 sourceId = database.upsertMediaSource(QStringLiteral("mpd"),
                                                       QStringLiteral("local"),
                                                       m_musicDirectory,
                                                       m_configPath);
    if (sourceId <= 0) {
        emit finished(0, database.lastError());
        return;
    }
    if (!database.clearMpdTracksForSource(sourceId)) {
        emit finished(0, database.lastError());
        return;
    }
    if (!database.beginTransaction()) {
        emit finished(0, database.lastError());
        return;
    }

    const int totalTracks = static_cast<int>(std::min<qsizetype>(tracks.size(), std::numeric_limits<int>::max()));
    int imported = 0;
    for (const MpdTrack &track : tracks) {
        if (m_cancel.load()) {
            // Leave the transaction uncommitted; the local Database rolls it
            // back when it goes out of scope, so a cancelled import is discarded.
            emit finished(imported, {});
            return;
        }
        if (!database.upsertMpdTrack(sourceId, track)) {
            emit finished(imported, database.lastError());
            return;
        }
        ++imported;
        if (imported % 512 == 0) {
            emit progress(imported, totalTracks);
        }
    }

    if (!database.commitTransaction()) {
        emit finished(imported, database.lastError());
        return;
    }

    emit progress(imported, totalTracks);
    emit finished(imported, {});
}
