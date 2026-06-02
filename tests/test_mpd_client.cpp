#include "mpd/MpdClient.h"

#include <QElapsedTimer>
#include <QList>
#include <QTest>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace {

// Binds a one-shot loopback server on an ephemeral port; returns the listening
// fd and reports the bound port. Caller closes the fd.
int makeLoopbackServer(quint16 *port)
{
    const int serverFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        return -1;
    }
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(serverFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0
        || ::listen(serverFd, 1) != 0) {
        ::close(serverFd);
        return -1;
    }
    socklen_t addressLength = sizeof(address);
    ::getsockname(serverFd, reinterpret_cast<sockaddr *>(&address), &addressLength);
    *port = ntohs(address.sin_port);
    return serverFd;
}

// Accepts one client, sends the MPD greeting, drains its command line, then
// writes |chunks| in order with |gapMs| between them (to force separate client
// reads when exercising chunk-boundary behavior).
void serveChunks(int serverFd, const QList<QByteArray> &chunks, int gapMs)
{
    const int clientFd = ::accept(serverFd, nullptr, nullptr);
    if (clientFd < 0) {
        return;
    }
    const char *greeting = "OK MPD 0.23.0\n";
    ::send(clientFd, greeting, std::strlen(greeting), 0);

    QByteArray received;
    char buffer[256] {};
    while (!received.contains('\n')) {
        const ssize_t bytes = ::recv(clientFd, buffer, sizeof(buffer), 0);
        if (bytes <= 0) {
            break;
        }
        received.append(buffer, static_cast<qsizetype>(bytes));
    }
    for (qsizetype i = 0; i < chunks.size(); ++i) {
        ::send(clientFd, chunks[i].constData(), static_cast<size_t>(chunks[i].size()), 0);
        if (gapMs > 0 && i + 1 < chunks.size()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(gapMs));
        }
    }
    ::close(clientFd);
}

} // namespace

class MpdClientTest final : public QObject {
    Q_OBJECT

private slots:
    void allowsOnlyReadOnlyCommands();
    void readsListAllInfo();
    void reportsAckErrorWithoutLeadingNewline();
    void decodesUtf8SplitAcrossReads();
};

void MpdClientTest::allowsOnlyReadOnlyCommands()
{
    QVERIFY(MpdClient::isReadOnlyCommand(QStringLiteral("listallinfo")));
    QVERIFY(MpdClient::isReadOnlyCommand(QStringLiteral("find")));
    QVERIFY(!MpdClient::isReadOnlyCommand(QStringLiteral("play")));
    QVERIFY(!MpdClient::isReadOnlyCommand(QStringLiteral("clear")));
}

void MpdClientTest::readsListAllInfo()
{
    const int serverFd = ::socket(AF_INET, SOCK_STREAM, 0);
    QVERIFY(serverFd >= 0);

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    QVERIFY(::bind(serverFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    QVERIFY(::listen(serverFd, 1) == 0);

    socklen_t addressLength = sizeof(address);
    QVERIFY(::getsockname(serverFd, reinterpret_cast<sockaddr *>(&address), &addressLength) == 0);
    const quint16 port = ntohs(address.sin_port);

    QString received;
    std::atomic_bool served = false;
    std::thread serverThread([serverFd, &received, &served]() {
        const int clientFd = ::accept(serverFd, nullptr, nullptr);
        if (clientFd < 0) {
            return;
        }
        const char *greeting = "OK MPD 0.23.0\n";
        ::send(clientFd, greeting, std::strlen(greeting), 0);

        char buffer[256] {};
        while (!received.contains(QStringLiteral("listallinfo\n"))) {
            const ssize_t bytes = ::recv(clientFd, buffer, sizeof(buffer), 0);
            if (bytes <= 0) {
                break;
            }
            received += QString::fromUtf8(buffer, static_cast<qsizetype>(bytes));
        }
        const char *response = "file: Artist/Album/01.flac\n"
                               "Title: Track One\n"
                               "Artist: Artist\n"
                               "AlbumArtist: Album Artist\n"
                               "Album: Album\n"
                               "Track: 1/2\n"
                               "Disc: 1\n"
                               "duration: 123.5\n"
                               "MUSICBRAINZ_TRACKID: recording-id\n"
                               "OK\n";
        ::send(clientFd, response, std::strlen(response), 0);
        ::close(clientFd);
        served = true;
    });

    MpdClient client;
    QString error;
    const bool connected = client.connectToServer(QStringLiteral("127.0.0.1"), port, 3000, &error);
    QVector<MpdTrack> tracks;
    if (connected) {
        tracks = client.listAllInfo(&error);
    }
    serverThread.join();
    ::close(serverFd);

    QVERIFY2(connected, qPrintable(error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(tracks.size(), 1);
    QCOMPARE(tracks.first().uri, QStringLiteral("Artist/Album/01.flac"));
    QCOMPARE(tracks.first().title, QStringLiteral("Track One"));
    QCOMPARE(tracks.first().albumArtistName, QStringLiteral("Album Artist"));
    QCOMPARE(tracks.first().trackNumber, 1);
    QCOMPARE(tracks.first().durationMs, 123500);
    QCOMPARE(tracks.first().musicBrainz.recordingId, QStringLiteral("recording-id"));
    QCOMPARE(client.sentCommands(), QStringList{QStringLiteral("listallinfo")});
    QVERIFY(served);
}

void MpdClientTest::reportsAckErrorWithoutLeadingNewline()
{
    quint16 port = 0;
    const int serverFd = makeLoopbackServer(&port);
    QVERIFY(serverFd >= 0);

    std::thread serverThread([serverFd]() {
        // A single ACK line with no preceding newline — the error case that
        // previously matched neither terminator and blocked for 5 seconds.
        serveChunks(serverFd, {QByteArray("ACK [50@0] {listallinfo} unknown command\n")}, 0);
    });

    MpdClient client;
    QString error;
    const bool connected = client.connectToServer(QStringLiteral("127.0.0.1"), port, 3000, &error);
    QElapsedTimer timer;
    timer.start();
    QVector<MpdTrack> tracks;
    if (connected) {
        tracks = client.listAllInfo(&error);
    }
    const qint64 elapsed = timer.elapsed();
    serverThread.join();
    ::close(serverFd);

    QVERIFY2(connected, qPrintable(error));
    QVERIFY(tracks.isEmpty());
    QVERIFY2(!error.isEmpty(), "a leading ACK error must be surfaced");
    QVERIFY2(error.contains(QStringLiteral("ACK")), qPrintable(error));
    QVERIFY2(elapsed < 2000, "leading ACK must not block on the 5s read timeout");
}

void MpdClientTest::decodesUtf8SplitAcrossReads()
{
    quint16 port = 0;
    const int serverFd = makeLoopbackServer(&port);
    QVERIFY(serverFd >= 0);

    // 'é' is the two bytes C3 A9; split the response between them so a per-chunk
    // decode would corrupt the character into U+FFFD.
    QByteArray response =
        "file: A/B/01.flac\n"
        "Title: Caf\xC3\xA9 Test\n"
        "Artist: Artist\n"
        "Album: Album\n"
        "OK\n";
    const qsizetype marker = response.indexOf(static_cast<char>(0xC3));
    QVERIFY(marker > 0);
    const QByteArray first = response.left(marker + 1);
    const QByteArray rest = response.mid(marker + 1);

    std::thread serverThread([serverFd, first, rest]() {
        serveChunks(serverFd, {first, rest}, 100);
    });

    MpdClient client;
    QString error;
    const bool connected = client.connectToServer(QStringLiteral("127.0.0.1"), port, 3000, &error);
    QVector<MpdTrack> tracks;
    if (connected) {
        tracks = client.listAllInfo(&error);
    }
    serverThread.join();
    ::close(serverFd);

    QVERIFY2(connected, qPrintable(error));
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(tracks.size(), 1);
    QCOMPARE(tracks.first().title, QString::fromUtf8("Caf\xC3\xA9 Test"));
}

QTEST_MAIN(MpdClientTest)
#include "test_mpd_client.moc"
