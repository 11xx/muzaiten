#pragma once

#include "mpd/MpdTrack.h"

#include <QString>
#include <QVector>

#include <atomic>

class QTcpSocket;

class MpdClient final {
public:
    MpdClient();
    ~MpdClient();

    static bool isReadOnlyCommand(const QString &command);

    // Optional cancellation flag (owned by the caller). When set true, the
    // blocking connect/read waits return promptly instead of running to their
    // full timeout. The pointed-to flag may live on another thread.
    void setCancelFlag(const std::atomic_bool *cancel) { m_cancelFlag = cancel; }

    bool connectToServer(const QString &host, quint16 port, int timeoutMs, QString *error);
    QVector<MpdTrack> listAllInfo(QString *error);
    QStringList sentCommands() const;

private:
    QStringList command(const QString &command, const QStringList &args, QString *error);
    QString readResponse(QString *error);
    bool isCancelled() const { return m_cancelFlag != nullptr && m_cancelFlag->load(); }
    bool waitForConnectedInterruptible(int timeoutMs);
    bool waitForReadableInterruptible(int timeoutMs);

    QTcpSocket *m_socket = nullptr;
    QStringList m_sentCommands;
    const std::atomic_bool *m_cancelFlag = nullptr;
};
