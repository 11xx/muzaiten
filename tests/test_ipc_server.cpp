#include "ipc/IpcServer.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QTest>

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

bool localSocketBindBlocked(const QString &path)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return errno == EPERM;
    }

    const QByteArray encoded = QFile::encodeName(path);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    if (encoded.size() >= static_cast<qsizetype>(sizeof(address.sun_path))) {
        ::close(fd);
        return false;
    }
    std::memcpy(address.sun_path, encoded.constData(), static_cast<size_t>(encoded.size() + 1));
    const bool blocked =
        ::bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 && errno == EPERM;
    ::close(fd);
    QFile::remove(path);
    return blocked;
}

} // namespace

// Exercises the IPC transport: framing, dispatch to the handler, the ok/error
// reply envelope, and malformed input. The MainWindow-side command semantics
// are not under test here — the handler is a stub.
class IpcServerTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();
    void repliesToHandlerResult();
    void wrapsHandlerErrors();
    void rejectsMalformedRequests();
    void handlesMultipleRequestsPerConnection();
    void secondServerCannotStealLiveSocket();

private:
    QJsonObject roundTrip(const QByteArray &line);

    QTemporaryDir m_dir;
    IpcServer *m_server = nullptr;
    QString m_path;
};

void IpcServerTest::init()
{
    m_server = new IpcServer(this);
    m_path = m_dir.filePath(QStringLiteral("ipc.sock"));
    m_server->setHandler([](const QString &command, const QJsonObject &args) -> QJsonObject {
        if (command == QLatin1String("echo")) {
            return {{QStringLiteral("echoed"), args}};
        }
        return {{QStringLiteral("error"), QStringLiteral("unknown command \"%1\"").arg(command)}};
    });
    if (!m_server->listen(m_path)) {
        if (localSocketBindBlocked(m_dir.filePath(QStringLiteral("probe.sock")))) {
            QSKIP("local socket binding is blocked by this sandbox");
        }
        QVERIFY2(false, qPrintable(m_server->lastError()));
    }
}

void IpcServerTest::cleanup()
{
    delete m_server;
    m_server = nullptr;
}

QJsonObject IpcServerTest::roundTrip(const QByteArray &line)
{
    // Server and client share this thread, so pump the event loop (qWaitFor)
    // instead of using the blocking waitFor* calls, which would starve the
    // server side.
    QLocalSocket socket;
    socket.connectToServer(m_path);
    if (!QTest::qWaitFor([&] { return socket.state() == QLocalSocket::ConnectedState; }, 2000)) {
        return {};
    }
    socket.write(line + '\n');
    QByteArray reply;
    if (!QTest::qWaitFor([&] {
            reply += socket.readAll();
            return reply.contains('\n');
        }, 2000)) {
        return {};
    }
    return QJsonDocument::fromJson(reply.left(reply.indexOf('\n'))).object();
}

void IpcServerTest::repliesToHandlerResult()
{
    const QJsonObject reply = roundTrip(R"({"command":"echo","args":{"value":42}})");
    QVERIFY(reply.value("ok").toBool());
    QCOMPARE(reply.value("echoed").toObject().value("value").toInt(), 42);
}

void IpcServerTest::wrapsHandlerErrors()
{
    const QJsonObject reply = roundTrip(R"({"command":"nope"})");
    QVERIFY(!reply.value("ok").toBool());
    QCOMPARE(reply.value("error").toString(), QStringLiteral("unknown command \"nope\""));
}

void IpcServerTest::rejectsMalformedRequests()
{
    QVERIFY(!roundTrip("this is not json").value("ok").toBool());
    QVERIFY(!roundTrip("[1,2,3]").value("ok").toBool());
    QVERIFY(!roundTrip(R"({"args":{}})").value("ok").toBool());
}

void IpcServerTest::handlesMultipleRequestsPerConnection()
{
    QLocalSocket socket;
    socket.connectToServer(m_path);
    QVERIFY(QTest::qWaitFor([&] { return socket.state() == QLocalSocket::ConnectedState; }, 2000));
    // Two requests in one write: framing must split them into two replies.
    socket.write(R"({"command":"echo","args":{"n":1}})" "\n" R"({"command":"echo","args":{"n":2}})" "\n");
    QByteArray data;
    QVERIFY(QTest::qWaitFor([&] {
        data += socket.readAll();
        return data.count('\n') >= 2;
    }, 2000));
    const QList<QByteArray> lines = data.split('\n');
    QCOMPARE(QJsonDocument::fromJson(lines[0]).object().value("echoed").toObject().value("n").toInt(), 1);
    QCOMPARE(QJsonDocument::fromJson(lines[1]).object().value("echoed").toObject().value("n").toInt(), 2);
}

void IpcServerTest::secondServerCannotStealLiveSocket()
{
    IpcServer second;
    QVERIFY(!second.listen(m_path));
    QVERIFY(!second.lastError().isEmpty());

    const QJsonObject reply = roundTrip(R"({"command":"echo","args":{"still":true}})");
    QVERIFY(reply.value("ok").toBool());
    QCOMPARE(reply.value("echoed").toObject().value("still").toBool(), true);
}

QTEST_GUILESS_MAIN(IpcServerTest)
#include "test_ipc_server.moc"
