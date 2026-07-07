#include "player/PlayerCore.h"

#include "playback/PlaybackBackend.h"

#include <QSignalSpy>
#include <QTest>

// Fake backend: records calls, lets tests drive gapless/finished transitions.
class FakeBackend final : public PlaybackBackend {
    Q_OBJECT

public:
    void setProfile(const PlaybackProfile &) override {}
    void play(const QUrl &url) override
    {
        m_source = url;
        m_state = State::Playing;
        playedUrls.push_back(url);
    }
    void loadPaused(const QUrl &url) override
    {
        m_source = url;
        m_state = State::Paused;
        loadedPausedUrls.push_back(url);
    }
    void prepareNext(const QUrl &url) override { preparedUrls.push_back(url); }
    void pause() override { m_state = State::Paused; }
    void resume() override { m_state = State::Playing; }
    void stop() override
    {
        m_source.clear();
        m_state = State::Stopped;
        ++stopCalls;
    }
    void seek(qint64 positionMs) override { lastSeekMs = positionMs; }
    void setVolume(double volume0To1) override { lastVolume = volume0To1; }
    void setOutputMode(OutputMode mode, const QString &device = QString()) override
    {
        outputModes.push_back(mode);
        outputDevices.push_back(device);
    }
    State state() const override { return m_state; }
    bool hasSource() const override { return !m_source.isEmpty(); }
    qint64 position() const override { return positionMs; }
    qint64 duration() const override { return durationMs; }

    QVector<QUrl> playedUrls;
    QVector<QUrl> loadedPausedUrls;
    QVector<QUrl> preparedUrls;
    QVector<OutputMode> outputModes;
    QVector<QString> outputDevices;
    qint64 lastSeekMs = -1;
    double lastVolume = -1.0;
    qint64 positionMs = 0;
    qint64 durationMs = 0;
    int stopCalls = 0;

private:
    QUrl m_source;
    State m_state = State::Stopped;
};

namespace {

Track makeTrack(const QString &path)
{
    Track track;
    track.path = path;
    track.title = path;
    return track;
}

QVector<Track> makeTracks(const QStringList &paths)
{
    QVector<Track> tracks;
    for (const QString &path : paths) {
        tracks.push_back(makeTrack(path));
    }
    return tracks;
}

} // namespace

class PlayerCoreTest final : public QObject {
    Q_OBJECT

private slots:
    void init();
    void playAtStartsTrackAndPreparesNext();
    void appendAndPlayJumpsToExistingEntry();
    void playTracksNextInsertsAfterCurrent();
    void removingCurrentAdvancesPlayback();
    void removingCurrentWhilePausedStaysPausedOnNext();
    void removingLastTrackClearsPlayback();
    void gaplessAdvanceMovesIndexWithoutReveal();
    void finishedAtEndOfQueueStops();
    void trackFinishedEmittedOnBackendFinish();
    void trackFinishedEmittedOnGaplessAdvance();
    void explicitJumpCollapsesPlayNext();
    void clearKeepingCurrentKeepsOnlyCurrent();
    void metadataPatchUpdatesRowsWithoutQueueReset();
    void missingPatchUpdatesRowsWithoutQueueReset();
    void repeatAllWrapsAtEndOfQueue();
    void repeatAllGaplesslyPreparesFirstTrack();
    void repeatOneReplaysCurrentTrack();
    void shuffleVisitsEveryTrackOnceThenStops();
    void shufflePreviousRetracesHistory();
    void shuffleNextReplaysForwardAfterPrevious();
    void shuffleManualPickRefreshesBucketAndPreservesPrevious();
    void shuffleAppendAndPlayRefreshesBucket();
    void shuffleBackwardJumpDoesNotBadgePlayNext();
    void libraryShuffleInjectsLibraryTrack();
    void radioShuffleAtPercent100InjectsRadioProviderTrack();
    void radioShuffleAtPercent0UsesQueueShuffle();
    void explicitRadioActiveTakesPrecedenceOverRadioShuffleRoll();
    void radioMidQueueAdvancesLinearly();
    void radioManualAppendMidQueueAdvancesBeforeProvider();
    void radioAtEndInjectsProviderPickOnAutoAdvance();
    void radioAtEndInjectsProviderPickOnManualNext();
    void radioEmptyProviderFallsBackToEndOfQueue();
    void radioInactiveLeavesShuffleUntouched();
    void dsdTakeoverDefersThenStartsNatively();
    void declinedDsdSkipsContiguousBlockUntilPlaybackStarts();
    void declinedDsdUnderRepeatAllStopsInsteadOfLooping();

private:
    FakeBackend *m_backend = nullptr;   // owned by m_core
    std::unique_ptr<PlayerCore> m_core;
};

void PlayerCoreTest::init()
{
    qRegisterMetaType<QVector<int>>("QVector<int>");
    m_backend = new FakeBackend;
    m_core = std::make_unique<PlayerCore>(m_backend);
}

void PlayerCoreTest::playAtStartsTrackAndPreparesNext()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c"}));
    QSignalSpy started(m_core.get(), &PlayerCore::currentTrackChanged);

    m_core->playAt(0);

    QCOMPARE(m_core->queueIndex(), 0);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/a"));
    QCOMPARE(started.count(), 1);
    QCOMPARE(m_backend->playedUrls.size(), 1);
    QCOMPARE(m_backend->playedUrls.last(), QUrl::fromLocalFile("/a"));
    // The gapless "next" buffer must point at the following queue row.
    QCOMPARE(m_backend->preparedUrls.last(), QUrl::fromLocalFile("/b"));
}

void PlayerCoreTest::appendAndPlayJumpsToExistingEntry()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->playAt(0);

    m_core->appendAndPlay(makeTrack("/b"));
    QCOMPARE(m_core->queue().size(), 2);  // no duplicate appended
    QCOMPARE(m_core->queueIndex(), 1);

    QSignalSpy aboutToAdd(m_core.get(), &PlayerCore::aboutToAddTracks);
    m_core->appendAndPlay(makeTrack("/c"));
    QCOMPARE(aboutToAdd.count(), 1);
    QCOMPARE(m_core->queue().size(), 3);
    QCOMPARE(m_core->queueIndex(), 2);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/c"));
}

void PlayerCoreTest::playTracksNextInsertsAfterCurrent()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->playAt(0);

    m_core->playTracksNext(makeTracks({"/x", "/y"}));
    QCOMPARE(m_core->queue().size(), 4);
    QCOMPARE(m_core->queue().at(1).path, QStringLiteral("/x"));
    QCOMPARE(m_core->queue().at(2).path, QStringLiteral("/y"));
    QCOMPARE(m_core->playNextInsertIndex(), 3);
    // A second batch lands after the first (batch ordering is preserved).
    m_core->playTracksNext(makeTracks({"/z"}));
    QCOMPARE(m_core->queue().at(3).path, QStringLiteral("/z"));
    QCOMPARE(m_core->playNextInsertIndex(), 4);
    // The prepared next must follow the new order.
    QCOMPARE(m_backend->preparedUrls.last(), QUrl::fromLocalFile("/x"));
}

void PlayerCoreTest::removingCurrentAdvancesPlayback()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c"}));
    m_core->playAt(1);  // playing /b
    QCOMPARE(m_backend->playedUrls.last(), QUrl::fromLocalFile("/b"));

    QSignalSpy changed(m_core.get(), &PlayerCore::currentTrackChanged);
    m_core->removeRows({1});  // remove the track that's playing

    QCOMPARE(m_core->queue().size(), 2);
    QCOMPARE(m_core->queueIndex(), 1);
    // It advances onto and plays the successor (/c) rather than leaving the
    // removed /b audio playing.
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/c"));
    QCOMPARE(m_backend->playedUrls.last(), QUrl::fromLocalFile("/c"));
    QCOMPARE(changed.last().at(0).value<Track>().path, QStringLiteral("/c"));
    QCOMPARE(changed.last().at(1).toBool(), true);  // new track playing → scrobbler notified
}

void PlayerCoreTest::removingCurrentWhilePausedStaysPausedOnNext()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c"}));
    m_core->playAt(1);
    m_core->togglePlayPause();  // pause on /b
    QCOMPARE(m_backend->state(), PlaybackBackend::State::Paused);

    m_core->removeRows({1});
    QCOMPARE(m_core->queueIndex(), 1);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/c"));
    // Stay paused, but on the new current so the backend source matches the queue.
    QCOMPARE(m_backend->loadedPausedUrls.last(), QUrl::fromLocalFile("/c"));
    QCOMPARE(m_backend->state(), PlaybackBackend::State::Paused);
}

void PlayerCoreTest::removingLastTrackClearsPlayback()
{
    m_core->resetQueue(makeTracks({"/a"}));
    m_core->playAt(0);

    QSignalSpy cleared(m_core.get(), &PlayerCore::playbackCleared);
    m_core->removeRows({0});
    QVERIFY(m_core->queue().isEmpty());
    QCOMPARE(m_core->queueIndex(), -1);
    QVERIFY(m_core->currentTrack().path.isEmpty());
    QCOMPARE(cleared.count(), 1);
    QVERIFY(m_backend->stopCalls > 0);
}

void PlayerCoreTest::gaplessAdvanceMovesIndexWithoutReveal()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->playAt(0);

    QSignalSpy indexChanged(m_core.get(), &PlayerCore::currentIndexChanged);
    QSignalSpy started(m_core.get(), &PlayerCore::currentTrackChanged);
    emit m_backend->preparedTrackStarted();

    QCOMPARE(m_core->queueIndex(), 1);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/b"));
    QCOMPARE(indexChanged.count(), 1);
    QCOMPARE(indexChanged.last().at(1).toBool(), false);  // not user-initiated
    QCOMPARE(started.count(), 1);
    QCOMPARE(started.last().at(1).toBool(), true);  // scrobbler notified
    // End of queue: prepared next cleared.
    QCOMPARE(m_backend->preparedUrls.last(), QUrl());
}

void PlayerCoreTest::finishedAtEndOfQueueStops()
{
    m_core->resetQueue(makeTracks({"/a"}));
    m_core->playAt(0);

    const int stopsBefore = m_backend->stopCalls;
    emit m_backend->finished();
    QCOMPARE(m_backend->stopCalls, stopsBefore + 1);
    QCOMPARE(m_core->queueIndex(), 0);  // queue position survives end-of-queue

    // With more rows, finished advances instead.
    m_core->resetQueue(makeTracks({"/a", "/b"}), 0, 1);
    emit m_backend->finished();
    QCOMPARE(m_core->queueIndex(), 1);
    QCOMPARE(m_backend->playedUrls.last(), QUrl::fromLocalFile("/b"));
}

void PlayerCoreTest::trackFinishedEmittedOnBackendFinish()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}), 0, 1);
    m_core->playAt(0);

    QSignalSpy finished(m_core.get(), &PlayerCore::trackFinished);
    emit m_backend->finished();   // /a played out; auto-advance to /b

    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.last().at(0).value<Track>().path, QStringLiteral("/a"));
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/b"));

    // End-of-queue finish (no successor) still reports the outgoing track ended.
    finished.clear();
    emit m_backend->finished();
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.last().at(0).value<Track>().path, QStringLiteral("/b"));
}

void PlayerCoreTest::trackFinishedEmittedOnGaplessAdvance()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->playAt(0);

    QSignalSpy finished(m_core.get(), &PlayerCore::trackFinished);
    emit m_backend->preparedTrackStarted();   // gapless takeover; finished() never fires

    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.last().at(0).value<Track>().path, QStringLiteral("/a"));
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/b"));
}

void PlayerCoreTest::explicitJumpCollapsesPlayNext()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c", "/d"}));
    m_core->playAt(0);
    m_core->playTracksNext(makeTracks({"/x"}));
    QCOMPARE(m_core->playNextInsertIndex(), 2);

    m_core->playAt(3, true, false, /*explicitJump=*/true);
    QCOMPARE(m_core->playNextInsertIndex(), 4);
}

void PlayerCoreTest::clearKeepingCurrentKeepsOnlyCurrent()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c"}));
    m_core->playAt(1);

    m_core->clearKeepingCurrent();
    QCOMPARE(m_core->queue().size(), 1);
    QCOMPARE(m_core->queue().first().path, QStringLiteral("/b"));
    QCOMPARE(m_core->queueIndex(), 0);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/b"));
}

void PlayerCoreTest::metadataPatchUpdatesRowsWithoutQueueReset()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->playAt(0);

    QSignalSpy queueReset(m_core.get(), &PlayerCore::queueChanged);
    QSignalSpy rowsChanged(m_core.get(), &PlayerCore::queueTracksChanged);
    QSignalSpy currentUpdated(m_core.get(), &PlayerCore::currentTrackUpdated);

    Track patched = makeTrack(QStringLiteral("/b"));
    patched.title = QStringLiteral("patched b");
    m_core->patchTracksFromMetadata({patched});

    QCOMPARE(queueReset.count(), 0);
    QCOMPARE(rowsChanged.count(), 1);
    QCOMPARE(rowsChanged.first().at(0).value<QVector<int>>(), QVector<int>{1});
    QCOMPARE(currentUpdated.count(), 0);
    QCOMPARE(m_core->queue().at(1).title, QStringLiteral("patched b"));

    patched = makeTrack(QStringLiteral("/a"));
    patched.title = QStringLiteral("patched a");
    m_core->patchTracksFromMetadata({patched});

    QCOMPARE(queueReset.count(), 0);
    QCOMPARE(rowsChanged.count(), 2);
    QCOMPARE(rowsChanged.last().at(0).value<QVector<int>>(), QVector<int>{0});
    QCOMPARE(currentUpdated.count(), 1);
    QCOMPARE(currentUpdated.last().at(0).value<Track>().title, QStringLiteral("patched a"));
}

void PlayerCoreTest::missingPatchUpdatesRowsWithoutQueueReset()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->playAt(0);

    QSignalSpy queueReset(m_core.get(), &PlayerCore::queueChanged);
    QSignalSpy rowsChanged(m_core.get(), &PlayerCore::queueTracksChanged);
    QSignalSpy currentUpdated(m_core.get(), &PlayerCore::currentTrackUpdated);

    m_core->markTracksMissing({QStringLiteral("/a"), QStringLiteral("/b")});

    QCOMPARE(queueReset.count(), 0);
    QCOMPARE(rowsChanged.count(), 1);
    QCOMPARE(rowsChanged.first().at(0).value<QVector<int>>(), QVector<int>({0, 1}));
    QVERIFY(m_core->queue().at(0).missing);
    QVERIFY(m_core->queue().at(1).missing);
    QVERIFY(m_core->currentTrack().missing);
    QCOMPARE(currentUpdated.count(), 1);
}

void PlayerCoreTest::repeatAllWrapsAtEndOfQueue()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->setRepeatMode(RepeatMode::All);
    m_core->playAt(1);

    // Auto-advance past the last track wraps to the first.
    emit m_backend->finished();
    QCOMPARE(m_core->queueIndex(), 0);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/a"));

    // A manual Next on the last track wraps too.
    m_core->playAt(1);
    m_core->next();
    QCOMPARE(m_core->queueIndex(), 0);
}

void PlayerCoreTest::repeatAllGaplesslyPreparesFirstTrack()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->setRepeatMode(RepeatMode::All);
    m_core->playAt(1);
    // On the last track the gapless buffer points at the wrap target.
    QCOMPARE(m_backend->preparedUrls.last(), QUrl::fromLocalFile("/a"));

    emit m_backend->preparedTrackStarted();
    QCOMPARE(m_core->queueIndex(), 0);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/a"));
}

void PlayerCoreTest::repeatOneReplaysCurrentTrack()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->setRepeatMode(RepeatMode::One);
    m_core->playAt(0);
    // Repeat-one does not gaplessly preload anything.
    QCOMPARE(m_backend->preparedUrls.last(), QUrl());

    const int playsBefore = m_backend->playedUrls.size();
    emit m_backend->finished();
    QCOMPARE(m_core->queueIndex(), 0);
    QCOMPARE(m_backend->playedUrls.size(), playsBefore + 1);
    QCOMPARE(m_backend->playedUrls.last(), QUrl::fromLocalFile("/a"));

    // A manual Next still moves on rather than re-looping.
    m_core->next();
    QCOMPARE(m_core->queueIndex(), 1);
}

void PlayerCoreTest::shuffleVisitsEveryTrackOnceThenStops()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->setShuffleMode(ShuffleMode::Queue);
    m_core->playAt(0);

    // Only one unvisited track remains, so the pick is deterministic.
    m_core->next();
    QCOMPARE(m_core->queueIndex(), 1);

    // Both tracks have played and repeat is off: Next is a no-op.
    m_core->next();
    QCOMPARE(m_core->queueIndex(), 1);
}

void PlayerCoreTest::shufflePreviousRetracesHistory()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c"}));
    m_core->setShuffleMode(ShuffleMode::Queue);
    m_core->playAt(0);

    m_core->next();
    const int second = m_core->queueIndex();
    QVERIFY(second != 0);

    // Previous walks the shuffle history back to the starting track.
    m_core->previous();
    QCOMPARE(m_core->queueIndex(), 0);
}

void PlayerCoreTest::shuffleNextReplaysForwardAfterPrevious()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c", "/d"}));
    m_core->setShuffleMode(ShuffleMode::Queue);
    m_core->playAt(0);

    m_core->next();
    const int first = m_core->queueIndex();
    m_core->next();
    const int second = m_core->queueIndex();
    QVERIFY(first != 0);
    QVERIFY(second != first);

    // Retrace all the way back to the starting track.
    m_core->previous();
    QCOMPARE(m_core->queueIndex(), first);
    m_core->previous();
    QCOMPARE(m_core->queueIndex(), 0);

    // Next now replays the exact same forward order it just retraced, rather than
    // re-rolling a fresh shuffle pick: shuffle navigation is linear in memory.
    m_core->next();
    QCOMPARE(m_core->queueIndex(), first);
    m_core->next();
    QCOMPARE(m_core->queueIndex(), second);
}

void PlayerCoreTest::shuffleManualPickRefreshesBucketAndPreservesPrevious()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->setShuffleMode(ShuffleMode::Queue);
    m_core->playAt(0);
    m_core->next();
    QCOMPARE(m_core->queueIndex(), 1);

    // Exhausted before the manual pick: without a bucket refresh, this would
    // leave no eligible next track.
    m_core->playAt(0, true, false, /*explicitJump=*/true);
    QCOMPARE(m_core->queueIndex(), 0);
    m_core->next();
    QCOMPARE(m_core->queueIndex(), 1);

    // The manual pick also becomes a normal navigation step for Previous.
    m_core->playAt(0, true, false, /*explicitJump=*/true);
    m_core->previous();
    QCOMPARE(m_core->queueIndex(), 1);
}

void PlayerCoreTest::shuffleAppendAndPlayRefreshesBucket()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->setShuffleMode(ShuffleMode::Queue);
    m_core->playAt(0);
    m_core->next();
    QCOMPARE(m_core->queueIndex(), 1);

    m_core->appendAndPlay(makeTrack("/a"));
    QCOMPARE(m_core->queue().size(), 2);
    QCOMPARE(m_core->queueIndex(), 0);
    m_core->next();
    QCOMPARE(m_core->queueIndex(), 1);

    m_core->appendAndPlay(makeTrack("/c"));
    QCOMPARE(m_core->queue().size(), 3);
    QCOMPARE(m_core->queueIndex(), 2);
    m_core->next();
    QVERIFY(m_core->queueIndex() != 2);
}

void PlayerCoreTest::shuffleBackwardJumpDoesNotBadgePlayNext()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c", "/d"}));
    m_core->setShuffleMode(ShuffleMode::Queue);
    m_core->playAt(0);
    // Under shuffle the next track is a random/remembered pick, not the row after
    // current, so the play-next region is meaningless and must stay collapsed:
    // every move keeps it at current+1 so no skipped-over rows are badged.
    QCOMPARE(m_core->playNextInsertIndex(), m_core->queueIndex() + 1);

    m_core->next();
    QCOMPARE(m_core->playNextInsertIndex(), m_core->queueIndex() + 1);
    m_core->next();
    QCOMPARE(m_core->playNextInsertIndex(), m_core->queueIndex() + 1);

    // Retracing backward must not leave a stale region spanning the skipped rows.
    m_core->previous();
    QCOMPARE(m_core->playNextInsertIndex(), m_core->queueIndex() + 1);
    m_core->previous();
    QCOMPARE(m_core->playNextInsertIndex(), m_core->queueIndex() + 1);
}

void PlayerCoreTest::libraryShuffleInjectsLibraryTrack()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->setRandomTrackProvider([](int, const QSet<QString> &) {
        return QVector<Track>{makeTrack(QStringLiteral("/lib"))};
    });
    m_core->setShuffleMode(ShuffleMode::Library);
    m_core->setLibraryShufflePercent(100);  // always inject
    m_core->playAt(0);

    // A library injection is the player's own pick, not a user edit: it must
    // announce itself via aboutToInjectLibraryTrack so a playlist-backed queue
    // can keep it queue-only, and must NOT fire the user-add aboutToAddTracks.
    QSignalSpy injected(m_core.get(), &PlayerCore::aboutToInjectLibraryTrack);
    QSignalSpy userAdd(m_core.get(), &PlayerCore::aboutToAddTracks);

    m_core->next();
    QCOMPARE(m_core->queue().size(), 3);
    QCOMPARE(m_core->queue().last().path, QStringLiteral("/lib"));
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/lib"));
    QCOMPARE(m_backend->playedUrls.last(), QUrl::fromLocalFile("/lib"));
    QCOMPARE(injected.count(), 1);
    QCOMPARE(injected.first().first().value<Track>().path, QStringLiteral("/lib"));
    QCOMPARE(userAdd.count(), 0);
}

void PlayerCoreTest::radioShuffleAtPercent100InjectsRadioProviderTrack()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    int radioCalls = 0;
    m_core->setRadioProvider([&radioCalls](int, const QSet<QString> &) {
        ++radioCalls;
        return QVector<Track>{makeTrack(QStringLiteral("/radio"))};
    });
    m_core->setShuffleMode(ShuffleMode::Radio);
    m_core->setRadioShufflePercent(100);
    m_core->playAt(0);

    QSignalSpy injected(m_core.get(), &PlayerCore::aboutToInjectLibraryTrack);
    QSignalSpy userAdd(m_core.get(), &PlayerCore::aboutToAddTracks);

    m_core->next();
    QVERIFY(radioCalls > 0);
    QCOMPARE(m_core->queue().size(), 3);
    QCOMPARE(m_core->queue().last().path, QStringLiteral("/radio"));
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/radio"));
    QCOMPARE(injected.count(), 1);
    QCOMPARE(userAdd.count(), 0);
    QVERIFY(!m_core->radioActive());
}

void PlayerCoreTest::radioShuffleAtPercent0UsesQueueShuffle()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c"}));
    int radioCalls = 0;
    m_core->setRadioProvider([&radioCalls](int, const QSet<QString> &) {
        ++radioCalls;
        return QVector<Track>{makeTrack(QStringLiteral("/radio"))};
    });
    m_core->setShuffleMode(ShuffleMode::Radio);
    m_core->setRadioShufflePercent(0);
    m_core->playAt(0);

    m_core->next();
    QCOMPARE(radioCalls, 0);
    QCOMPARE(m_core->queue().size(), 3);
    QVERIFY(m_core->currentTrack().path == QStringLiteral("/b")
            || m_core->currentTrack().path == QStringLiteral("/c"));
}

void PlayerCoreTest::explicitRadioActiveTakesPrecedenceOverRadioShuffleRoll()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    int radioCalls = 0;
    m_core->setRadioProvider([&radioCalls](int, const QSet<QString> &) {
        ++radioCalls;
        return QVector<Track>{makeTrack(QStringLiteral("/radio"))};
    });
    m_core->setShuffleMode(ShuffleMode::Radio);
    m_core->setRadioShufflePercent(100);
    m_core->setRadioActive(true);
    m_core->playAt(0);

    m_core->next();
    QCOMPARE(m_core->queue().size(), 2);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/b"));
    QVERIFY(m_core->radioActive());
}

void PlayerCoreTest::dsdTakeoverDefersThenStartsNatively()
{
    Track dsd = makeTrack(QStringLiteral("/music/test.dsf"));
    dsd.codec = QStringLiteral("dsf");
    m_core->resetQueue({dsd});
    m_core->setPlaybackStartPlanner([](const Track &track) {
        PlayerCore::PlaybackStartPlan plan;
        if (isDsdTrack(track)) {
            plan.action = PlayerCore::PlaybackStartPlan::Action::DeferForDsdTakeover;
            plan.device = QStringLiteral("hw:3");
        }
        return plan;
    });
    QSignalSpy requested(m_core.get(), &PlayerCore::dsdTakeoverRequested);
    QSignalSpy started(m_core.get(), &PlayerCore::currentTrackChanged);

    m_core->playAt(0);

    QCOMPARE(requested.count(), 1);
    QCOMPARE(started.count(), 0);
    QVERIFY(m_backend->playedUrls.isEmpty());
    QCOMPARE(m_backend->stopCalls, 1);

    m_core->resolveDsdTakeover(true);

    QCOMPARE(started.count(), 1);
    QCOMPARE(m_backend->playedUrls.last(), QUrl::fromLocalFile(dsd.path));
    QCOMPARE(m_backend->outputModes.last(), PlaybackBackend::OutputMode::NativeDsd);
    QCOMPARE(m_backend->outputDevices.last(), QStringLiteral("hw:3"));
}

void PlayerCoreTest::declinedDsdSkipsContiguousBlockUntilPlaybackStarts()
{
    Track first = makeTrack(QStringLiteral("/music/one.dsf"));
    first.codec = QStringLiteral("dsf");
    Track second = makeTrack(QStringLiteral("/music/two.dff"));
    second.codec = QStringLiteral("dff");
    Track pcm = makeTrack(QStringLiteral("/music/three.flac"));
    pcm.codec = QStringLiteral("flac");
    m_core->resetQueue({first, second, pcm});
    m_core->setPlaybackStartPlanner([](const Track &track) {
        PlayerCore::PlaybackStartPlan plan;
        if (isDsdTrack(track)) {
            plan.action = PlayerCore::PlaybackStartPlan::Action::DeferForDsdTakeover;
            plan.device = QStringLiteral("hw:3");
        }
        return plan;
    });
    QSignalSpy requested(m_core.get(), &PlayerCore::dsdTakeoverRequested);
    QSignalSpy skipped(m_core.get(), &PlayerCore::trackStartSkipped);

    m_core->playAt(0);
    m_core->resolveDsdTakeover(false);

    QCOMPARE(requested.count(), 1);
    QCOMPARE(skipped.count(), 2);
    QCOMPARE(m_core->queueIndex(), 2);
    QCOMPARE(m_backend->playedUrls.last(), QUrl::fromLocalFile(pcm.path));
    QCOMPARE(m_backend->outputModes.last(), PlaybackBackend::OutputMode::Normal);

    // The suppression ends only after an actual successful playback transition.
    emit m_backend->stateChanged(PlaybackBackend::State::Playing);
    m_core->playAt(1);
    QCOMPARE(requested.count(), 2);
}

void PlayerCoreTest::declinedDsdUnderRepeatAllStopsInsteadOfLooping()
{
    // Every row is DSD and the takeover is declined, so the block-skip suppresses
    // them all. Repeat All would otherwise hand decideAutoNext() an endless supply
    // of rows; the skip cascade must terminate (and not recurse a frame per row).
    Track a = makeTrack(QStringLiteral("/music/a.dsf"));
    a.codec = QStringLiteral("dsf");
    Track b = makeTrack(QStringLiteral("/music/b.dsf"));
    b.codec = QStringLiteral("dsf");
    m_core->resetQueue({a, b});
    m_core->setRepeatMode(RepeatMode::All);
    m_core->setPlaybackStartPlanner([](const Track &track) {
        PlayerCore::PlaybackStartPlan plan;
        if (isDsdTrack(track)) {
            plan.action = PlayerCore::PlaybackStartPlan::Action::DeferForDsdTakeover;
            plan.device = QStringLiteral("hw:3");
        }
        return plan;
    });

    m_core->playAt(0);
    m_core->resolveDsdTakeover(false);  // must return, not hang or overflow

    QVERIFY(m_backend->playedUrls.isEmpty());
    QVERIFY(m_backend->stopCalls > 0);
}

void PlayerCoreTest::radioMidQueueAdvancesLinearly()
{
    // Radio active but not at the queue end: queued rows keep priority, so a
    // gapless advance plays the plain next row — no provider pull, no injection.
    m_core->resetQueue(makeTracks({"/a", "/b", "/c"}));
    int radioCalls = 0;
    m_core->setRadioProvider([&radioCalls](int, const QSet<QString> &) {
        ++radioCalls;
        return QVector<Track>{makeTrack(QStringLiteral("/radio"))};
    });
    m_core->setRadioActive(true);
    m_core->playAt(0);

    QSignalSpy injected(m_core.get(), &PlayerCore::aboutToInjectLibraryTrack);
    emit m_backend->preparedTrackStarted();

    QCOMPARE(m_core->queueIndex(), 1);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/b"));
    QCOMPARE(m_core->queue().size(), 3);
    QCOMPARE(injected.count(), 0);
    QCOMPARE(radioCalls, 0);
}

void PlayerCoreTest::radioManualAppendMidQueueAdvancesBeforeProvider()
{
    // A user-appended row during active radio is ordinary queue material at
    // enqueue time. Auto-advance consumes it before asking the provider for the
    // next radio pick; provider pulls resume only once the queue end is reached.
    m_core->resetQueue(makeTracks({"/seed"}));
    int radioCalls = 0;
    m_core->setRadioProvider([&radioCalls](int, const QSet<QString> &) {
        ++radioCalls;
        return QVector<Track>{makeTrack(QStringLiteral("/radio"))};
    });
    m_core->setRadioActive(true);
    m_core->playAt(0);
    m_core->appendTracks(makeTracks({"/manual"}));
    radioCalls = 0;

    QSignalSpy injected(m_core.get(), &PlayerCore::aboutToInjectLibraryTrack);
    emit m_backend->finished();

    QCOMPARE(m_core->queueIndex(), 1);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/manual"));
    QCOMPARE(m_core->queue().size(), 2);
    QCOMPARE(injected.count(), 0);

    const int callsBeforeQueueEnd = radioCalls;
    emit m_backend->finished();

    QCOMPARE(m_core->queue().size(), 3);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/radio"));
    QCOMPARE(injected.count(), 1);
    QVERIFY(radioCalls > callsBeforeQueueEnd);
}

void PlayerCoreTest::radioAtEndInjectsProviderPickOnAutoAdvance()
{
    // At the queue end, a natural finish pulls a radio pick and injects it via
    // aboutToInjectLibraryTrack (queue-only semantics), exactly like shuffle.
    m_core->resetQueue(makeTracks({"/a"}));
    m_core->setRadioProvider([](int, const QSet<QString> &) {
        return QVector<Track>{makeTrack(QStringLiteral("/radio"))};
    });
    m_core->setRadioActive(true);
    m_core->playAt(0);

    QSignalSpy injected(m_core.get(), &PlayerCore::aboutToInjectLibraryTrack);
    QSignalSpy userAdd(m_core.get(), &PlayerCore::aboutToAddTracks);
    emit m_backend->finished();

    QCOMPARE(m_core->queue().size(), 2);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/radio"));
    QCOMPARE(m_backend->playedUrls.last(), QUrl::fromLocalFile("/radio"));
    QCOMPARE(injected.count(), 1);
    QCOMPARE(injected.first().first().value<Track>().path, QStringLiteral("/radio"));
    QCOMPARE(userAdd.count(), 0);  // radio picks are never user edits
}

void PlayerCoreTest::radioAtEndInjectsProviderPickOnManualNext()
{
    // Manual Next at the queue end must also route through the radio provider,
    // even with shuffle off (the branch next() would otherwise clamp on).
    m_core->resetQueue(makeTracks({"/a"}));
    m_core->setRadioProvider([](int, const QSet<QString> &) {
        return QVector<Track>{makeTrack(QStringLiteral("/radio"))};
    });
    m_core->setRadioActive(true);
    m_core->playAt(0);

    QSignalSpy injected(m_core.get(), &PlayerCore::aboutToInjectLibraryTrack);
    m_core->next();

    QCOMPARE(m_core->queue().size(), 2);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/radio"));
    QCOMPARE(injected.count(), 1);
}

void PlayerCoreTest::radioEmptyProviderFallsBackToEndOfQueue()
{
    // A provider that yields nothing falls through to the normal end-of-queue
    // behaviour: the pipeline stops, queue position survives.
    m_core->resetQueue(makeTracks({"/a"}));
    m_core->setRadioProvider([](int, const QSet<QString> &) { return QVector<Track>{}; });
    m_core->setRadioActive(true);
    m_core->playAt(0);

    const int stopsBefore = m_backend->stopCalls;
    emit m_backend->finished();

    QCOMPARE(m_backend->stopCalls, stopsBefore + 1);
    QCOMPARE(m_core->queue().size(), 1);
    QCOMPARE(m_core->queueIndex(), 0);
}

void PlayerCoreTest::radioInactiveLeavesShuffleUntouched()
{
    // With radio inactive, a radio provider installed but idle must not interfere
    // with the existing library-shuffle injection path.
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    int radioCalls = 0;
    m_core->setRadioProvider([&radioCalls](int, const QSet<QString> &) {
        ++radioCalls;
        return QVector<Track>{makeTrack(QStringLiteral("/radio"))};
    });
    m_core->setRandomTrackProvider([](int, const QSet<QString> &) {
        return QVector<Track>{makeTrack(QStringLiteral("/lib"))};
    });
    m_core->setShuffleMode(ShuffleMode::Library);
    m_core->setLibraryShufflePercent(100);
    m_core->playAt(0);

    m_core->next();
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/lib"));
    QCOMPARE(radioCalls, 0);
}

QTEST_GUILESS_MAIN(PlayerCoreTest)
#include "test_player_core.moc"
