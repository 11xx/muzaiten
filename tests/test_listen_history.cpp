#include "scrobble/ListenHistoryStore.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

class TestListenHistory final : public QObject {
    Q_OBJECT

private slots:
    void recordAndQueryUnsent();
    void recordOnlyOwesEnabledServices();
    void recordWithNoServicesKeepsHistoryOnly();
    void duplicateTimestampCollapses();
    void markSentPerService();
    void clearPendingPreservesHistory();
    void markOwedQueuesOnlyUnsentRows();
    void invalidListensRejected();

private:
    static Track makeTrack(const QString &title = QStringLiteral("Song"),
                           const QString &artist = QStringLiteral("Artist"))
    {
        Track track;
        track.title = title;
        track.artistName = artist;
        track.albumTitle = QStringLiteral("Album");
        track.path = QStringLiteral("/music/a.flac");
        track.durationMs = 200000;
        track.trackNumber = 3;
        track.musicBrainz.recordingId = QStringLiteral("rec-mbid");
        return track;
    }
};

void TestListenHistory::recordAndQueryUnsent()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));
    QVERIFY(store.isOpen());

    QVERIFY(store.recordListen(makeTrack(), 1000, true, true) > 0);
    QVERIFY(store.recordListen(makeTrack(QStringLiteral("Other")), 2000, true, true) > 0);
    QCOMPARE(store.totalCount(), 2);
    QCOMPARE(store.unsentCount(ListenHistoryStore::LastFm), 2);
    QCOMPARE(store.unsentCount(ListenHistoryStore::ListenBrainz), 2);

    const auto listens = store.unsent(ListenHistoryStore::LastFm, 10);
    QCOMPARE(listens.size(), 2);
    // Oldest first, and the round-tripped snapshot keeps the track fields.
    QCOMPARE(listens.first().listenedAtSecs, 1000);
    QCOMPARE(listens.first().track.title, QStringLiteral("Song"));
    QCOMPARE(listens.first().track.artistName, QStringLiteral("Artist"));
    QCOMPARE(listens.first().track.albumTitle, QStringLiteral("Album"));
    QCOMPARE(listens.first().track.durationMs, 200000);
    QCOMPARE(listens.first().track.trackNumber, 3);
    QCOMPARE(listens.first().track.musicBrainz.recordingId, QStringLiteral("rec-mbid"));
}

void TestListenHistory::recordOnlyOwesEnabledServices()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));

    QVERIFY(store.recordListen(makeTrack(), 1000, true, false) > 0);
    QCOMPARE(store.totalCount(), 1);
    QCOMPARE(store.unsentCount(ListenHistoryStore::LastFm), 1);
    QCOMPARE(store.unsentCount(ListenHistoryStore::ListenBrainz), 0);

    const auto rows = store.historyRows(10);
    QCOMPARE(rows.size(), 1);
    QVERIFY(rows.first().owedLastFm);
    QVERIFY(!rows.first().owedListenBrainz);
}

void TestListenHistory::recordWithNoServicesKeepsHistoryOnly()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));

    QVERIFY(store.recordListen(makeTrack(), 1000, false, false) > 0);
    QCOMPARE(store.totalCount(), 1);
    QCOMPARE(store.unsentCount(ListenHistoryStore::LastFm), 0);
    QCOMPARE(store.unsentCount(ListenHistoryStore::ListenBrainz), 0);
}

void TestListenHistory::duplicateTimestampCollapses()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));

    QVERIFY(store.recordListen(makeTrack(), 1000, true, true) > 0);
    QCOMPARE(store.recordListen(makeTrack(), 1000, true, true), -1);
    QCOMPARE(store.totalCount(), 1);
}

void TestListenHistory::markSentPerService()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));

    const qint64 id = store.recordListen(makeTrack(), 1000, true, true);
    store.markSent(ListenHistoryStore::LastFm, {id});
    QCOMPARE(store.unsentCount(ListenHistoryStore::LastFm), 0);
    QCOMPARE(store.unsentCount(ListenHistoryStore::ListenBrainz), 1);
}

void TestListenHistory::clearPendingPreservesHistory()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));

    QVERIFY(store.recordListen(makeTrack(), 1000, true, false) > 0);
    QCOMPARE(store.clearPending(ListenHistoryStore::LastFm), 1);
    QCOMPARE(store.totalCount(), 1);
    QCOMPARE(store.unsentCount(ListenHistoryStore::LastFm), 0);
    const auto rows = store.historyRows(10);
    QCOMPARE(rows.size(), 1);
    QVERIFY(!rows.first().owedLastFm);
    QVERIFY(!rows.first().sentLastFm);
}

void TestListenHistory::markOwedQueuesOnlyUnsentRows()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));

    const qint64 unsentId = store.recordListen(makeTrack(), 1000, false, false);
    const qint64 sentId = store.recordListen(makeTrack(QStringLiteral("Other")), 2000, true, false);
    store.markSent(ListenHistoryStore::LastFm, {sentId});

    QCOMPARE(store.markOwed(ListenHistoryStore::LastFm, {unsentId, sentId}), 1);
    QCOMPARE(store.unsentCount(ListenHistoryStore::LastFm), 1);
}

void TestListenHistory::invalidListensRejected()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));

    QCOMPARE(store.recordListen(makeTrack(), 0, true, true), -1);
    Track untitled;
    QCOMPARE(store.recordListen(untitled, 1000, true, true), -1);
    // A title-less track still records under its filename (local history keeps
    // everything; per-service metadata rules apply only at upload time).
    Track fileOnly;
    fileOnly.filename = QStringLiteral("a.flac");
    QVERIFY(store.recordListen(fileOnly, 1000, true, true) > 0);
}

QTEST_GUILESS_MAIN(TestListenHistory)
#include "test_listen_history.moc"
