#include "ipc/IpcServer.h"

#include "ipc/IpcSocket.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>

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

IpcServer::~IpcServer() = default;

void IpcServer::setHandler(Handler handler)
{
    m_handler = std::move(handler);
}

bool IpcServer::listen(QString path)
{
    QString lockPath;
    if (path.isEmpty()) {
        path = IpcSocket::serverPath();
        lockPath = IpcSocket::lockPath();
    } else {
        lockPath = path + QStringLiteral(".lock");
    }
    auto lock = std::make_unique<QLockFile>(lockPath);
    if (!lock->tryLock(0)) {
        m_lastError = QStringLiteral("another muzaiten instance already owns %1").arg(lockPath);
        return false;
    }

    // A leftover socket file from an unclean shutdown would make listen() fail
    // forever. Only unlink it after taking the per-state lock; removeServer()
    // can unlink a live Unix socket, so it must not race another starter.
    QLocalServer::removeServer(path);
    if (!m_server->listen(path)) {
        m_lastError = m_server->errorString();
        lock->unlock();
        return false;
    }
    m_lock = std::move(lock);
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
    if (!m_buffers.contains(socket)) {
        return;
    }
    m_buffers[socket] += socket->readAll();
    // A handler may pump the event loop (e.g. "raise" builds the window), during
    // which the client can disconnect — the disconnected slot then removes this
    // socket from m_buffers and deleteLater()s it. So never hold a reference into
    // m_buffers across a handler call, and re-validate the socket after each one.
    QPointer<QLocalSocket> guard(socket);
    qsizetype newline = -1;
    while (m_buffers.contains(socket) && (newline = m_buffers[socket].indexOf('\n')) >= 0) {
        const QByteArray line = m_buffers[socket].left(newline).trimmed();
        m_buffers[socket].remove(0, newline + 1);
        if (line.isEmpty()) {
            continue;
        }
        const QByteArray reply = replyFor(line);
        if (guard.isNull()) {
            return;  // the client went away while the handler ran
        }
        socket->write(reply);
    }
    if (!guard.isNull() && m_buffers.contains(socket) && m_buffers[socket].size() > maxRequestBytes) {
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
