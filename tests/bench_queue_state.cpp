// Queue-state save benchmark (dev tool; built by default, deliberately NOT
// registered with CTest). Timings are a product-decision artifact, not a gate.
//
// Builds a queue of N synthetic tracks inside a throwaway state root, saves it
// once, then moves the cursor M times with a save after each move. Reports the
// structural save, the total and mean cost of the cursor saves, and the size of
// the persisted documents. Never touches the default XDG paths and never starts
// playback.
//
//   bench_queue_state [tracks=5000] [moves=100]

#include <QApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>

#define private public
#include "app/AppCore.h"
#include "player/PlayerCore.h"
#include "ui/MainWindow.h"
#undef private
#include "core/Track.h"
#include "db/SettingsStore.h"

#include <cstdio>
#include <cstdlib>

namespace {

QVector<Track> syntheticQueue(int count)
{
    QVector<Track> tracks;
    tracks.reserve(count);
    for (int i = 0; i < count; ++i) {
        Track track;
        track.path = QStringLiteral("/bench/queue/album-%1/%2 - track.flac").arg(i / 12).arg(i % 12 + 1, 2, 10, QLatin1Char('0'));
        track.parentDir = QStringLiteral("/bench/queue/album-%1").arg(i / 12);
        track.filename = QStringLiteral("%1 - track.flac").arg(i % 12 + 1, 2, 10, QLatin1Char('0'));
        track.title = QStringLiteral("Synthetic track %1").arg(i);
        track.artistName = QStringLiteral("Artist %1").arg(i % 97);
        track.albumArtistName = track.artistName;
        track.albumTitle = QStringLiteral("Album %1").arg(i / 12);
        track.date = QStringLiteral("2001");
        track.originalDate = track.date;
        track.trackNumber = i % 12 + 1;
        track.discNumber = 1;
        track.durationMs = 200'000 + (i % 60) * 1000;
        track.fileSize = 30'000'000 + i;
        track.codec = QStringLiteral("flac");
        track.sampleRateHz = 44'100;
        track.bitrateKbps = 900;
        track.channels = 2;
        track.bitDepth = 16;
        tracks.push_back(track);
    }
    return tracks;
}

int positiveArg(int argc, char **argv, int position, int fallback)
{
    if (argc <= position) {
        return fallback;
    }
    const int value = std::atoi(argv[position]);
    return value > 0 ? value : fallback;
}

} // namespace

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }
    QTemporaryDir stateRoot;
    if (!stateRoot.isValid()) {
        std::fprintf(stderr, "cannot create a temporary state root\n");
        return 1;
    }
    qputenv("MUZAITEN_STATE_ROOT", stateRoot.path().toUtf8());
    qputenv("MUZAITEN_DEMO_SILENT_AUDIO", "1");

    QApplication app(argc, argv);
    const int trackCount = positiveArg(argc, argv, 1, 5000);
    const int moves = positiveArg(argc, argv, 2, 100);

    AppCore core;
    MainWindow window(&core);
    PlayerCore *player = window.m_player;
    player->resetQueue(syntheticQueue(trackCount), 0);

    QElapsedTimer timer;
    timer.start();
    window.saveQueueState();
    const qint64 structuralUs = timer.nsecsElapsed() / 1000;

    timer.restart();
    for (int step = 1; step <= moves; ++step) {
        const int index = step % trackCount;
        player->m_queueIndex = index;
        player->m_playNextInsertIndex = index + 1;
        window.saveQueueState();
    }
    const qint64 cursorUs = timer.nsecsElapsed() / 1000;

    const qsizetype stateBytes = window.m_state->setting(QStringLiteral("queue.state")).toUtf8().size();
    const qsizetype cursorBytes = window.m_state->setting(QStringLiteral("queue.cursor")).toUtf8().size();

    std::printf("tracks=%d moves=%d\n", trackCount, moves);
    std::printf("structural save: %.3f ms\n", structuralUs / 1000.0);
    std::printf("cursor saves: total %.3f ms, mean %.3f ms\n", cursorUs / 1000.0, cursorUs / 1000.0 / moves);
    std::printf("queue.state: %lld bytes\n", static_cast<long long>(stateBytes));
    std::printf("queue.cursor: %lld bytes\n", static_cast<long long>(cursorBytes));
    return 0;
}
