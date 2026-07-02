#include "scrobble/ListenHistoryStore.h"
#include "scrobble/PlayEventRecorder.h"

#include <QTemporaryDir>
#include <QtTest>

using PlayEvent = ListenHistoryStore::PlayEvent;

namespace {

Track makeTrack(const QString &path, qint64 durationMs = 200000)
{
    Track track;
    track.path = path;
    track.title = QStringLiteral("Song %1").arg(path);
    track.artistName = QStringLiteral("Artist");
    track.albumTitle = QStringLiteral("Album");
    track.durationMs = durationMs;
    track.musicBrainz.recordingId = QStringLiteral("rec-%1").arg(path);
    return track;
}

// Collects every event a recorder emits, so tests can assert on outcomes.
struct EventSink {
    QList<PlayEvent> events;
    explicit EventSink(PlayEventRecorder &rec)
    {
        QObject::connect(&rec, &PlayEventRecorder::playEventReady,
                         [this](PlayEvent event) { events.push_back(event); });
    }
};

} // namespace

class TestPlayEvents final : public QObject {
    Q_OBJECT

private slots:
    void storeRoundTrip();
    void storeRejectsInvalid();
    void storeForgetBehaviorRemovesPlayEventsAndOptionalImports();
    void recorderNaturalFinish();
    void recorderEarlySkipChainsPrevious();
    void recorderPauseExcludedFromPlayed();
    void recorderStoppedAndSessionEnd();
    void recorderSessionRollover();
    void recorderLongPlayingTrackKeepsSession();
    void recorderCompletionUnknownWhenNoDuration();
    void recorderResumeSeedsElapsed();
};

void TestPlayEvents::storeRoundTrip()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));
    QVERIFY(store.isOpen());

    PlayEvent event;
    event.startedAtSecs = 10000;
    event.endedAtSecs = 10200;
    event.playedMs = 120000;
    event.durationMs = 200000;
    event.completion = 0.6;
    event.outcome = QStringLiteral("finished");
    event.userInitiated = true;
    event.source = QStringLiteral("queue_manual");
    event.shuffleMode = QStringLiteral("queue");
    event.track = makeTrack(QStringLiteral("/music/a.flac"));
    event.previousTrackPath = QStringLiteral("/music/z.flac");
    event.sessionId = QStringLiteral("sess-1");

    const qint64 id = store.recordPlayEvent(event);
    QVERIFY(id > 0);
    QCOMPARE(store.playEventCount(), 1);

    // A second, sparser event to check ordering (newest first) and NULLs.
    PlayEvent second;
    second.startedAtSecs = 20000;
    second.playedMs = 3000;
    second.durationMs = 0;      // duration unknown
    second.completion = -1.0;   // stored NULL, reported as <0
    second.outcome = QStringLiteral("skipped");
    second.source = QStringLiteral("queue_auto");
    second.track = makeTrack(QStringLiteral("/music/b.flac"), 0);
    second.sessionId = QStringLiteral("sess-1");
    QVERIFY(store.recordPlayEvent(second) > 0);
    QCOMPARE(store.playEventCount(), 2);

    const auto rows = store.recentPlayEvents(10);
    QCOMPARE(rows.size(), 2);

    // Newest first.
    const PlayEvent &newest = rows.first();
    QCOMPARE(newest.startedAtSecs, 20000);
    QCOMPARE(newest.outcome, QStringLiteral("skipped"));
    QVERIFY(newest.completion < 0.0);   // NULL round-trips to unknown
    QCOMPARE(newest.durationMs, 0);

    const PlayEvent &oldest = rows.last();
    QCOMPARE(oldest.startedAtSecs, 10000);
    QCOMPARE(oldest.endedAtSecs, 10200);
    QCOMPARE(oldest.playedMs, 120000);
    QCOMPARE(oldest.durationMs, 200000);
    QVERIFY(qFuzzyCompare(oldest.completion, 0.6));
    QCOMPARE(oldest.outcome, QStringLiteral("finished"));
    QVERIFY(oldest.userInitiated);
    QCOMPARE(oldest.source, QStringLiteral("queue_manual"));
    QCOMPARE(oldest.shuffleMode, QStringLiteral("queue"));
    QCOMPARE(oldest.previousTrackPath, QStringLiteral("/music/z.flac"));
    QCOMPARE(oldest.sessionId, QStringLiteral("sess-1"));
    // Track restored from track_json.
    QCOMPARE(oldest.track.path, QStringLiteral("/music/a.flac"));
    QCOMPARE(oldest.track.albumTitle, QStringLiteral("Album"));
    QCOMPARE(oldest.track.durationMs, 200000);
    QCOMPARE(oldest.track.musicBrainz.recordingId, QStringLiteral("rec-/music/a.flac"));
}

void TestPlayEvents::storeRejectsInvalid()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));

    PlayEvent base;
    base.outcome = QStringLiteral("finished");
    base.source = QStringLiteral("queue_auto");
    base.track = makeTrack(QStringLiteral("/music/a.flac"));
    base.sessionId = QStringLiteral("sess-1");

    PlayEvent noOutcome = base;
    noOutcome.outcome.clear();
    QCOMPARE(store.recordPlayEvent(noOutcome), -1);

    PlayEvent noSession = base;
    noSession.sessionId.clear();
    QCOMPARE(store.recordPlayEvent(noSession), -1);

    PlayEvent noPath = base;
    noPath.track.path.clear();
    QCOMPARE(store.recordPlayEvent(noPath), -1);

    QCOMPARE(store.playEventCount(), 0);
    // The valid baseline still records.
    QVERIFY(store.recordPlayEvent(base) > 0);
}

void TestPlayEvents::storeForgetBehaviorRemovesPlayEventsAndOptionalImports()
{
    QTemporaryDir dir;
    ListenHistoryStore store(dir.filePath(QStringLiteral("history.sqlite")));
    QVERIFY(store.isOpen());

    const Track a = makeTrack(QStringLiteral("/music/a.flac"));
    const Track b = makeTrack(QStringLiteral("/music/b.flac"));
    QVERIFY(store.recordListen(a, 1000, true, true) > 0);

    PlayEvent eventA;
    eventA.startedAtSecs = 2000;
    eventA.playedMs = 5000;
    eventA.outcome = QStringLiteral("skipped");
    eventA.source = QStringLiteral("radio");
    eventA.track = a;
    eventA.sessionId = QStringLiteral("session");
    QVERIFY(store.recordPlayEvent(eventA) > 0);

    PlayEvent eventB = eventA;
    eventB.startedAtSecs = 3000;
    eventB.track = b;
    QVERIFY(store.recordPlayEvent(eventB) > 0);

    ListenHistoryStore::ImportedListen importedA;
    importedA.source = ListenHistoryStore::LastFm;
    importedA.listenedAtSecs = 4000;
    importedA.title = a.title;
    importedA.artist = a.artistName;
    importedA.matchedTrackPath = a.path;

    ListenHistoryStore::ImportedListen importedB = importedA;
    importedB.listenedAtSecs = 5000;
    importedB.title = b.title;
    importedB.matchedTrackPath = b.path;

    QCOMPARE(store.recordImportedListens({importedA, importedB}), 2);
    QCOMPARE(store.totalCount(), 1);
    QCOMPARE(store.playEventCount(), 2);
    QCOMPARE(store.importedListenCount(ListenHistoryStore::LastFm), 2);

    QCOMPARE(store.forgetTrackBehavior({a.path, a.path}, false), 1);
    QCOMPARE(store.totalCount(), 1);
    QCOMPARE(store.playEventCount(), 1);
    QCOMPARE(store.importedListenCount(ListenHistoryStore::LastFm), 2);

    QCOMPARE(store.forgetTrackBehavior({a.path}, true), 1);
    QCOMPARE(store.totalCount(), 1);
    QCOMPARE(store.playEventCount(), 1);
    QCOMPARE(store.importedListenCount(ListenHistoryStore::LastFm), 1);
}

void TestPlayEvents::recorderNaturalFinish()
{
    qint64 clock = 1'000'000;
    PlayEventRecorder rec;
    rec.setClock([&clock] { return clock; });
    EventSink sink(rec);

    const Track a = makeTrack(QStringLiteral("/a"), 200000);
    rec.trackStarted(a, true, QStringLiteral("queue_manual"));
    clock += 250000;   // played past the end
    rec.trackFinishedNaturally(a);

    QCOMPARE(sink.events.size(), 1);
    const PlayEvent &e = sink.events.first();
    QCOMPARE(e.outcome, QStringLiteral("finished"));
    QCOMPARE(e.playedMs, 250000);          // playedMs == wall-clock elapsed
    QVERIFY(qFuzzyCompare(e.completion, 1.0));   // capped at 1.0
    QVERIFY(e.userInitiated);
    QCOMPARE(e.source, QStringLiteral("queue_manual"));
    QCOMPARE(e.startedAtSecs, 1000);
}

void TestPlayEvents::recorderEarlySkipChainsPrevious()
{
    qint64 clock = 5'000'000;
    PlayEventRecorder rec;
    rec.setClock([&clock] { return clock; });
    EventSink sink(rec);

    const Track a = makeTrack(QStringLiteral("/a"));
    const Track b = makeTrack(QStringLiteral("/b"));
    rec.trackStarted(a, true, QStringLiteral("queue_manual"));
    clock += 5000;
    rec.trackStarted(b, false, QStringLiteral("queue_auto"));   // finalizes A as skipped

    QCOMPARE(sink.events.size(), 1);
    QCOMPARE(sink.events.first().outcome, QStringLiteral("skipped"));
    QCOMPARE(sink.events.first().playedMs, 5000);
    QCOMPARE(sink.events.first().track.path, QStringLiteral("/a"));

    clock += 1000;
    rec.trackFinishedNaturally(b);
    QCOMPARE(sink.events.size(), 2);
    // B carries A as its previous track (same session).
    QCOMPARE(sink.events.at(1).track.path, QStringLiteral("/b"));
    QCOMPARE(sink.events.at(1).previousTrackPath, QStringLiteral("/a"));
    // A had no predecessor.
    QVERIFY(sink.events.at(0).previousTrackPath.isEmpty());
}

void TestPlayEvents::recorderPauseExcludedFromPlayed()
{
    qint64 clock = 2'000'000;
    PlayEventRecorder rec;
    rec.setClock([&clock] { return clock; });
    EventSink sink(rec);

    const Track a = makeTrack(QStringLiteral("/a"));
    rec.trackStarted(a, true, QStringLiteral("queue_manual"));
    clock += 3000;                       // played
    rec.playbackStateChanged(false);     // pause
    clock += 10000;                      // paused, excluded
    rec.playbackStateChanged(true);      // resume
    clock += 2000;                       // played
    rec.trackFinishedNaturally(a);

    QCOMPARE(sink.events.size(), 1);
    QCOMPARE(sink.events.first().playedMs, 5000);   // 3000 + 2000, pause excluded
}

void TestPlayEvents::recorderStoppedAndSessionEnd()
{
    qint64 clock = 3'000'000;
    PlayEventRecorder rec;
    rec.setClock([&clock] { return clock; });
    EventSink sink(rec);

    rec.trackStarted(makeTrack(QStringLiteral("/a")), true, QStringLiteral("queue_manual"));
    clock += 4000;
    rec.playbackCleared();
    QCOMPARE(sink.events.size(), 1);
    QCOMPARE(sink.events.first().outcome, QStringLiteral("stopped"));

    rec.trackStarted(makeTrack(QStringLiteral("/b")), true, QStringLiteral("queue_manual"));
    clock += 1000;
    rec.flushSessionEnd();
    QCOMPARE(sink.events.size(), 2);
    QCOMPARE(sink.events.at(1).outcome, QStringLiteral("session_end"));

    // A second flush with nothing open emits nothing.
    rec.flushSessionEnd();
    QCOMPARE(sink.events.size(), 2);
}

void TestPlayEvents::recorderSessionRollover()
{
    qint64 clock = 10'000'000;
    PlayEventRecorder rec;
    rec.setClock([&clock] { return clock; });
    EventSink sink(rec);

    const Track a = makeTrack(QStringLiteral("/a"));
    const Track b = makeTrack(QStringLiteral("/b"));
    const Track c = makeTrack(QStringLiteral("/c"));

    rec.trackStarted(a, true, QStringLiteral("queue_manual"));
    clock += 10 * 60 * 1000;                     // 10 min, within the gap
    rec.trackStarted(b, false, QStringLiteral("queue_auto"));   // emits A
    clock += 2000;
    rec.playbackStateChanged(false);             // pause: idleness starts here
    clock += 40 * 60 * 1000;                     // 40 min paused, past the gap
    rec.trackStarted(c, false, QStringLiteral("queue_auto"));   // emits B, rolls session
    rec.flushSessionEnd();                                      // emits C

    QCOMPARE(sink.events.size(), 3);
    const PlayEvent &ea = sink.events.at(0);
    const PlayEvent &eb = sink.events.at(1);
    const PlayEvent &ec = sink.events.at(2);

    // Within-gap starts share a session and chain previousTrackPath.
    QCOMPARE(ea.sessionId, eb.sessionId);
    QVERIFY(!ea.sessionId.isEmpty());
    QCOMPARE(eb.previousTrackPath, QStringLiteral("/a"));

    // The >30 min idle gap rolls a fresh session and breaks the chain.
    QVERIFY(ec.sessionId != eb.sessionId);
    QVERIFY(ec.previousTrackPath.isEmpty());
}

void TestPlayEvents::recorderLongPlayingTrackKeepsSession()
{
    qint64 clock = 20'000'000;
    PlayEventRecorder rec;
    rec.setClock([&clock] { return clock; });
    EventSink sink(rec);

    // A track longer than the idle gap plays continuously: skipping out of it
    // afterwards is one uninterrupted listening session, not a return from
    // idleness.
    const Track mix = makeTrack(QStringLiteral("/mix"), 45 * 60 * 1000);
    const Track b = makeTrack(QStringLiteral("/b"));
    rec.trackStarted(mix, true, QStringLiteral("queue_manual"));
    clock += 40 * 60 * 1000;                     // played 40 min, no other events
    rec.trackStarted(b, false, QStringLiteral("queue_auto"));   // emits mix as skipped
    rec.flushSessionEnd();                                      // emits B

    QCOMPARE(sink.events.size(), 2);
    QCOMPARE(sink.events.at(0).track.path, QStringLiteral("/mix"));
    QCOMPARE(sink.events.at(1).sessionId, sink.events.at(0).sessionId);
    QCOMPARE(sink.events.at(1).previousTrackPath, QStringLiteral("/mix"));
}

void TestPlayEvents::recorderCompletionUnknownWhenNoDuration()
{
    qint64 clock = 4'000'000;
    PlayEventRecorder rec;
    rec.setClock([&clock] { return clock; });
    EventSink sink(rec);

    const Track a = makeTrack(QStringLiteral("/a"), 0);   // duration unknown
    rec.trackStarted(a, true, QStringLiteral("queue_manual"));
    clock += 30000;
    rec.trackFinishedNaturally(a);

    QCOMPARE(sink.events.size(), 1);
    QVERIFY(sink.events.first().completion < 0.0);
    QCOMPARE(sink.events.first().playedMs, 30000);
}

void TestPlayEvents::recorderResumeSeedsElapsed()
{
    qint64 clock = 6'000'000;
    PlayEventRecorder rec;
    rec.setClock([&clock] { return clock; });
    EventSink sink(rec);

    const Track a = makeTrack(QStringLiteral("/a"));
    rec.resumeTrack(a, 45000, true, QStringLiteral("resume"));
    clock += 5000;
    rec.trackFinishedNaturally(a);

    QCOMPARE(sink.events.size(), 1);
    const PlayEvent &e = sink.events.first();
    QCOMPARE(e.playedMs, 50000);        // 45000 seed + 5000 live
    QCOMPARE(e.source, QStringLiteral("resume"));
    QVERIFY(!e.userInitiated);
    // startedAt anchored to when the track originally began (now - elapsed).
    QCOMPARE(e.startedAtSecs, (6'000'000 - 45000) / 1000);
}

QTEST_GUILESS_MAIN(TestPlayEvents)
#include "test_play_events.moc"
