#include "mpd/MpdClient.h"

#include <QTest>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <thread>

class MpdClientTest final : public QObject {
    Q_OBJECT

private slots:
    void allowsOnlyReadOnlyCommands();
    void readsListAllInfo();
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

QTEST_MAIN(MpdClientTest)
#include "test_mpd_client.moc"
