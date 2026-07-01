#pragma once

#include "core/Track.h"
#include "scrobble/ListenHistoryStore.h"

#include <QObject>
#include <QString>

#include <functional>

// Watches playback and finalizes exactly one PlayEvent per track "spin",
// capturing how each playback ended (finished / skipped / stopped /
// session_end), how much was actually heard, where the track came from, and
// which listening session it belonged to. Runs always, independent of any
// scrobbling service — skips and stops are as much signal as completed listens.
//
// Timing is fully clock-driven (no QElapsedTimer/QTimer): every accumulation is
// computed from clock deltas at slot-invocation time, so tests can inject a
// deterministic fake clock via setClock(). Emits playEventReady() once per spin;
// the owner persists it into history.sqlite.
class PlayEventRecorder final : public QObject {
    Q_OBJECT

public:
    explicit PlayEventRecorder(QObject *parent = nullptr);

    // Injects the millisecond wall-clock source. Defaults to
    // QDateTime::currentMSecsSinceEpoch(); tests supply a mutable fake.
    void setClock(std::function<qint64()> clock);

public slots:
    // A track began playing. Finalizes any open event as "skipped", rolls the
    // listening session if the app was idle past the gap, then opens a fresh
    // event. userInitiated marks an explicit user pick; source describes how the
    // track was chosen (queue_manual | queue_auto | library_shuffle | resume).
    void trackStarted(const Track &track, bool userInitiated, const QString &source);
    // Session-restore continuation: seeds the accumulated playback time with
    // elapsedMs and anchors the start to when the track originally began, honors
    // the restored play/pause state, and is never treated as user-initiated.
    void resumeTrack(const Track &track, qint64 elapsedMs, bool playing, const QString &source);
    // The open track reached its natural end. Ignored if nothing is open or the
    // path does not match the open event.
    void trackFinishedNaturally(const Track &track);
    // Pause/resume: toggles segment accumulation and refreshes last-activity.
    void playbackStateChanged(bool playing);
    // Playback was torn down (queue emptied / current removed): finalize as
    // "stopped".
    void playbackCleared();
    // App exit: finalize any open event as "session_end". Safe to call twice.
    void flushSessionEnd();
    // The current shuffle mode ("off" | "queue" | "library"); the value at track
    // start is stamped into that track's event.
    void setShuffleMode(const QString &mode);

signals:
    void playEventReady(ListenHistoryStore::PlayEvent event);

private:
    // Finalizes the open event with the given outcome and emits it. No-op when
    // no event is open.
    void finalizeOpenEvent(const QString &outcome);
    // How much of the open event has actually played so far, including the live
    // segment if currently playing.
    qint64 playedMs(qint64 now) const;

    std::function<qint64()> m_msecsNow;

    QString m_sessionId;
    qint64 m_lastActivityMs = 0;

    // The open event, if any.
    bool m_hasOpenEvent = false;
    Track m_track;
    bool m_userInitiated = false;
    QString m_source;
    QString m_eventShuffleMode;
    qint64 m_startedAtMs = 0;
    qint64 m_accumulatedMs = 0;
    qint64 m_segmentStartMs = 0;
    bool m_playing = false;
    QString m_previousTrackPath;

    // The current shuffle mode, stamped into the next opened event.
    QString m_shuffleMode;

    // The previously finalized event's track/session, used to chain
    // previousTrackPath within a session.
    QString m_lastFinalizedTrackPath;
    QString m_lastFinalizedSessionId;
};
