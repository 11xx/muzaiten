#pragma once

#include "mpd/MpdTrack.h"

#include <QString>
#include <QVector>

class QTcpSocket;

class MpdClient final {
public:
    MpdClient();
    ~MpdClient();

    static bool isReadOnlyCommand(const QString &command);

    bool connectToServer(const QString &host, quint16 port, int timeoutMs, QString *error);
    QVector<MpdTrack> listAllInfo(QString *error);
    QStringList sentCommands() const;

private:
    QStringList command(const QString &command, const QStringList &args, QString *error);
    QString readResponse(QString *error);

    QTcpSocket *m_socket = nullptr;
    QStringList m_sentCommands;
};
