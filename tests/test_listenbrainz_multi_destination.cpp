#include "scrobble/ListenBrainzHub.h"
#include "scrobble/ListenBrainzUrl.h"
#include "scrobble/ListenHistoryStore.h"
#include "scrobble/ScrobbleDestination.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

namespace {

// A minimal stand-in for a ListenBrainz-compatible server. It answers whatever
// status it is told to, which is the whole point: the tests below are about how
// one destination's answer affects the others, not about protocol detail.
class FakeServer final : public QTcpServer {
public:
    explicit FakeServer(int status, QObject *parent = nullptr)
        : QTcpServer(parent)
        , m_status(status)
    {
    }

    QString apiRoot() const
    {
        return ListenBrainzUrl::normalizeBase(QStringLiteral("http://127.0.0.1:%1").arg(serverPort())).apiRoot;
    }

    int submitCount() const { return m_submitCount; }
    QList<QByteArray> submittedBodies() const { return m_bodies; }
    void setStatus(int status) { m_status = status; }

protected:
    void incomingConnection(qintptr handle) override
    {
        auto *socket = new QTcpSocket(this);
        socket->setSocketDescriptor(handle);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            m_buffers[socket] += socket->readAll();
            const QByteArray &buffer = m_buffers[socket];
            const int headerEnd = buffer.indexOf("\r\n\r\n");
            if (headerEnd < 0) {
                return;
            }
            const QByteArray head = buffer.left(headerEnd);
            const int lengthAt = head.indexOf("Content-Length:");
            const int declared = lengthAt < 0
                ? 0
                : head.mid(lengthAt + 15, head.indexOf('\r', lengthAt) - lengthAt - 15).trimmed().toInt();
            if (buffer.size() < headerEnd + 4 + declared) {
                return;
            }

            if (head.startsWith("POST")) {
                ++m_submitCount;
                m_bodies.append(buffer.mid(headerEnd + 4, declared));
            }
            const QByteArray body = m_status == 200 ? QByteArray(R"({"status":"ok","valid":true})")
                                                    : QByteArray(R"({"code":401,"error":"Invalid token"})");
            socket->write("HTTP/1.1 " + QByteArray::number(m_status) + " X\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                          "Connection: close\r\n\r\n" + body);
            socket->flush();
            socket->disconnectFromHost();
            m_buffers.remove(socket);
            socket->deleteLater();
        });
    }

private:
    int m_status = 200;
    int m_submitCount = 0;
    QList<QByteArray> m_bodies;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

Track makeTrack(const QString &title)
{
    Track track;
    track.title = title;
    track.artistName = QStringLiteral("Artist");
    track.albumTitle = QStringLiteral("Album");
    track.path = QStringLiteral("/music/%1.flac").arg(title);
    track.durationMs = 200000;
    return track;
}

}   // namespace

class ListenBrainzMultiDestinationTest final : public QObject {
    Q_OBJECT

private slots:
    void aRejectedTokenDisablesOnlyItsOwnDestination();
    void anUnreachableDestinationDoesNotStallTheOthers();
    void validationReportsPerDestination();
    void anUnansweredTestIsNotARejection();
    void repeatedlyUnreachableDoesNotDisableADestination();
    void enablingMidTrackAnnouncesWhatIsPlaying();
    void adoptingTheSameTrackTwiceAnnouncesItOnce();
};

void ListenBrainzMultiDestinationTest::aRejectedTokenDisablesOnlyItsOwnDestination()
{
    QTemporaryDir dir;
    const QString historyPath = dir.filePath(QStringLiteral("history.sqlite"));

    FakeServer healthy(200);
    FakeServer rejecting(401);
    QVERIFY(healthy.listen(QHostAddress::LocalHost));
    QVERIFY(rejecting.listen(QHostAddress::LocalHost));

    ScrobbleDestinationSet destinations;
    const QString goodId = destinations.addCustom(QStringLiteral("Healthy"), healthy.apiRoot(), true);
    const QString badId = destinations.addCustom(QStringLiteral("Rejecting"), rejecting.apiRoot(), true);

    {
        ListenHistoryStore store(historyPath);
        QVERIFY(store.isOpen());
        store.recordListen(makeTrack(QStringLiteral("One")), 1000, {goodId, badId});
        store.recordListen(makeTrack(QStringLiteral("Two")), 2000, {goodId, badId});
        QCOMPARE(store.pendingCount(goodId), 2);
        QCOMPARE(store.pendingCount(badId), 2);
    }

    ListenBrainzHub hub;
    QSignalSpy disabled(&hub, &ListenBrainzHub::disabledAfterFailures);
    hub.configure(
        destinations, [](const QString &) { return QStringLiteral("token"); }, true, historyPath);

    // The rejecting destination turns itself off, and says which one it was.
    QTRY_VERIFY_WITH_TIMEOUT(disabled.count() >= 1, 10000);
    QCOMPARE(disabled.count(), 1);
    QCOMPARE(disabled.first().at(0).toString(), badId);

    ListenHistoryStore store(historyPath);
    // The healthy destination delivered both listens regardless.
    QTRY_COMPARE_WITH_TIMEOUT(store.pendingCount(goodId), 0, 10000);
    QCOMPARE(store.sentCount(goodId), 2);
    // The rejected one kept its backlog: a bad token loses no history.
    QCOMPARE(store.pendingCount(badId), 2);
    QCOMPARE(store.sentCount(badId), 0);
}

void ListenBrainzMultiDestinationTest::anUnreachableDestinationDoesNotStallTheOthers()
{
    QTemporaryDir dir;
    const QString historyPath = dir.filePath(QStringLiteral("history.sqlite"));

    FakeServer healthy(200);
    QVERIFY(healthy.listen(QHostAddress::LocalHost));

    // A port nothing is listening on: the request fails at connect time rather
    // than returning a status, which is the ordinary "server is down" case.
    FakeServer closed(200);
    QVERIFY(closed.listen(QHostAddress::LocalHost));
    const QString closedRoot = closed.apiRoot();
    closed.close();

    ScrobbleDestinationSet destinations;
    const QString goodId = destinations.addCustom(QStringLiteral("Healthy"), healthy.apiRoot(), true);
    const QString downId = destinations.addCustom(QStringLiteral("Down"), closedRoot, true);

    {
        ListenHistoryStore store(historyPath);
        store.recordListen(makeTrack(QStringLiteral("One")), 1000, {goodId, downId});
        store.recordListen(makeTrack(QStringLiteral("Two")), 2000, {goodId, downId});
    }

    ListenBrainzHub hub;
    QSignalSpy failed(&hub, &ListenBrainzHub::submissionFailed);
    hub.configure(
        destinations, [](const QString &) { return QStringLiteral("token"); }, true, historyPath);

    ListenHistoryStore store(historyPath);
    QTRY_COMPARE_WITH_TIMEOUT(store.pendingCount(goodId), 0, 10000);
    QCOMPARE(store.sentCount(goodId), 2);

    // The unreachable one reported its own failure and kept its backlog to
    // retry, without ever holding up the healthy destination.
    QTRY_VERIFY_WITH_TIMEOUT(failed.count() >= 1, 10000);
    QCOMPARE(failed.first().at(0).toString(), downId);
    QCOMPARE(store.pendingCount(downId), 2);
}

// Three minutes of an unreachable server used to turn a destination off for
// good, and a destination that is off is not owed the listens that follow. An
// outage must cost delivery time, never history.
void ListenBrainzMultiDestinationTest::repeatedlyUnreachableDoesNotDisableADestination()
{
    QTemporaryDir dir;
    const QString historyPath = dir.filePath(QStringLiteral("history.sqlite"));

    FakeServer closed(200);
    QVERIFY(closed.listen(QHostAddress::LocalHost));
    const QString closedRoot = closed.apiRoot();
    closed.close();

    ScrobbleDestinationSet destinations;
    const QString downId = destinations.addCustom(QStringLiteral("Down"), closedRoot, true);
    {
        ListenHistoryStore store(historyPath);
        store.recordListen(makeTrack(QStringLiteral("One")), 1000, {downId});
    }

    ListenBrainzHub hub;
    QSignalSpy failed(&hub, &ListenBrainzHub::submissionFailed);
    QSignalSpy disabled(&hub, &ListenBrainzHub::disabledAfterFailures);
    hub.configure(
        destinations, [](const QString &) { return QStringLiteral("token"); }, true, historyPath);

    // Well past the three answered failures that do disable a destination.
    for (int attempt = 0; attempt < 5; ++attempt) {
        QTRY_VERIFY_WITH_TIMEOUT(failed.count() >= attempt + 1, 10000);
        hub.uploadBacklog(downId);
    }

    QCOMPARE(disabled.count(), 0);
    ListenHistoryStore store(historyPath);
    QCOMPARE(store.pendingCount(downId), 1);
}

void ListenBrainzMultiDestinationTest::validationReportsPerDestination()
{
    QTemporaryDir dir;
    FakeServer accepting(200);
    FakeServer rejecting(401);
    QVERIFY(accepting.listen(QHostAddress::LocalHost));
    QVERIFY(rejecting.listen(QHostAddress::LocalHost));

    ListenBrainzHub hub;
    QSignalSpy validated(&hub, &ListenBrainzHub::tokenValidated);

    // Neither destination is configured: testing a server the user has typed but
    // not saved is the normal case when adding one.
    const QString acceptedId = QStringLiteral("c144daa3-617d-497d-82c4-f22d915aa354");
    const QString rejectedId = QStringLiteral("d31ed0b2-1dd3-49d4-b659-7849e06d4b07");
    hub.validateToken(acceptedId, 1, accepting.apiRoot(), QStringLiteral("token"));
    hub.validateToken(rejectedId, 2, rejecting.apiRoot(), QStringLiteral("token"));

    QTRY_COMPARE_WITH_TIMEOUT(validated.count(), 2, 10000);

    // Each answer carries back the request it belongs to, alongside its verdict.
    QHash<QString, ScrobbleTestResult::Outcome> results;
    QHash<QString, quint64> requests;
    for (const QList<QVariant> &call : validated) {
        results.insert(call.at(0).toString(), call.at(2).value<ScrobbleTestResult>().outcome);
        requests.insert(call.at(0).toString(), call.at(1).toULongLong());
    }
    QCOMPARE(results.value(acceptedId), ScrobbleTestResult::Outcome::Accepted);
    QCOMPARE(results.value(rejectedId), ScrobbleTestResult::Outcome::Rejected);
    QCOMPARE(requests.value(acceptedId), 1u);
    QCOMPARE(requests.value(rejectedId), 2u);
}

// A server that never answers is not a server that refused the token: the two
// call for different fixes, and only one of them is the user's credentials.
void ListenBrainzMultiDestinationTest::anUnansweredTestIsNotARejection()
{
    FakeServer down(200);
    QVERIFY(down.listen(QHostAddress::LocalHost));
    const QString downRoot = down.apiRoot();
    down.close();

    ListenBrainzHub hub;
    QSignalSpy validated(&hub, &ListenBrainzHub::tokenValidated);
    hub.validateToken(QStringLiteral("c144daa3-617d-497d-82c4-f22d915aa354"), 1, downRoot, QStringLiteral("token"));

    QTRY_COMPARE_WITH_TIMEOUT(validated.count(), 1, 10000);
    const ScrobbleTestResult result = validated.first().at(2).value<ScrobbleTestResult>();
    QCOMPARE(result.outcome, ScrobbleTestResult::Outcome::Unanswered);
    QVERIFY(!result.error.isEmpty());
}

void ListenBrainzMultiDestinationTest::enablingMidTrackAnnouncesWhatIsPlaying()
{
    QTemporaryDir dir;
    const QString historyPath = dir.filePath(QStringLiteral("history.sqlite"));

    FakeServer server(200);
    QVERIFY(server.listen(QHostAddress::LocalHost));

    ScrobbleDestinationSet destinations;
    const QString id = destinations.addCustom(QStringLiteral("Server"), server.apiRoot(), false);

    ListenBrainzHub hub;
    const auto tokenFor = [](const QString &) { return QStringLiteral("token"); };
    hub.configure(destinations, tokenFor, true, historyPath);

    // A track starts while the destination is off: nothing is sent.
    hub.trackStarted(makeTrack(QStringLiteral("One")));
    QTest::qWait(200);
    QCOMPARE(server.submitCount(), 0);

    // Turning it on mid-track announces what is already playing instead of
    // waiting for the next track.
    QVERIFY(destinations.setEnabled(id, true));
    hub.configure(destinations, tokenFor, true, historyPath);
    hub.adoptCurrentTrack(makeTrack(QStringLiteral("One")), true);

    QTRY_COMPARE_WITH_TIMEOUT(server.submitCount(), 1, 10000);
    const QJsonObject body = QJsonDocument::fromJson(server.submittedBodies().first()).object();
    QCOMPARE(body.value(QStringLiteral("listen_type")).toString(), QStringLiteral("playing_now"));
    const QJsonObject metadata = body.value(QStringLiteral("payload")).toArray().first().toObject()
                                     .value(QStringLiteral("track_metadata")).toObject();
    QCOMPARE(metadata.value(QStringLiteral("track_name")).toString(), QStringLiteral("One"));
}

void ListenBrainzMultiDestinationTest::adoptingTheSameTrackTwiceAnnouncesItOnce()
{
    QTemporaryDir dir;
    const QString historyPath = dir.filePath(QStringLiteral("history.sqlite"));

    FakeServer server(200);
    QVERIFY(server.listen(QHostAddress::LocalHost));

    ScrobbleDestinationSet destinations;
    destinations.addCustom(QStringLiteral("Server"), server.apiRoot(), true);

    ListenBrainzHub hub;
    const auto tokenFor = [](const QString &) { return QStringLiteral("token"); };
    hub.configure(destinations, tokenFor, true, historyPath);
    hub.adoptCurrentTrack(makeTrack(QStringLiteral("One")), true);
    QTRY_COMPARE_WITH_TIMEOUT(server.submitCount(), 1, 10000);

    // Saving settings again re-applies the same configuration; a destination
    // that has already announced this track must not announce it once more.
    hub.configure(destinations, tokenFor, true, historyPath);
    hub.adoptCurrentTrack(makeTrack(QStringLiteral("One")), true);
    QTest::qWait(500);
    QCOMPARE(server.submitCount(), 1);
}

QTEST_MAIN(ListenBrainzMultiDestinationTest)
#include "test_listenbrainz_multi_destination.moc"
