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
        m_state = loadPausedLeavesStopped ? State::Stopped : State::Paused;
        positionMs = 0;
        loadedPausedUrls.push_back(url);
        loadedPausedPositions.push_back(positionMs);
    }
    void prepareNext(const QUrl &url) override { preparedUrls.push_back(url); }
    void setGaplessStopPending(bool pending) override
    {
        gaplessStopPending = pending;
        gaplessStopPendingHistory.push_back(pending);
        if (pending && commitOnSuppression) {
            preparedUrlAtSuppression = preparedUrls.last();
            commitOnSuppression = false;
            emit preparedTrackStarted();
        }
    }
    void pause() override
    {
        m_state = State::Paused;
        ++pauseCalls;
    }
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
    void fail() { m_state = State::Error; }
    void onGaplessTrackAdvanced() override
    {
        if (!preparedUrls.isEmpty()) {
            m_source = preparedUrls.last();
        }
    }
    QUrl currentSource() const { return m_source; }
    void stabilizeGaplessHandoff() override
    {
        ++stabilizeCalls;
        if (commitOnStabilize) {
            commitOnStabilize = false;
            emit preparedTrackStarted();
        }
    }

    QVector<QUrl> playedUrls;
    QVector<QUrl> loadedPausedUrls;
    QVector<qint64> loadedPausedPositions;
    QVector<QUrl> preparedUrls;
    QUrl preparedUrlAtSuppression;
    QVector<OutputMode> outputModes;
    QVector<QString> outputDevices;
    qint64 lastSeekMs = -1;
    double lastVolume = -1.0;
    qint64 positionMs = 0;
    qint64 durationMs = 0;
    int stopCalls = 0;
    int pauseCalls = 0;
    int stabilizeCalls = 0;
    bool gaplessStopPending = false;
    QVector<bool> gaplessStopPendingHistory;
    bool commitOnStabilize = false;
    bool commitOnSuppression = false;
    bool loadPausedLeavesStopped = false;

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
    void queueMutationStabilizesAudibleSuccessorBeforeReordering();
    void removingCurrentAdvancesPlayback();
    void removingCurrentWhilePausedStaysPausedOnNext();
    void removingLastTrackClearsPlayback();
    void removalFiltersInvalidRowsAndMarksSuccessorAutomatic();
    void removingOtherRowDoesNotRepresentCurrentTrack();
    void clearKeepingCurrentDoesNotRepresentCurrentTrack();
    void unresolvableTrackSkipsWithoutLeavingOldAudio();
    void allUnresolvableRepeatQueueStops();
    void nextAtEndDoesNotRestartCurrent();
    void playControlsRestartCurrentAfterBackendError();
    void gaplessAdvanceMovesIndexWithoutReveal();
    void finishedAtEndOfQueueStops();
    void trackFinishedEmittedOnBackendFinish();
    void trackFinishedEmittedOnGaplessAdvance();
    void backendFinishMarksNonGaplessAdvanceAutomatic();
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
    void linearPreviousDoesNotBadgeDepartedRowAsPlayNext();
    void previousAndDirectJumpEmitNavigationSignal();
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
    void stopAfterStartsUnarmed();
    void stopAfterDeadlineExpiresAfterShortDuration();
    void stopAfterDeadlineArmsWhilePlaying();
    void stopAfterDeadlineArmsWhilePaused();
    void stopAfterDeadlineDisarmsAtQueueExhaustion();
    void stopAfterPauseDoesNotDisarm();
    void stopAfterReplacementAfterFinalConditionDoesNotTrigger();
    void stopAfterReplacementAndCancellationRestorePreparation();
    void stopAfterManualStopDisarms();
    void stopAfterManualNavigationDoesNotDecrement();
    void stopAfterBackendFinishConsumesOnce();
    void stopAfterGaplessAdvanceConsumesOnce();
    void stopAfterFinalGaplessAdvancePausesActualRow();
    void stagedStartResumesStoppedSourceViaToggle();
    void stagedStartSurvivesQueuePreservingMutations();
    void resetQueueChangedCurrentClearsStagedStart();
    void queueResetEmitsOnceOnlyForReset();
    void stopAfterOrdinaryPauseResumeDoesNotDuplicateNotification();
    void stopAfterManualNavigationClearsStagedNotification();
    void stopAfterLateSuppressionCommitsPreparedShuffleRow();
    void prepareNextKeepsNestedPreparationTarget();
    void stopAfterRepeatOneCountsCompletions();
    void stopAfterFinalRepeatOneStagesCurrentRow();
    void stopAfterFinalCompletionStagesExistingRow();
    void stopAfterFinalCompletionRejectsUnresolvableCandidate();
    void stopAfterFinalCompletionRejectsDsdPredecessor();
    void stopAfterFinalCompletionRejectsDsdCandidate();
    void stopAfterFinalCompletionRejectsSkipPlan();
    void stopAfterFinalCompletionRejectsDsdTakeoverPlan();
    void stopAfterJitRadioLeavesQueueStopped();
    void stopAfterExhaustionDisarms();
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

void PlayerCoreTest::queueMutationStabilizesAudibleSuccessorBeforeReordering()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c"}));
    m_core->playAt(0);
    m_backend->commitOnStabilize = true;

    m_core->playTracksNext(makeTracks({"/x"}));

    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/b"));
    QCOMPARE(m_core->queueIndex(), 1);
    QCOMPARE(m_core->queue().at(2).path, QStringLiteral("/x"));
    QVERIFY(m_backend->stabilizeCalls > 0);
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

void PlayerCoreTest::removalFiltersInvalidRowsAndMarksSuccessorAutomatic()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c"}));
    m_core->playAt(1);
    QSignalSpy indexChanged(m_core.get(), &PlayerCore::currentIndexChanged);

    m_core->removeRows({-1, 1, 99});

    QCOMPARE(m_core->queue().size(), 2);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/c"));
    QCOMPARE(indexChanged.count(), 1);
    QCOMPARE(indexChanged.first().at(0).toInt(), 1);
    QCOMPARE(indexChanged.first().at(1).toBool(), false);
}

void PlayerCoreTest::removingOtherRowDoesNotRepresentCurrentTrack()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c"}));
    m_core->playAt(1);
    QSignalSpy represented(m_core.get(), &PlayerCore::currentTrackChanged);

    m_core->removeRows({0});

    QCOMPARE(m_core->queueIndex(), 0);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/b"));
    QCOMPARE(represented.count(), 0);
}

void PlayerCoreTest::clearKeepingCurrentDoesNotRepresentCurrentTrack()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c"}));
    m_core->playAt(1);
    QSignalSpy represented(m_core.get(), &PlayerCore::currentTrackChanged);

    m_core->clearKeepingCurrent();

    QCOMPARE(m_core->queue().size(), 1);
    QCOMPARE(m_core->queue().first().path, QStringLiteral("/b"));
    QCOMPARE(m_core->queueIndex(), 0);
    QCOMPARE(represented.count(), 0);
}

void PlayerCoreTest::unresolvableTrackSkipsWithoutLeavingOldAudio()
{
    m_core->resetQueue(makeTracks({"/a", "/missing", "/c"}));
    m_core->setPathResolver([](const Track &track) {
        return track.path == QLatin1String("/missing") ? QString() : track.path;
    });
    m_core->playAt(0);

    QSignalSpy unresolvable(m_core.get(), &PlayerCore::trackUnresolvable);
    m_core->playAt(1);

    QCOMPARE(unresolvable.count(), 1);
    QCOMPARE(unresolvable.first().at(0).value<Track>().path, QStringLiteral("/missing"));
    QCOMPARE(m_core->queueIndex(), 2);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/c"));
    QCOMPARE(m_backend->playedUrls.last(), QUrl::fromLocalFile("/c"));
}

void PlayerCoreTest::allUnresolvableRepeatQueueStops()
{
    m_core->resetQueue(makeTracks({"/missing-a", "/missing-b"}));
    m_core->setPathResolver([](const Track &) { return QString(); });
    m_core->setRepeatMode(RepeatMode::All);

    m_core->playAt(0);

    QVERIFY(m_backend->playedUrls.isEmpty());
    QCOMPARE(m_backend->state(), PlaybackBackend::State::Stopped);
    QCOMPARE(m_backend->stopCalls, 1);
}

void PlayerCoreTest::nextAtEndDoesNotRestartCurrent()
{
    m_core->resetQueue(makeTracks({"/only"}));
    m_core->playAt(0);
    const qsizetype playsBefore = m_backend->playedUrls.size();

    m_core->next();

    QCOMPARE(m_backend->playedUrls.size(), playsBefore);
    QCOMPARE(m_core->queueIndex(), 0);
}

void PlayerCoreTest::playControlsRestartCurrentAfterBackendError()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->playAt(0);
    QCOMPARE(m_backend->playedUrls.size(), 1);

    m_backend->fail();
    m_core->play();
    QCOMPARE(m_backend->playedUrls.size(), 2);
    QCOMPARE(m_backend->playedUrls.last(), QUrl::fromLocalFile("/a"));

    m_backend->fail();
    m_core->togglePlayPause();
    QCOMPARE(m_backend->playedUrls.size(), 3);
    QCOMPARE(m_backend->playedUrls.last(), QUrl::fromLocalFile("/a"));
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

void PlayerCoreTest::backendFinishMarksNonGaplessAdvanceAutomatic()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->playAt(0);
    QSignalSpy indexChanged(m_core.get(), &PlayerCore::currentIndexChanged);

    emit m_backend->finished();

    QCOMPARE(indexChanged.count(), 1);
    QCOMPARE(indexChanged.first().at(0).toInt(), 1);
    QCOMPARE(indexChanged.first().at(1).toBool(), false);

    indexChanged.clear();
    m_core->next();
    QCOMPARE(indexChanged.count(), 0); // final-row Next is a no-op

    m_core->playAt(0);
    indexChanged.clear();
    m_core->next();
    QCOMPARE(indexChanged.count(), 1);
    QCOMPARE(indexChanged.first().at(0).toInt(), 1);
    QCOMPARE(indexChanged.first().at(1).toBool(), true);
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

void PlayerCoreTest::linearPreviousDoesNotBadgeDepartedRowAsPlayNext()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c"}));
    m_core->playAt(1);  // playing /b, empty play-next region ends at row 2
    QCOMPARE(m_core->playNextInsertIndex(), 2);

    // Stepping back to /a used to keep the old boundary (2), turning the just
    // departed /b — region [1, 2) — into a spurious "play next" badge.
    m_core->previous();
    QCOMPARE(m_core->queueIndex(), 0);
    QCOMPARE(m_core->playNextInsertIndex(), m_core->queueIndex() + 1);

    // Same with a real pending play-next batch: Previous is a manual move, so
    // like an explicit jump it clears the batch instead of mis-spanning it.
    m_core->playAt(1);
    m_core->playTracksNext(makeTracks({"/x"}));
    QCOMPARE(m_core->playNextInsertIndex(), 3);
    m_core->previous();
    QCOMPARE(m_core->queueIndex(), 0);
    QCOMPARE(m_core->playNextInsertIndex(), m_core->queueIndex() + 1);
}

void PlayerCoreTest::previousAndDirectJumpEmitNavigationSignal()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->playAt(1);

    // The signal fires before the outgoing track's play event is finalized so
    // telemetry can classify Back as navigation rather than rejection.
    QSignalSpy navigation(m_core.get(), &PlayerCore::aboutToNavigateWithoutRejecting);
    m_core->previous();
    QCOMPARE(navigation.count(), 1);
    m_core->next();
    QCOMPARE(navigation.count(), 1);
    m_core->playAt(1, true, false, /*explicitJump=*/true);
    QCOMPARE(navigation.count(), 2);
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

void PlayerCoreTest::stopAfterStartsUnarmed()
{
    const PlayerCore::StopAfterStatus status = m_core->stopAfterStatus();
    QCOMPARE(status.mode, PlayerCore::StopAfterMode::None);
    QCOMPARE(status.remainingMs, 0);
    QCOMPARE(status.remainingCompletions, 0);
}

void PlayerCoreTest::stopAfterDeadlineExpiresAfterShortDuration()
{
    m_core->resetQueue(makeTracks({"/a"}));
    m_core->playAt(0);
    m_backend->positionMs = 321;
    QSignalSpy triggered(m_core.get(), &PlayerCore::stopAfterTriggered);

    m_core->armStopAfterDuration(std::chrono::milliseconds{1100});
    QTest::qWait(100);
    QCOMPARE(triggered.count(), 0);

    QTRY_COMPARE_WITH_TIMEOUT(triggered.count(), 1, 3000);
    QCOMPARE(m_core->stopAfterStatus().mode, PlayerCore::StopAfterMode::None);
    QCOMPARE(m_backend->pauseCalls, 1);
    QCOMPARE(m_backend->state(), PlaybackBackend::State::Paused);
    QVERIFY(m_backend->hasSource());
    QCOMPARE(m_backend->position(), 321);
}

void PlayerCoreTest::stopAfterDeadlineArmsWhilePlaying()
{
    m_core->resetQueue(makeTracks({"/a"}));
    m_core->playAt(0);

    m_core->armStopAfterMinutes(0);

    QCOMPARE(m_core->stopAfterStatus().mode, PlayerCore::StopAfterMode::Deadline);
    QVERIFY(m_core->stopAfterStatus().remainingMs >= 59000);
    QVERIFY(m_backend->gaplessStopPending == false);
}

void PlayerCoreTest::stopAfterDeadlineArmsWhilePaused()
{
    m_core->resetQueue(makeTracks({"/a"}));
    m_core->playAt(0);
    m_core->togglePlayPause();

    m_core->armStopAfterMinutes(1441);

    QCOMPARE(m_core->stopAfterStatus().mode, PlayerCore::StopAfterMode::Deadline);
    QVERIFY(m_core->stopAfterStatus().remainingMs <= 1440 * 60 * 1000);
    QVERIFY(m_core->stopAfterStatus().remainingMs >= 1439 * 60 * 1000);
}

void PlayerCoreTest::stopAfterDeadlineDisarmsAtQueueExhaustion()
{
    m_core->resetQueue(makeTracks({"/a"}));
    m_core->playAt(0);
    m_core->armStopAfterMinutes(1);
    const int stopsBefore = m_backend->stopCalls;

    emit m_backend->finished();

    QCOMPARE(m_core->stopAfterStatus().mode, PlayerCore::StopAfterMode::None);
    QCOMPARE(m_backend->stopCalls, stopsBefore + 1);
}

void PlayerCoreTest::stopAfterPauseDoesNotDisarm()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->playAt(0);
    m_core->armStopAfterCompletions(2);

    m_core->togglePlayPause();

    const PlayerCore::StopAfterStatus status = m_core->stopAfterStatus();
    QCOMPARE(status.mode, PlayerCore::StopAfterMode::NaturalCompletions);
    QCOMPARE(status.remainingCompletions, 2);
}

void PlayerCoreTest::stopAfterReplacementAfterFinalConditionDoesNotTrigger()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->playAt(0);
    m_core->armStopAfterCompletions(1);
    m_backend->commitOnStabilize = true;
    QSignalSpy triggered(m_core.get(), &PlayerCore::stopAfterTriggered);

    m_core->armStopAfterCompletions(3);

    QCOMPARE(triggered.count(), 0);
    QCOMPARE(m_core->stopAfterStatus().mode, PlayerCore::StopAfterMode::NaturalCompletions);
    QCOMPARE(m_core->stopAfterStatus().remainingCompletions, 3);
    QCOMPARE(m_core->queueIndex(), 1);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/b"));
    QCOMPARE(m_backend->state(), PlaybackBackend::State::Playing);
}

void PlayerCoreTest::stopAfterReplacementAndCancellationRestorePreparation()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->playAt(0);
    m_core->armStopAfterCompletions(1);
    QVERIFY(m_backend->gaplessStopPending);
    QCOMPARE(m_backend->preparedUrls.last(), QUrl());

    m_core->armStopAfterMinutes(1);
    QCOMPARE(m_core->stopAfterStatus().mode, PlayerCore::StopAfterMode::Deadline);
    QVERIFY(!m_backend->gaplessStopPending);
    QCOMPARE(m_backend->preparedUrls.last(), QUrl::fromLocalFile("/b"));

    m_core->cancelStopAfter();
    QCOMPARE(m_core->stopAfterStatus().mode, PlayerCore::StopAfterMode::None);
    QVERIFY(!m_backend->gaplessStopPending);
    QCOMPARE(m_backend->preparedUrls.last(), QUrl::fromLocalFile("/b"));
}

void PlayerCoreTest::stopAfterManualStopDisarms()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->playAt(0);
    m_core->armStopAfterCompletions(2);
    const int stopsBefore = m_backend->stopCalls;

    m_core->stop();

    QCOMPARE(m_core->stopAfterStatus().mode, PlayerCore::StopAfterMode::None);
    QCOMPARE(m_backend->stopCalls, stopsBefore + 1);
    QVERIFY(!m_backend->hasSource());
    QVERIFY(!m_backend->gaplessStopPending);
}

void PlayerCoreTest::stopAfterManualNavigationDoesNotDecrement()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c"}));
    m_core->playAt(0);
    m_core->armStopAfterCompletions(3);

    m_core->next();
    QCOMPARE(m_core->stopAfterStatus().remainingCompletions, 3);
    m_core->previous();
    QCOMPARE(m_core->stopAfterStatus().remainingCompletions, 3);
    m_core->playAt(2, true, false, /*explicitJump=*/true);
    QCOMPARE(m_core->stopAfterStatus().remainingCompletions, 3);

    m_core->removeRows({1});
    QCOMPARE(m_core->stopAfterStatus().remainingCompletions, 3);
}

void PlayerCoreTest::stopAfterBackendFinishConsumesOnce()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c"}));
    m_core->playAt(0);
    m_core->armStopAfterCompletions(2);

    emit m_backend->finished();

    QCOMPARE(m_core->stopAfterStatus().mode, PlayerCore::StopAfterMode::NaturalCompletions);
    QCOMPARE(m_core->stopAfterStatus().remainingCompletions, 1);
    QCOMPARE(m_core->queueIndex(), 1);
}

void PlayerCoreTest::stopAfterGaplessAdvanceConsumesOnce()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c"}));
    m_core->playAt(0);
    m_core->armStopAfterCompletions(2);

    emit m_backend->preparedTrackStarted();

    QCOMPARE(m_core->stopAfterStatus().mode, PlayerCore::StopAfterMode::NaturalCompletions);
    QCOMPARE(m_core->stopAfterStatus().remainingCompletions, 1);
    QCOMPARE(m_core->queueIndex(), 1);
    QVERIFY(m_backend->gaplessStopPending);
}

void PlayerCoreTest::stopAfterFinalGaplessAdvancePausesActualRow()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c"}));
    m_core->playAt(0);
    QSignalSpy currentChanged(m_core.get(), &PlayerCore::currentTrackChanged);
    QSignalSpy triggered(m_core.get(), &PlayerCore::stopAfterTriggered);

    m_core->armStopAfterCompletions(1);
    emit m_backend->preparedTrackStarted();

    QCOMPARE(m_core->stopAfterStatus().mode, PlayerCore::StopAfterMode::None);
    QCOMPARE(m_core->queueIndex(), 1);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/b"));
    QCOMPARE(m_backend->state(), PlaybackBackend::State::Paused);
    QCOMPARE(m_backend->pauseCalls, 1);
    QCOMPARE(triggered.count(), 1);
    QCOMPARE(m_backend->preparedUrls.last(), QUrl::fromLocalFile("/c"));
    QCOMPARE(currentChanged.last().at(0).value<Track>().path, QStringLiteral("/b"));
    QCOMPARE(currentChanged.last().at(1).toBool(), false);

    currentChanged.clear();
    m_core->play();

    QCOMPARE(currentChanged.count(), 1);
    QCOMPARE(currentChanged.last().at(0).value<Track>().path, QStringLiteral("/b"));
    QCOMPARE(currentChanged.last().at(1).toBool(), true);
    QCOMPARE(m_backend->state(), PlaybackBackend::State::Playing);
}

void PlayerCoreTest::stagedStartResumesStoppedSourceViaToggle()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->playAt(0);
    m_backend->loadPausedLeavesStopped = true;
    QSignalSpy currentChanged(m_core.get(), &PlayerCore::currentTrackChanged);

    m_core->armStopAfterCompletions(1);
    emit m_backend->finished();

    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/b"));
    QCOMPARE(m_backend->state(), PlaybackBackend::State::Stopped);
    QCOMPARE(currentChanged.last().at(1).toBool(), false);

    currentChanged.clear();
    m_core->togglePlayPause();

    QCOMPARE(currentChanged.count(), 1);
    QCOMPARE(currentChanged.last().at(0).value<Track>().path, QStringLiteral("/b"));
    QCOMPARE(currentChanged.last().at(1).toBool(), true);
    QCOMPARE(m_backend->state(), PlaybackBackend::State::Playing);
}

void PlayerCoreTest::stagedStartSurvivesQueuePreservingMutations()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->playAt(0);
    QSignalSpy currentChanged(m_core.get(), &PlayerCore::currentTrackChanged);

    m_core->armStopAfterCompletions(1);
    emit m_backend->finished();
    currentChanged.clear();

    m_core->clearKeepingCurrent();
    m_core->play();

    QCOMPARE(currentChanged.count(), 1);
    QCOMPARE(currentChanged.last().at(0).value<Track>().path, QStringLiteral("/b"));
    QCOMPARE(currentChanged.last().at(1).toBool(), true);

    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->playAt(0);
    m_core->armStopAfterCompletions(1);
    emit m_backend->finished();
    currentChanged.clear();

    m_core->resetQueue(makeTracks({"/a", "/b"}), 1, 2);
    m_core->play();

    QCOMPARE(currentChanged.count(), 1);
    QCOMPARE(currentChanged.last().at(0).value<Track>().path, QStringLiteral("/b"));
    QCOMPARE(currentChanged.last().at(1).toBool(), true);
}

void PlayerCoreTest::resetQueueChangedCurrentClearsStagedStart()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->playAt(0);
    QSignalSpy currentChanged(m_core.get(), &PlayerCore::currentTrackChanged);

    m_core->armStopAfterCompletions(1);
    emit m_backend->finished();
    currentChanged.clear();

    m_core->resetQueue(makeTracks({"/a", "/c"}), 1, 2);
    m_core->play();

    QCOMPARE(currentChanged.count(), 0);
    QCOMPARE(m_backend->state(), PlaybackBackend::State::Playing);
}

void PlayerCoreTest::queueResetEmitsOnceOnlyForReset()
{
    QSignalSpy resetSpy(m_core.get(), &PlayerCore::queueReset);
    QSignalSpy changedSpy(m_core.get(), &PlayerCore::queueChanged);

    m_core->resetQueue(makeTracks({QStringLiteral("/a")}));
    QCOMPARE(resetSpy.count(), 1);
    QCOMPARE(changedSpy.count(), 0);

    m_core->appendTracks(makeTracks({QStringLiteral("/b")}));
    QCOMPARE(resetSpy.count(), 1);
    QCOMPARE(changedSpy.count(), 1);

    m_core->clearAll();
    QCOMPARE(resetSpy.count(), 1);
    QCOMPARE(changedSpy.count(), 2);

    m_core->resetQueue(makeTracks({QStringLiteral("/c")}));
    QCOMPARE(resetSpy.count(), 2);
}

void PlayerCoreTest::stopAfterOrdinaryPauseResumeDoesNotDuplicateNotification()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->playAt(0);
    QSignalSpy currentChanged(m_core.get(), &PlayerCore::currentTrackChanged);

    m_core->togglePlayPause();
    m_core->togglePlayPause();

    QCOMPARE(currentChanged.count(), 0);
    QCOMPARE(m_backend->state(), PlaybackBackend::State::Playing);
}

void PlayerCoreTest::stopAfterManualNavigationClearsStagedNotification()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c"}));
    m_core->playAt(0);
    QSignalSpy currentChanged(m_core.get(), &PlayerCore::currentTrackChanged);

    m_core->armStopAfterCompletions(1);
    emit m_backend->finished();

    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/b"));
    QCOMPARE(currentChanged.last().at(1).toBool(), false);

    currentChanged.clear();
    m_core->playAt(2, true, false, /*explicitJump=*/true);
    QCOMPARE(currentChanged.count(), 1);
    QCOMPARE(currentChanged.last().at(0).value<Track>().path, QStringLiteral("/c"));
    QCOMPARE(currentChanged.last().at(1).toBool(), true);

    m_core->play();
    QCOMPARE(currentChanged.count(), 1);
}

void PlayerCoreTest::prepareNextKeepsNestedPreparationTarget()
{
    bool reentered = false;
    m_core->setRadioActive(true);
    m_core->setRadioProvider([&reentered, this](int, const QSet<QString> &) {
        if (!reentered) {
            reentered = true;
            m_core->resetQueue(makeTracks({"/a", "/b"}), 0, 1);
            m_core->prepareNext();
            return QVector<Track>{makeTrack(QStringLiteral("/stale"))};
        }
        return QVector<Track>{};
    });
    m_core->resetQueue(makeTracks({"/a"}), 0, 1);
    m_core->playAt(0);

    QVERIFY(reentered);
    QCOMPARE(m_core->queueIndex(), 0);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/a"));
    QCOMPARE(m_backend->currentSource(), QUrl::fromLocalFile("/a"));
    QCOMPARE(m_backend->preparedUrls.last(), QUrl::fromLocalFile("/b"));

    emit m_backend->preparedTrackStarted();

    QCOMPARE(m_core->queueIndex(), 1);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/b"));
    QCOMPARE(m_backend->currentSource(), QUrl::fromLocalFile("/b"));
}

void PlayerCoreTest::stopAfterLateSuppressionCommitsPreparedShuffleRow()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c"}));
    m_core->setShuffleMode(ShuffleMode::Queue);
    m_core->playAt(2);
    m_backend->commitOnSuppression = true;
    QSignalSpy triggered(m_core.get(), &PlayerCore::stopAfterTriggered);

    m_core->armStopAfterCompletions(1);

    QCOMPARE(triggered.count(), 1);
    QCOMPARE(m_core->stopAfterStatus().mode, PlayerCore::StopAfterMode::None);
    QVERIFY(!m_backend->preparedUrlAtSuppression.isEmpty());
    QCOMPARE(m_core->currentTrack().path, m_backend->preparedUrlAtSuppression.toLocalFile());
    const QString restoredPath = m_backend->preparedUrlAtSuppression.toLocalFile() == QStringLiteral("/a")
        ? QStringLiteral("/b") : QStringLiteral("/a");
    QCOMPARE(m_backend->preparedUrls.last(), QUrl::fromLocalFile(restoredPath));
    QCOMPARE(m_backend->state(), PlaybackBackend::State::Paused);
}

void PlayerCoreTest::stopAfterRepeatOneCountsCompletions()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->setRepeatMode(RepeatMode::One);
    m_core->playAt(0);
    m_core->armStopAfterCompletions(2);
    const int playsBefore = m_backend->playedUrls.size();

    emit m_backend->finished();
    QCOMPARE(m_core->stopAfterStatus().remainingCompletions, 1);
    QCOMPARE(m_backend->playedUrls.size(), playsBefore + 1);

    emit m_backend->finished();
    QCOMPARE(m_core->stopAfterStatus().mode, PlayerCore::StopAfterMode::None);
    QCOMPARE(m_backend->playedUrls.size(), playsBefore + 1);
}

void PlayerCoreTest::stopAfterFinalRepeatOneStagesCurrentRow()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->setRepeatMode(RepeatMode::One);
    m_core->playAt(0);
    QSignalSpy triggered(m_core.get(), &PlayerCore::stopAfterTriggered);
    const int stopsBefore = m_backend->stopCalls;

    m_core->armStopAfterCompletions(1);
    emit m_backend->finished();

    QCOMPARE(triggered.count(), 1);
    QCOMPARE(m_core->stopAfterStatus().mode, PlayerCore::StopAfterMode::None);
    QCOMPARE(m_core->queueIndex(), 0);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/a"));
    QCOMPARE(m_backend->loadedPausedUrls.last(), QUrl::fromLocalFile("/a"));
    QCOMPARE(m_backend->loadedPausedPositions.last(), 0);
    QCOMPARE(m_backend->state(), PlaybackBackend::State::Paused);
    QCOMPARE(m_backend->stopCalls, stopsBefore);
}

void PlayerCoreTest::stopAfterFinalCompletionStagesExistingRow()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c"}));
    m_core->setRadioActive(true);
    m_core->playAt(0);
    QStringList queueBefore;
    for (const Track &track : m_core->queue()) {
        queueBefore.push_back(track.path);
    }
    QSignalSpy currentChanged(m_core.get(), &PlayerCore::currentTrackChanged);
    const int outputModesBefore = m_backend->outputModes.size();

    m_core->armStopAfterCompletions(1);
    emit m_backend->finished();

    QCOMPARE(m_core->stopAfterStatus().mode, PlayerCore::StopAfterMode::None);
    QStringList queueAfter;
    for (const Track &track : m_core->queue()) {
        queueAfter.push_back(track.path);
    }
    QCOMPARE(queueAfter, queueBefore);
    QCOMPARE(m_core->queueIndex(), 1);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/b"));
    QCOMPARE(m_backend->loadedPausedUrls.last(), QUrl::fromLocalFile("/b"));
    QCOMPARE(m_backend->loadedPausedPositions.last(), 0);
    QCOMPARE(m_backend->preparedUrls.last(), QUrl::fromLocalFile("/c"));
    QCOMPARE(m_backend->outputModes.size(), outputModesBefore + 1);
    QCOMPARE(m_backend->outputModes.last(), PlaybackBackend::OutputMode::Normal);
    QCOMPARE(m_backend->state(), PlaybackBackend::State::Paused);
    QCOMPARE(currentChanged.last().at(0).value<Track>().path, QStringLiteral("/b"));
    QVERIFY(!currentChanged.last().at(1).toBool());

    currentChanged.clear();
    m_core->play();

    QCOMPARE(currentChanged.count(), 1);
    QCOMPARE(currentChanged.last().at(0).value<Track>().path, QStringLiteral("/b"));
    QVERIFY(currentChanged.last().at(1).toBool());
    QCOMPARE(m_backend->state(), PlaybackBackend::State::Playing);
    QVERIFY(m_core->radioActive());
}

void PlayerCoreTest::stopAfterFinalCompletionRejectsUnresolvableCandidate()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->setPathResolver([](const Track &track) {
        return track.path == QStringLiteral("/b") ? QString() : track.path;
    });
    m_core->playAt(0);
    const int stopsBefore = m_backend->stopCalls;

    m_core->armStopAfterCompletions(1);
    emit m_backend->finished();

    QCOMPARE(m_core->queueIndex(), 0);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/a"));
    QCOMPARE(m_core->queue().size(), 2);
    QCOMPARE(m_backend->stopCalls, stopsBefore + 1);
    QVERIFY(m_backend->loadedPausedUrls.isEmpty());
}

void PlayerCoreTest::stopAfterFinalCompletionRejectsDsdPredecessor()
{
    Track predecessor = makeTrack(QStringLiteral("/a.dsf"));
    predecessor.codec = QStringLiteral("dsf");
    m_core->resetQueue({predecessor, makeTrack(QStringLiteral("/b"))});
    m_core->playAt(0);
    const int stopsBefore = m_backend->stopCalls;

    m_core->armStopAfterCompletions(1);
    emit m_backend->finished();

    QCOMPARE(m_core->queueIndex(), 0);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/a.dsf"));
    QCOMPARE(m_backend->stopCalls, stopsBefore + 1);
    QVERIFY(m_backend->loadedPausedUrls.isEmpty());
}

void PlayerCoreTest::stopAfterFinalCompletionRejectsDsdCandidate()
{
    Track candidate = makeTrack(QStringLiteral("/b.dsf"));
    candidate.codec = QStringLiteral("dsf");
    m_core->resetQueue({makeTrack(QStringLiteral("/a")), candidate});
    m_core->playAt(0);
    const int stopsBefore = m_backend->stopCalls;

    m_core->armStopAfterCompletions(1);
    emit m_backend->finished();

    QCOMPARE(m_core->queueIndex(), 0);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/a"));
    QCOMPARE(m_backend->stopCalls, stopsBefore + 1);
    QVERIFY(m_backend->loadedPausedUrls.isEmpty());
}

void PlayerCoreTest::stopAfterFinalCompletionRejectsSkipPlan()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->setPlaybackStartPlanner([](const Track &track) {
        PlayerCore::PlaybackStartPlan plan;
        if (track.path == QStringLiteral("/b")) {
            plan.action = PlayerCore::PlaybackStartPlan::Action::Skip;
        }
        return plan;
    });
    m_core->playAt(0);
    const int stopsBefore = m_backend->stopCalls;

    m_core->armStopAfterCompletions(1);
    emit m_backend->finished();

    QCOMPARE(m_core->queueIndex(), 0);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/a"));
    QCOMPARE(m_backend->stopCalls, stopsBefore + 1);
    QVERIFY(m_backend->loadedPausedUrls.isEmpty());
}

void PlayerCoreTest::stopAfterFinalCompletionRejectsDsdTakeoverPlan()
{
    m_core->resetQueue(makeTracks({"/a", "/b"}));
    m_core->setPlaybackStartPlanner([](const Track &track) {
        PlayerCore::PlaybackStartPlan plan;
        if (track.path == QStringLiteral("/b")) {
            plan.action = PlayerCore::PlaybackStartPlan::Action::DeferForDsdTakeover;
            plan.device = QStringLiteral("hw:3");
        }
        return plan;
    });
    m_core->playAt(0);
    QSignalSpy requested(m_core.get(), &PlayerCore::dsdTakeoverRequested);
    const int stopsBefore = m_backend->stopCalls;

    m_core->armStopAfterCompletions(1);
    emit m_backend->finished();

    QCOMPARE(m_core->queueIndex(), 0);
    QCOMPARE(m_core->currentTrack().path, QStringLiteral("/a"));
    QCOMPARE(m_backend->stopCalls, stopsBefore + 1);
    QVERIFY(m_backend->loadedPausedUrls.isEmpty());
    QCOMPARE(requested.count(), 0);
}

void PlayerCoreTest::stopAfterJitRadioLeavesQueueStopped()
{
    m_core->resetQueue(makeTracks({"/a"}));
    m_core->setRadioProvider([](int, const QSet<QString> &) {
        return QVector<Track>{makeTrack(QStringLiteral("/radio"))};
    });
    m_core->setRadioActive(true);
    m_core->playAt(0);
    m_core->armStopAfterCompletions(1);
    const int stopsBefore = m_backend->stopCalls;
    const int queueSizeBefore = m_core->queue().size();

    emit m_backend->finished();

    QCOMPARE(m_core->stopAfterStatus().mode, PlayerCore::StopAfterMode::None);
    QCOMPARE(m_core->queue().size(), queueSizeBefore);
    QCOMPARE(m_core->queueIndex(), 0);
    QVERIFY(m_core->radioActive());
    QCOMPARE(m_backend->stopCalls, stopsBefore + 1);
    QVERIFY(!m_backend->hasSource());
}

void PlayerCoreTest::stopAfterExhaustionDisarms()
{
    m_core->resetQueue(makeTracks({"/a"}));
    m_core->playAt(0);
    m_core->armStopAfterCompletions(1);
    const int stopsBefore = m_backend->stopCalls;

    emit m_backend->finished();

    QCOMPARE(m_core->stopAfterStatus().mode, PlayerCore::StopAfterMode::None);
    QCOMPARE(m_core->queueIndex(), 0);
    QCOMPARE(m_backend->stopCalls, stopsBefore + 1);
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
