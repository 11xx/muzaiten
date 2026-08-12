#include "scrobble/ListenHistoryStore.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QVariant>
#include <QtTest>

#include <array>

class TestListenHistory final : public QObject {
    Q_OBJECT

private slots:
    void recordAndQueryUnsent();
    void recordOnlyOwesEnabledServices();
    void recordWithNoServicesKeepsHistoryOnly();
    void duplicateTimestampCollapses();
    void markSentPerService();
    void clearPendingPreservesHistory();
    void markOwedSkipsAlreadyOwedRows();
    void invalidListensRejected();
    void recordAndQueryRatingEvents();
    void ratingEventsDoNotAffectAffinities();
    void recordAndQueryQueueRemovals();
    void queueRemovalsDoNotAffectAffinities();
    void recordAndQueryRadioPicks();
    void radioPicksDoNotAffectAffinities();
    void schemaVersionIsCurrent();
    void destinationsDeliverIndependently();
    void backlogDrainsOldestFirstPerDestination();
    void enablingADestinationDoesNotClaimOldListens();
    void removingADestinationDropsOnlyItsRows();
    void historyRowsSummarizeDeliveryAcrossDestinations();
    void migratesLegacyFlagsOnce();
    void migrationLeavesLegacyColumnsInPlace();

private:
    // A history database as an earlier build left it: fixed owed_*/sent_*
    // columns, no delivery rows, and no migration marker.
    static void writeLegacyHistory(const QString &path)
    {
        const QString connection = QStringLiteral("legacy-history-fixture");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(path);
            QVERIFY(db.open());

            QSqlQuery create(db);
            QVERIFY(create.exec(QStringLiteral(
                "CREATE TABLE listens ("
                " id INTEGER PRIMARY KEY,"
                " listened_at INTEGER NOT NULL,"
                " title TEXT NOT NULL,"
                " artist TEXT NOT NULL,"
                " album TEXT,"
                " path TEXT,"
                " duration_ms INTEGER,"
                " track_json TEXT NOT NULL,"
                " owed_lastfm INTEGER NOT NULL DEFAULT 0,"
                " sent_lastfm INTEGER NOT NULL DEFAULT 0,"
                " owed_listenbrainz INTEGER NOT NULL DEFAULT 0,"
                " sent_listenbrainz INTEGER NOT NULL DEFAULT 0,"
                " UNIQUE(listened_at, artist, title))")));
            QVERIFY(create.exec(QStringLiteral("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)")));
            QVERIFY(create.exec(QStringLiteral("INSERT INTO meta(key, value) VALUES('schemaVersion', '7')")));

            // Owed to both, delivered to neither; delivered to Last.fm only;
            // owed to ListenBrainz only; owed to nobody.
            const QList<std::array<int, 5>> rows = {
                {1000, 1, 0, 1, 0},
                {2000, 1, 1, 0, 0},
                {3000, 0, 0, 1, 0},
                {4000, 0, 0, 0, 0},
            };
            for (const auto &row : rows) {
                QSqlQuery insert(db);
                insert.prepare(QStringLiteral(
                    "INSERT INTO listens(listened_at, title, artist, album, path, duration_ms, track_json,"
                    " owed_lastfm, sent_lastfm, owed_listenbrainz, sent_listenbrainz)"
                    " VALUES(?, ?, 'Artist', 'Album', '/music/a.flac', 200000, '{\"title\":\"Song\"}', ?, ?, ?, ?)"));
                insert.addBindValue(row[0]);
                insert.addBindValue(QStringLiteral("Song %1").arg(row[0]));
                insert.addBindValue(row[1]);
                insert.addBindValue(row[2]);
                insert.addBindValue(row[3]);
                insert.addBindValue(row[4]);
                QVERIFY(insert.exec());
            }
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }

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

    QVERIFY(store.recordListen(makeTrack(), 1000, {ListenHistoryStore::LastFm, ListenHistoryStore::ListenBrainz}) > 0);
    QVERIFY(store.recordListen(makeTrack(QStringLiteral("Other")), 2000, {ListenHistoryStore::LastFm, ListenHistoryStore::ListenBrainz}) > 0);
    QCOMPARE(store.totalCount(), 2);
    QCOMPARE(store.pendingCount(ListenHistoryStore::LastFm), 2);
    QCOMPARE(store.pendingCount(ListenHistoryStore::ListenBrainz), 2);

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

    QVERIFY(store.recordListen(makeTrack(), 1000, {ListenHistoryStore::LastFm}) > 0);
    QCOMPARE(store.totalCount(), 1);
    QCOMPARE(store.pendingCount(ListenHistoryStore::LastFm), 1);
    QCOMPARE(store.pendingCount(ListenHistoryStore::ListenBrainz), 0);

    const auto rows = store.historyRows(10);
    QCOMPARE(rows.size(), 1);
    QVERIFY(rows.first().owedTo(ListenHistoryStore::LastFm));
    QVERIFY(!rows.first().owedTo(ListenHistoryStore::ListenBrainz));
    QCOMPARE(rows.first().owedCount(), 1);
}

void TestListenHistory::recordWithNoServicesKeepsHistoryOnly()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));

    QVERIFY(store.recordListen(makeTrack(), 1000, {}) > 0);
    QCOMPARE(store.totalCount(), 1);
    QCOMPARE(store.pendingCount(ListenHistoryStore::LastFm), 0);
    QCOMPARE(store.pendingCount(ListenHistoryStore::ListenBrainz), 0);
}

void TestListenHistory::duplicateTimestampCollapses()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));

    QVERIFY(store.recordListen(makeTrack(), 1000, {ListenHistoryStore::LastFm, ListenHistoryStore::ListenBrainz}) > 0);
    QCOMPARE(store.recordListen(makeTrack(), 1000, {ListenHistoryStore::LastFm, ListenHistoryStore::ListenBrainz}), -1);
    QCOMPARE(store.totalCount(), 1);
}

void TestListenHistory::markSentPerService()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));

    const qint64 id = store.recordListen(makeTrack(), 1000, {ListenHistoryStore::LastFm, ListenHistoryStore::ListenBrainz});
    store.markSent(ListenHistoryStore::LastFm, {id});
    QCOMPARE(store.pendingCount(ListenHistoryStore::LastFm), 0);
    QCOMPARE(store.pendingCount(ListenHistoryStore::ListenBrainz), 1);
}

void TestListenHistory::clearPendingPreservesHistory()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));

    QVERIFY(store.recordListen(makeTrack(), 1000, {ListenHistoryStore::LastFm}) > 0);
    QCOMPARE(store.clearPending(ListenHistoryStore::LastFm), 1);
    QCOMPARE(store.totalCount(), 1);
    QCOMPARE(store.pendingCount(ListenHistoryStore::LastFm), 0);
    const auto rows = store.historyRows(10);
    QCOMPARE(rows.size(), 1);
    QVERIFY(!rows.first().owedTo(ListenHistoryStore::LastFm));
    QVERIFY(!rows.first().sentTo(ListenHistoryStore::LastFm));
}

void TestListenHistory::markOwedSkipsAlreadyOwedRows()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));

    const qint64 unsentId = store.recordListen(makeTrack(), 1000, {});
    const qint64 sentId = store.recordListen(makeTrack(QStringLiteral("Other")), 2000, {ListenHistoryStore::LastFm});
    store.markSent(ListenHistoryStore::LastFm, {sentId});

    QCOMPARE(store.markOwed(ListenHistoryStore::LastFm, {unsentId, sentId}), 1);
    QCOMPARE(store.pendingCount(ListenHistoryStore::LastFm), 1);
}

void TestListenHistory::invalidListensRejected()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));

    QCOMPARE(store.recordListen(makeTrack(), 0, {ListenHistoryStore::LastFm, ListenHistoryStore::ListenBrainz}), -1);
    Track untitled;
    QCOMPARE(store.recordListen(untitled, 1000, {ListenHistoryStore::LastFm, ListenHistoryStore::ListenBrainz}), -1);
    // A title-less track still records under its filename (local history keeps
    // everything; per-service metadata rules apply only at upload time).
    Track fileOnly;
    fileOnly.filename = QStringLiteral("a.flac");
    QVERIFY(store.recordListen(fileOnly, 1000, {ListenHistoryStore::LastFm, ListenHistoryStore::ListenBrainz}) > 0);
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
    QVERIFY(store.recordListen(track, 1000, {}) > 0);

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

void TestListenHistory::recordAndQueryQueueRemovals()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));
    QVERIFY(store.isOpen());

    Track radio = makeTrack();
    ListenHistoryStore::QueueRemovalEvent radioEvent;
    radioEvent.occurredAtSecs = 1000;
    radioEvent.track = radio;
    radioEvent.wasRadioPick = true;
    radioEvent.wasUnheard = true;
    radioEvent.radioActive = true;
    QVERIFY(store.recordQueueRemoval(radioEvent));

    Track manual = makeTrack(QStringLiteral("Manual"), QStringLiteral("Other Artist"));
    manual.path = QStringLiteral("/music/manual.flac");
    manual.musicBrainz.recordingId.clear();
    ListenHistoryStore::QueueRemovalEvent manualEvent;
    manualEvent.occurredAtSecs = 1001;
    manualEvent.track = manual;
    manualEvent.wasUnheard = false;
    QVERIFY(store.recordQueueRemoval(manualEvent));

    const QVector<ListenHistoryStore::QueueRemovalEvent> events = store.queueRemovalEvents();
    QCOMPARE(events.size(), 2);
    QCOMPARE(events.at(0).occurredAtSecs, 1000);
    QCOMPARE(events.at(0).track.path, radio.path);
    QCOMPARE(events.at(0).track.musicBrainz.recordingId, QStringLiteral("rec-mbid"));
    QVERIFY(events.at(0).wasRadioPick);
    QVERIFY(events.at(0).wasUnheard);
    QVERIFY(events.at(0).radioActive);

    QCOMPARE(events.at(1).occurredAtSecs, 1001);
    QCOMPARE(events.at(1).track.path, QStringLiteral("/music/manual.flac"));
    QVERIFY(events.at(1).track.musicBrainz.recordingId.isEmpty());
    QVERIFY(!events.at(1).wasRadioPick);
    QVERIFY(!events.at(1).wasUnheard);
    QVERIFY(!events.at(1).radioActive);

    const QVector<ListenHistoryStore::QueueRemovalEvent> limited = store.queueRemovalEvents(1);
    QCOMPARE(limited.size(), 1);
    QCOMPARE(limited.first().track.path, radio.path);
}

void TestListenHistory::queueRemovalsDoNotAffectAffinities()
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
    QVERIFY(store.recordListen(track, 1000, {}) > 0);

    const auto before = store.trackAffinities();
    QVERIFY(before.contains(track.path));

    ListenHistoryStore::QueueRemovalEvent sameTrack;
    sameTrack.occurredAtSecs = 1300;
    sameTrack.track = track;
    sameTrack.wasRadioPick = true;
    sameTrack.wasUnheard = true;
    sameTrack.radioActive = true;
    QVERIFY(store.recordQueueRemoval(sameTrack));

    Track other = makeTrack(QStringLiteral("Other"), QStringLiteral("Other Artist"));
    other.path = QStringLiteral("/music/other.flac");
    ListenHistoryStore::QueueRemovalEvent otherTrack;
    otherTrack.occurredAtSecs = 1301;
    otherTrack.track = other;
    otherTrack.wasUnheard = true;
    QVERIFY(store.recordQueueRemoval(otherTrack));

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

void TestListenHistory::recordAndQueryRadioPicks()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));
    QVERIFY(store.isOpen());

    ListenHistoryStore::RadioPickEvent pick;
    pick.occurredAtSecs = 1000;
    pick.track = makeTrack();
    pick.sessionKind = QStringLiteral("seeded");
    pick.exploration0To100 = 35;
    pick.weightsJson = R"({"genreWeight":2.5,"ratingWeight":0.5})";
    pick.components = {
        {QStringLiteral("genre"), 1.25},
        {QStringLiteral("rating"), 0.5},
        {QStringLiteral("same-artist"), -0.25},
    };
    pick.score = 1.5;
    QVERIFY(store.recordRadioPick(pick));

    ListenHistoryStore::RadioPickEvent anchorless;
    anchorless.occurredAtSecs = 1001;
    anchorless.track = makeTrack(QStringLiteral("Other"), QStringLiteral("Other Artist"));
    anchorless.track.path = QStringLiteral("/music/other.flac");
    anchorless.track.musicBrainz.recordingId.clear();
    anchorless.sessionKind = QStringLiteral("anchorless");
    anchorless.exploration0To100 = 60;
    anchorless.weightsJson = R"({"genreWeight":3.0})";
    anchorless.components = {{QStringLiteral("novelty"), 0.8}};
    anchorless.score = 0.8;
    QVERIFY(store.recordRadioPick(anchorless));

    const QVector<ListenHistoryStore::RadioPickEvent> events = store.radioPickEvents();
    QCOMPARE(events.size(), 2);
    QCOMPARE(events.at(0).occurredAtSecs, 1000);
    QCOMPARE(events.at(0).track.path, QStringLiteral("/music/a.flac"));
    QCOMPARE(events.at(0).track.musicBrainz.recordingId, QStringLiteral("rec-mbid"));
    QCOMPARE(events.at(0).sessionKind, QStringLiteral("seeded"));
    QCOMPARE(events.at(0).exploration0To100, 35);
    QCOMPARE(events.at(0).weightsJson, QByteArray(R"({"genreWeight":2.5,"ratingWeight":0.5})"));
    QCOMPARE(events.at(0).components.size(), 3);
    QCOMPARE(events.at(0).components.at(0).name, QStringLiteral("genre"));
    QVERIFY(qFuzzyCompare(events.at(0).components.at(0).value, 1.25));
    QCOMPARE(events.at(0).components.at(2).name, QStringLiteral("same-artist"));
    QVERIFY(qFuzzyCompare(events.at(0).components.at(2).value, -0.25));
    QVERIFY(qFuzzyCompare(events.at(0).score, 1.5));

    QCOMPARE(events.at(1).track.path, QStringLiteral("/music/other.flac"));
    QVERIFY(events.at(1).track.musicBrainz.recordingId.isEmpty());
    QCOMPARE(events.at(1).sessionKind, QStringLiteral("anchorless"));

    const QVector<ListenHistoryStore::RadioPickEvent> limited = store.radioPickEvents(1);
    QCOMPARE(limited.size(), 1);
    QCOMPARE(limited.first().sessionKind, QStringLiteral("seeded"));
}

void TestListenHistory::radioPicksDoNotAffectAffinities()
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
    play.source = QStringLiteral("radio");
    play.sessionId = QStringLiteral("session-1");
    play.track = track;
    QVERIFY(store.recordPlayEvent(play) > 0);
    QVERIFY(store.recordListen(track, 1000, {}) > 0);

    const auto before = store.trackAffinities();
    QVERIFY(before.contains(track.path));

    ListenHistoryStore::RadioPickEvent sameTrack;
    sameTrack.occurredAtSecs = 1300;
    sameTrack.track = track;
    sameTrack.sessionKind = QStringLiteral("seeded");
    sameTrack.exploration0To100 = 30;
    sameTrack.weightsJson = R"({"genreWeight":3.0})";
    sameTrack.components = {{QStringLiteral("genre"), 1.0}};
    sameTrack.score = 1.0;
    QVERIFY(store.recordRadioPick(sameTrack));

    Track other = makeTrack(QStringLiteral("Other"), QStringLiteral("Other Artist"));
    other.path = QStringLiteral("/music/other.flac");
    ListenHistoryStore::RadioPickEvent otherTrack;
    otherTrack.occurredAtSecs = 1301;
    otherTrack.track = other;
    otherTrack.sessionKind = QStringLiteral("artist");
    otherTrack.exploration0To100 = 80;
    otherTrack.weightsJson = R"({"genreWeight":3.0})";
    otherTrack.components = {{QStringLiteral("novelty"), 0.8}};
    otherTrack.score = 0.8;
    QVERIFY(store.recordRadioPick(otherTrack));

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

    QCOMPARE(store.metaValue(QStringLiteral("schemaVersion")), QStringLiteral("8"));
}

void TestListenHistory::destinationsDeliverIndependently()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));

    const QString koito = QStringLiteral("custom-1");
    const QString other = QStringLiteral("custom-2");

    const qint64 first = store.recordListen(makeTrack(), 1000, {koito, other});
    const qint64 second = store.recordListen(makeTrack(QStringLiteral("Two")), 2000, {koito, other});
    const qint64 third = store.recordListen(makeTrack(QStringLiteral("Three")), 3000, {koito});

    QCOMPARE(store.pendingCount(koito), 3);
    QCOMPARE(store.pendingCount(other), 2);

    // Draining one destination leaves the other's backlog exactly as it was.
    store.markSent(koito, {first, second, third});
    QCOMPARE(store.pendingCount(koito), 0);
    QCOMPARE(store.sentCount(koito), 3);
    QCOMPARE(store.pendingCount(other), 2);
    QCOMPARE(store.sentCount(other), 0);

    // And so does clearing one destination's backlog.
    QCOMPARE(store.clearPending(other), 2);
    QCOMPARE(store.pendingCount(other), 0);
    QCOMPARE(store.sentCount(koito), 3);
    QCOMPARE(store.totalCount(), 3);
}

void TestListenHistory::backlogDrainsOldestFirstPerDestination()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));

    const QString koito = QStringLiteral("custom-1");
    store.recordListen(makeTrack(QStringLiteral("Third")), 3000, {koito});
    store.recordListen(makeTrack(QStringLiteral("First")), 1000, {koito});
    store.recordListen(makeTrack(QStringLiteral("Second")), 2000, {koito});

    const auto listens = store.unsent(koito, 10);
    QCOMPARE(listens.size(), 3);
    QCOMPARE(listens.at(0).listenedAtSecs, 1000);
    QCOMPARE(listens.at(1).listenedAtSecs, 2000);
    QCOMPARE(listens.at(2).listenedAtSecs, 3000);

    // The batch limit takes the oldest, not an arbitrary slice.
    const auto batch = store.unsent(koito, 2);
    QCOMPARE(batch.size(), 2);
    QCOMPARE(batch.at(0).listenedAtSecs, 1000);
    QCOMPARE(batch.at(1).listenedAtSecs, 2000);
}

void TestListenHistory::enablingADestinationDoesNotClaimOldListens()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));

    const QString koito = QStringLiteral("custom-1");
    store.recordListen(makeTrack(), 1000, {ListenHistoryStore::LastFm});
    store.recordListen(makeTrack(QStringLiteral("Two")), 2000, {ListenHistoryStore::LastFm});

    // A destination configured after the fact starts empty: history it never
    // witnessed is not a backlog it is owed.
    QCOMPARE(store.pendingCount(koito), 0);
    QCOMPARE(store.unsent(koito, 10).size(), 0);

    // The user can still enqueue specific rows explicitly.
    const auto rows = store.historyRows(10);
    QCOMPARE(store.markOwed(koito, {rows.first().id}), 1);
    QCOMPARE(store.pendingCount(koito), 1);
}

void TestListenHistory::removingADestinationDropsOnlyItsRows()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));

    const QString koito = QStringLiteral("custom-1");
    const QString other = QStringLiteral("custom-2");
    const qint64 id = store.recordListen(makeTrack(), 1000, {koito, other, ListenHistoryStore::LastFm});
    store.recordListen(makeTrack(QStringLiteral("Two")), 2000, {koito, other});
    store.markSent(koito, {id});

    // Removal takes delivered and pending rows alike, and nothing else.
    QCOMPARE(store.forgetDestination(koito), 2);
    QCOMPARE(store.pendingCount(koito), 0);
    QCOMPARE(store.sentCount(koito), 0);
    QCOMPARE(store.pendingCount(other), 2);
    QCOMPARE(store.pendingCount(ListenHistoryStore::LastFm), 1);
    // Local history is permanent; it is not a scrobbler's to delete.
    QCOMPARE(store.totalCount(), 2);
}

void TestListenHistory::historyRowsSummarizeDeliveryAcrossDestinations()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));

    const QString koito = QStringLiteral("custom-1");
    const qint64 id = store.recordListen(makeTrack(), 1000, {koito, ListenHistoryStore::LastFm});
    store.recordListen(makeTrack(QStringLiteral("Two")), 2000, {});
    store.markSent(koito, {id});

    const auto rows = store.historyRows(10);
    QCOMPARE(rows.size(), 2);

    // Newest first, so the listen owed to nobody leads.
    QCOMPARE(rows.at(0).listenedAtSecs, 2000);
    QCOMPARE(rows.at(0).owedCount(), 0);
    QCOMPARE(rows.at(0).sentCount(), 0);

    const ListenHistoryStore::HistoryRow &owed = rows.at(1);
    QCOMPARE(owed.owedCount(), 2);
    QCOMPARE(owed.sentCount(), 1);
    QVERIFY(owed.sentTo(koito));
    QVERIFY(owed.owedTo(ListenHistoryStore::LastFm));
    QVERIFY(!owed.sentTo(ListenHistoryStore::LastFm));
    QVERIFY(!owed.owedTo(ListenHistoryStore::ListenBrainz));
}

void TestListenHistory::migratesLegacyFlagsOnce()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("history.sqlite"));
    writeLegacyHistory(path);

    {
        ListenHistoryStore store(path);
        QVERIFY(store.isOpen());

        // Each legacy owed flag becomes a delivery row carrying its sent state.
        QCOMPARE(store.totalCount(), 4);
        QCOMPARE(store.pendingCount(ListenHistoryStore::LastFm), 1);
        QCOMPARE(store.sentCount(ListenHistoryStore::LastFm), 1);
        QCOMPARE(store.pendingCount(ListenHistoryStore::ListenBrainz), 2);
        QCOMPARE(store.sentCount(ListenHistoryStore::ListenBrainz), 0);

        // A listen that was owed to nobody stays owed to nobody.
        const auto rows = store.historyRows(10);
        QCOMPARE(rows.size(), 4);
        for (const auto &row : rows) {
            if (row.listenedAtSecs == 4000) {
                QCOMPARE(row.owedCount(), 0);
            }
        }
    }

    // Work done after migrating must survive reopening: the marker is what stops
    // a second pass from resurrecting a backlog the user deliberately cleared.
    {
        ListenHistoryStore store(path);
        QCOMPARE(store.clearPending(ListenHistoryStore::ListenBrainz), 2);
        store.markSent(ListenHistoryStore::LastFm, {1});
    }
    {
        ListenHistoryStore store(path);
        QCOMPARE(store.pendingCount(ListenHistoryStore::ListenBrainz), 0);
        QCOMPARE(store.sentCount(ListenHistoryStore::ListenBrainz), 0);
        QCOMPARE(store.pendingCount(ListenHistoryStore::LastFm), 0);
        QCOMPARE(store.sentCount(ListenHistoryStore::LastFm), 2);
        QCOMPARE(store.totalCount(), 4);
    }
}

void TestListenHistory::migrationLeavesLegacyColumnsInPlace()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("history.sqlite"));
    writeLegacyHistory(path);

    {
        ListenHistoryStore store(path);
        QVERIFY(store.isOpen());
        // New listens no longer write the legacy flags, but the columns stay:
        // dropping them would mean rewriting the whole listens table.
        store.recordListen(makeTrack(QStringLiteral("Fresh")), 9000, {ListenHistoryStore::LastFm});
        QCOMPARE(store.pendingCount(ListenHistoryStore::LastFm), 2);
    }

    const QString connection = QStringLiteral("legacy-columns-check");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(path);
        QVERIFY(db.open());

        QSqlQuery columns(db);
        QVERIFY(columns.exec(QStringLiteral("SELECT COUNT(*) FROM pragma_table_info('listens') "
                                            "WHERE name IN ('owed_lastfm','sent_lastfm',"
                                            "'owed_listenbrainz','sent_listenbrainz')")));
        QVERIFY(columns.next());
        QCOMPARE(columns.value(0).toInt(), 4);

        // The fresh listen left them at their defaults rather than writing them.
        QSqlQuery fresh(db);
        QVERIFY(fresh.exec(QStringLiteral("SELECT owed_lastfm, sent_lastfm FROM listens WHERE listened_at = 9000")));
        QVERIFY(fresh.next());
        QCOMPARE(fresh.value(0).toInt(), 0);
        QCOMPARE(fresh.value(1).toInt(), 0);
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

QTEST_GUILESS_MAIN(TestListenHistory)
#include "test_listen_history.moc"
