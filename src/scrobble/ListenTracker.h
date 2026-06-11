#pragma once

#include "core/Track.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

// The single listen-progress engine: watches playback and decides when the
// current track counts as "listened" (half the duration, capped at 4 minutes —
// the Last.fm/ListenBrainz rule). Runs always, regardless of any scrobbling
// service being enabled, so the local listen history never has gaps. Emits
// listenReached() exactly once per track start, timestamped to the second
// playback began.
class ListenTracker final : public QObject {
    Q_OBJECT

public:
    explicit ListenTracker(QObject *parent = nullptr);

public slots:
    void trackStarted(const Track &track);
    // Resume an already-in-progress listen (e.g. playback restored with a
    // non-zero position). elapsedMs is how much of this track has already been
    // played; playing is the current play/pause state. A listen that already
    // crossed the threshold is assumed recorded by the prior session.
    void resumeTrack(const Track &track, qint64 elapsedMs, bool playing);
    void playbackStateChanged(bool playing);
    void checkListenProgress();

signals:
    void listenReached(Track track, qint64 startedAtSecs);

private:
    qint64 requiredListenMs(const Track &track) const;
    qint64 playedMs() const;

    QTimer *m_progressTimer = nullptr;
    Track m_currentTrack;
    bool m_hasCurrentTrack = false;
    bool m_playing = false;
    bool m_listenReached = false;
    qint64 m_startedAtSecs = 0;
    qint64 m_requiredMs = 0;
    qint64 m_accumulatedMs = 0;
    QElapsedTimer m_segmentTimer;
};
