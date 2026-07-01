#include "scrobble/PlayEventRecorder.h"

#include <QDateTime>
#include <QUuid>

#include <algorithm>

namespace {
// Playback idle longer than this between spins starts a new listening session.
constexpr qint64 kSessionIdleGapMs = 30 * 60 * 1000;

QString newSessionId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
} // namespace

PlayEventRecorder::PlayEventRecorder(QObject *parent)
    : QObject(parent)
    , m_msecsNow([] { return QDateTime::currentMSecsSinceEpoch(); })
{
}

void PlayEventRecorder::setClock(std::function<qint64()> clock)
{
    if (clock) {
        m_msecsNow = std::move(clock);
    }
}

void PlayEventRecorder::trackStarted(const Track &track, bool userInitiated, const QString &source)
{
    const qint64 now = m_msecsNow();
    // Decide the session roll from the activity seen *before* this start:
    // finalizing the outgoing event would otherwise reset last-activity to now
    // and mask a real idle gap. An open event still playing is continuous
    // listening, not idleness — a track longer than the gap (a DJ mix, an
    // ambient piece) must not roll the session merely because nothing stamped
    // activity while it played.
    const bool continuousPlayback = m_hasOpenEvent && m_playing;
    const bool rollSession = m_sessionId.isEmpty()
        || (!continuousPlayback && (now - m_lastActivityMs) > kSessionIdleGapMs);

    finalizeOpenEvent(QStringLiteral("skipped"));

    if (rollSession) {
        m_sessionId = newSessionId();
    }
    // Chain the previous track only within one session; a rolled session breaks
    // the chain because the last finalized event belongs to the old session.
    const QString previous = (m_lastFinalizedSessionId == m_sessionId) ? m_lastFinalizedTrackPath : QString();

    m_hasOpenEvent = true;
    m_track = track;
    m_userInitiated = userInitiated;
    m_source = source;
    m_eventShuffleMode = m_shuffleMode;
    m_startedAtMs = now;
    m_accumulatedMs = 0;
    m_segmentStartMs = now;
    m_playing = true;
    m_previousTrackPath = previous;
    m_lastActivityMs = now;
}

void PlayEventRecorder::resumeTrack(const Track &track, qint64 elapsedMs, bool playing, const QString &source)
{
    const qint64 now = m_msecsNow();
    // Same idle rule as trackStarted, including the continuous-playback carve-out.
    const bool continuousPlayback = m_hasOpenEvent && m_playing;
    const bool rollSession = m_sessionId.isEmpty()
        || (!continuousPlayback && (now - m_lastActivityMs) > kSessionIdleGapMs);

    finalizeOpenEvent(QStringLiteral("skipped"));

    if (rollSession) {
        m_sessionId = newSessionId();
    }
    const QString previous = (m_lastFinalizedSessionId == m_sessionId) ? m_lastFinalizedTrackPath : QString();

    const qint64 seed = std::max<qint64>(0, elapsedMs);
    m_hasOpenEvent = true;
    m_track = track;
    m_userInitiated = false;
    m_source = source;
    m_eventShuffleMode = m_shuffleMode;
    // Anchor the start to when the track originally began so startedAt reflects
    // the real listen, not the restore moment.
    m_startedAtMs = now - seed;
    m_accumulatedMs = seed;
    m_segmentStartMs = now;
    m_playing = playing;
    m_previousTrackPath = previous;
    m_lastActivityMs = now;
}

void PlayEventRecorder::trackFinishedNaturally(const Track &track)
{
    if (!m_hasOpenEvent || m_track.path != track.path) {
        return;
    }
    finalizeOpenEvent(QStringLiteral("finished"));
}

void PlayEventRecorder::playbackStateChanged(bool playing)
{
    const qint64 now = m_msecsNow();
    m_lastActivityMs = now;
    if (!m_hasOpenEvent || m_playing == playing) {
        return;
    }
    if (!playing) {
        m_accumulatedMs += std::max<qint64>(0, now - m_segmentStartMs);
    } else {
        m_segmentStartMs = now;
    }
    m_playing = playing;
}

void PlayEventRecorder::playbackCleared()
{
    finalizeOpenEvent(QStringLiteral("stopped"));
}

void PlayEventRecorder::flushSessionEnd()
{
    finalizeOpenEvent(QStringLiteral("session_end"));
}

void PlayEventRecorder::setShuffleMode(const QString &mode)
{
    m_shuffleMode = mode;
}

qint64 PlayEventRecorder::playedMs(qint64 now) const
{
    return m_accumulatedMs + (m_playing ? std::max<qint64>(0, now - m_segmentStartMs) : 0);
}

void PlayEventRecorder::finalizeOpenEvent(const QString &outcome)
{
    if (!m_hasOpenEvent) {
        return;
    }
    const qint64 now = m_msecsNow();
    const qint64 played = playedMs(now);

    ListenHistoryStore::PlayEvent event;
    event.startedAtSecs = m_startedAtMs / 1000;
    event.endedAtSecs = now / 1000;
    event.playedMs = played;
    event.durationMs = m_track.durationMs;
    event.completion = m_track.durationMs > 0
        ? std::min(1.0, static_cast<double>(played) / static_cast<double>(m_track.durationMs))
        : -1.0;
    event.outcome = outcome;
    event.userInitiated = m_userInitiated;
    event.source = m_source;
    event.shuffleMode = m_eventShuffleMode;
    event.track = m_track;
    event.previousTrackPath = m_previousTrackPath;
    event.sessionId = m_sessionId;

    m_lastFinalizedTrackPath = m_track.path;
    m_lastFinalizedSessionId = m_sessionId;
    m_hasOpenEvent = false;
    m_playing = false;
    m_lastActivityMs = now;

    emit playEventReady(event);
}
