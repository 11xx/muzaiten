#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>

class QLockFile;
class QLocalServer;
class QLocalSocket;

// Newline-delimited JSON command socket (the muzaitenctl endpoint).
//
// Pure transport: it owns the QLocalServer, framing, and parse errors, and
// hands every well-formed request to the installed handler. The handler
// returns the reply payload; a payload carrying an "error" key is sent as
// {"ok": false, "error": ...}, anything else as {"ok": true, ...payload}.
// Requests look like {"command": "status", "args": {...}} — one per line.
class IpcServer final : public QObject {
    Q_OBJECT

public:
    using Handler = std::function<QJsonObject(const QString &command, const QJsonObject &args)>;

    explicit IpcServer(QObject *parent = nullptr);
    ~IpcServer() override;

    void setHandler(Handler handler);

    // Listens at the given path (default: IpcSocket::serverPath()), taking an
    // atomic per-state lock before removing any stale socket file. False +
    // lastError() on failure; never fatal to the app.
    bool listen(QString path = {});
    QString serverPath() const;
    QString lastError() const;

private:
    void onNewConnection();
    void onReadyRead(QLocalSocket *socket);
    QByteArray replyFor(const QByteArray &line);

    QLocalServer *m_server = nullptr;
    Handler m_handler;
    QHash<QLocalSocket *, QByteArray> m_buffers;
    std::unique_ptr<QLockFile> m_lock;
    QString m_path;
    QString m_lastError;
};
