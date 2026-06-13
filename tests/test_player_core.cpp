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
    State state() const override { return m_state; }
    bool hasSource() const override { return !m_source.isEmpty(); }
    qint64 position() const override { return positionMs; }
    qint64 duration() const override { return durationMs; }

    QVector<QUrl> playedUrls;
    QVector<QUrl> loadedPausedUrls;
    QVector<QUrl> preparedUrls;
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
    void removingCurrentPresentsReplacement();
    void removingLastTrackClearsPlayback();
    void gaplessAdvanceMovesIndexWithoutReveal();
    void finishedAtEndOfQueueStops();
    void explicitJumpCollapsesPlayNext();
    void clearKeepingCurrentKeepsOnlyCurrent();
    void metadataPatchUpdatesRowsWithoutQueueReset();

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

void PlayerCoreTest::removingCurrentPresentsReplacement()
{
    m_core->resetQueue(makeTracks({"/a", "/b", "/c"}));
    m_core->playAt(1);

    QSignalSpy presented(m_core.get(), &PlayerCore::currentTrackChanged);
    m_core->removeRows({1});
    QCOMPARE(m_core->queue().size(), 2);
    QCOMPARE(m_core->queueIndex(), 1);
    QCOMPARE(presented.count(), 1);
    QCOMPARE(presented.last().at(0).value<Track>().path, QStringLiteral("/c"));
    QCOMPARE(presented.last().at(1).toBool(), false);  // no scrobble on re-present
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

QTEST_GUILESS_MAIN(PlayerCoreTest)
#include "test_player_core.moc"
