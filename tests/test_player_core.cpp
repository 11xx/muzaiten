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
    void explicitJumpCollapsesPlayNext();
    void clearKeepingCurrentKeepsOnlyCurrent();
    void metadataPatchUpdatesRowsWithoutQueueReset();
    void missingPatchUpdatesRowsWithoutQueueReset();
    void repeatAllWrapsAtEndOfQueue();
    void repeatAllGaplesslyPreparesFirstTrack();
    void repeatOneReplaysCurrentTrack();
    void shuffleVisitsEveryTrackOnceThenStops();
    void shufflePreviousRetracesHistory();
    void libraryShuffleInjectsLibraryTrack();
    void dsdTakeoverDefersThenStartsNatively();
    void declinedDsdSkipsContiguousBlockUntilPlaybackStarts();

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

void PlayerCoreTest::libraryShuffleInjectsLibraryTrack()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->setRandomTrackProvider([](int, const QSet<QString> &) {
        return QVector<Track>{makeTrack(QStringLiteral("/lib"))};
    });
    m_core->setShuffleMode(ShuffleMode::Library);
    m_core->setLibraryShufflePercent(100);  // always inject
    m_core->playAt(0);

    m_core->next();
    QCOMPARE(m_core->queue().size(), 3);
    QCOMPARE(m_core->queue().last().path, QStringLiteral("/lib"));
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/lib"));
    QCOMPARE(m_backend->playedUrls.last(), QUrl::fromLocalFile("/lib"));
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

QTEST_GUILESS_MAIN(PlayerCoreTest)
#include "test_player_core.moc"
