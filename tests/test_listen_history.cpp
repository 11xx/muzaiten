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
    void recordAndQueryRatingEvents();
    void ratingEventsDoNotAffectAffinities();
    void schemaVersionIsCurrent();

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

void TestListenHistory::recordAndQueryRatingEvents()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));
    QVERIFY(store.isOpen());

    Track track = makeTrack();
    ListenHistoryStore::RatingEvent set;
    set.occurredAtSecs = 1000;
    set.track = track;
    set.oldEffectiveRating0To100 = 55;
    set.newRating0To100 = 80;
    set.sourceSurface = QStringLiteral("track_table");
    set.playingTrackPath = QStringLiteral("/music/playing.flac");
    set.playingSource = QStringLiteral("radio");
    set.radioActive = true;
    QVERIFY(store.recordRatingEvent(set));

    ListenHistoryStore::RatingEvent change;
    change.occurredAtSecs = 1001;
    change.track = track;
    change.hasOldUserRating = true;
    change.oldUserRating0To100 = 80;
    change.oldEffectiveRating0To100 = 80;
    change.newRating0To100 = 40;
    change.sourceSurface = QStringLiteral("player_bar");
    QVERIFY(store.recordRatingEvent(change));

    ListenHistoryStore::RatingEvent clear;
    clear.occurredAtSecs = 1002;
    clear.track = track;
    clear.hasOldUserRating = true;
    clear.oldUserRating0To100 = 40;
    clear.oldEffectiveRating0To100 = 40;
    clear.newRating0To100 = -1;
    clear.sourceSurface = QStringLiteral("ipc");
    clear.playingTrackPath = QStringLiteral("/music/playing.flac");
    clear.playingSource = QStringLiteral("queue_manual");
    QVERIFY(store.recordRatingEvent(clear));

    const QVector<ListenHistoryStore::RatingEvent> events = store.ratingEvents();
    QCOMPARE(events.size(), 3);
    QCOMPARE(events.at(0).occurredAtSecs, 1000);
    QCOMPARE(events.at(0).track.path, track.path);
    QCOMPARE(events.at(0).track.title, track.title);
    QCOMPARE(events.at(0).track.musicBrainz.recordingId, QStringLiteral("rec-mbid"));
    QVERIFY(!events.at(0).hasOldUserRating);
    QCOMPARE(events.at(0).oldUserRating0To100, -1);
    QCOMPARE(events.at(0).oldEffectiveRating0To100, 55);
    QCOMPARE(events.at(0).newRating0To100, 80);
    QCOMPARE(events.at(0).sourceSurface, QStringLiteral("track_table"));
    QCOMPARE(events.at(0).playingTrackPath, QStringLiteral("/music/playing.flac"));
    QCOMPARE(events.at(0).playingSource, QStringLiteral("radio"));
    QVERIFY(events.at(0).radioActive);

    QVERIFY(events.at(1).hasOldUserRating);
    QCOMPARE(events.at(1).oldUserRating0To100, 80);
    QCOMPARE(events.at(1).oldEffectiveRating0To100, 80);
    QCOMPARE(events.at(1).newRating0To100, 40);
    QCOMPARE(events.at(1).sourceSurface, QStringLiteral("player_bar"));
    QVERIFY(events.at(1).playingTrackPath.isEmpty());
    QVERIFY(events.at(1).playingSource.isEmpty());
    QVERIFY(!events.at(1).radioActive);

    QCOMPARE(events.at(2).oldUserRating0To100, 40);
    QCOMPARE(events.at(2).oldEffectiveRating0To100, 40);
    QCOMPARE(events.at(2).newRating0To100, -1);
    QCOMPARE(events.at(2).sourceSurface, QStringLiteral("ipc"));

    const QVector<ListenHistoryStore::RatingEvent> limited = store.ratingEvents(2);
    QCOMPARE(limited.size(), 2);
    QCOMPARE(limited.at(0).newRating0To100, 80);
    QCOMPARE(limited.at(1).newRating0To100, 40);
}

void TestListenHistory::ratingEventsDoNotAffectAffinities()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));
    QVERIFY(store.isOpen());

    Track track = makeTrack();
    ListenHistoryStore::PlayEvent play;
    play.startedAtSecs = 1000;
    play.endedAtSecs = 1200;
    play.playedMs = 200000;
    play.durationMs = 200000;
    play.completion = 1.0;
    play.outcome = QStringLiteral("finished");
    play.source = QStringLiteral("queue_auto");
    play.sessionId = QStringLiteral("session-1");
    play.track = track;
    QVERIFY(store.recordPlayEvent(play) > 0);
    QVERIFY(store.recordListen(track, 1000, false, false) > 0);

    const auto before = store.trackAffinities();
    QVERIFY(before.contains(track.path));

    ListenHistoryStore::RatingEvent sameTrack;
    sameTrack.occurredAtSecs = 1300;
    sameTrack.track = track;
    sameTrack.newRating0To100 = 100;
    sameTrack.sourceSurface = QStringLiteral("playlist");
    QVERIFY(store.recordRatingEvent(sameTrack));

    Track other = makeTrack(QStringLiteral("Other"), QStringLiteral("Other Artist"));
    other.path = QStringLiteral("/music/other.flac");
    ListenHistoryStore::RatingEvent otherTrack;
    otherTrack.occurredAtSecs = 1301;
    otherTrack.track = other;
    otherTrack.oldEffectiveRating0To100 = 20;
    otherTrack.newRating0To100 = 60;
    otherTrack.sourceSurface = QStringLiteral("music_explorer");
    QVERIFY(store.recordRatingEvent(otherTrack));

    const auto after = store.trackAffinities();
    QCOMPARE(after.size(), before.size());
    QVERIFY(after.contains(track.path));
    QCOMPARE(after.value(track.path).playEvents, before.value(track.path).playEvents);
    QCOMPARE(after.value(track.path).finished, before.value(track.path).finished);
    QCOMPARE(after.value(track.path).skipped, before.value(track.path).skipped);
    QCOMPARE(after.value(track.path).lastPlayedAtSecs, before.value(track.path).lastPlayedAtSecs);
    QCOMPARE(after.value(track.path).listenCount, before.value(track.path).listenCount);
    QCOMPARE(after.value(track.path).baselineMax, before.value(track.path).baselineMax);
    QVERIFY(!after.contains(other.path));
}

void TestListenHistory::schemaVersionIsCurrent()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));
    QVERIFY(store.isOpen());

    QCOMPARE(store.metaValue(QStringLiteral("schemaVersion")), QStringLiteral("5"));
}

QTEST_GUILESS_MAIN(TestListenHistory)
#include "test_listen_history.moc"
