#include "ipc/IpcServer.h"

#include "ipc/IpcSocket.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocalServer>
#include <QLocalSocket>

namespace {

// A line is one request; cap the buffer so a misbehaving client cannot grow
// memory without ever sending a newline.
constexpr qsizetype maxRequestBytes = 64 * 1024;

QByteArray encodeReply(QJsonObject payload)
{
    const QString error = payload.take(QStringLiteral("error")).toString();
    payload.insert(QStringLiteral("ok"), error.isEmpty());
    if (!error.isEmpty()) {
        payload.insert(QStringLiteral("error"), error);
    }
    return QJsonDocument(payload).toJson(QJsonDocument::Compact) + '\n';
}

QJsonObject errorPayload(const QString &message)
{
    return {{QStringLiteral("error"), message}};
}

} // namespace

IpcServer::IpcServer(QObject *parent)
    : QObject(parent)
    , m_server(new QLocalServer(this))
{
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    connect(m_server, &QLocalServer::newConnection, this, &IpcServer::onNewConnection);
}

void IpcServer::setHandler(Handler handler)
{
    m_handler = std::move(handler);
}

bool IpcServer::listen(QString path)
{
    if (path.isEmpty()) {
        path = IpcSocket::serverPath();
    }
    // A leftover socket file from an unclean shutdown would make listen() fail
    // forever; QLocalServer's helper removes it only if nothing is bound.
    QLocalServer::removeServer(path);
    if (!m_server->listen(path)) {
        m_lastError = m_server->errorString();
        return false;
    }
    m_path = path;
    return true;
}

QString IpcServer::serverPath() const
{
    return m_path;
}

QString IpcServer::lastError() const
{
    return m_lastError;
}

void IpcServer::onNewConnection()
{
    while (QLocalSocket *socket = m_server->nextPendingConnection()) {
        m_buffers.insert(socket, {});
        connect(socket, &QLocalSocket::readyRead, this, [this, socket]() { onReadyRead(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket]() {
            m_buffers.remove(socket);
            socket->deleteLater();
        });
    }
}

void IpcServer::onReadyRead(QLocalSocket *socket)
{
    QByteArray &buffer = m_buffers[socket];
    buffer += socket->readAll();
    qsizetype newline = -1;
    while ((newline = buffer.indexOf('\n')) >= 0) {
        const QByteArray line = buffer.left(newline).trimmed();
        buffer.remove(0, newline + 1);
        if (!line.isEmpty()) {
            socket->write(replyFor(line));
        }
    }
    if (buffer.size() > maxRequestBytes) {
        socket->write(encodeReply(errorPayload(QStringLiteral("request too large"))));
        socket->disconnectFromServer();
    }
}

QByteArray IpcServer::replyFor(const QByteArray &line)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        return encodeReply(errorPayload(QStringLiteral("invalid request: expected a JSON object per line")));
    }
    const QJsonObject request = doc.object();
    const QString command = request.value(QStringLiteral("command")).toString();
    if (command.isEmpty()) {
        return encodeReply(errorPayload(QStringLiteral("missing \"command\"")));
    }
    if (!m_handler) {
        return encodeReply(errorPayload(QStringLiteral("server not ready")));
    }
    return encodeReply(m_handler(command, request.value(QStringLiteral("args")).toObject()));
}
