#include "scrobble/ListenTracker.h"

#include <QDateTime>

#include <algorithm>

namespace {
constexpr qint64 maxRequiredListenMs = 4 * 60 * 1000;
}

ListenTracker::ListenTracker(QObject *parent)
    : QObject(parent)
{
    m_progressTimer = new QTimer(this);
    m_progressTimer->setInterval(1000);
    connect(m_progressTimer, &QTimer::timeout, this, &ListenTracker::checkListenProgress);
}

void ListenTracker::trackStarted(const Track &track)
{
    m_currentTrack = track;
    m_hasCurrentTrack = true;
    m_playing = true;
    m_listenReached = false;
    m_startedAtSecs = QDateTime::currentSecsSinceEpoch();
    m_requiredMs = requiredListenMs(track);
    m_accumulatedMs = 0;
    m_segmentTimer.restart();
    m_progressTimer->start();
}

void ListenTracker::resumeTrack(const Track &track, qint64 elapsedMs, bool playing)
{
    m_currentTrack = track;
    m_hasCurrentTrack = true;
    m_playing = playing;
    m_requiredMs = requiredListenMs(track);
    m_accumulatedMs = std::max<qint64>(0, elapsedMs);
    // Anchor the timestamp to when the track originally began so the listen
    // reports the real start time.
    m_startedAtSecs = QDateTime::currentSecsSinceEpoch() - m_accumulatedMs / 1000;
    // If we already passed the threshold assume the prior session recorded it.
    m_listenReached = (m_accumulatedMs >= m_requiredMs);
    m_segmentTimer.restart();

    if (m_playing && !m_listenReached) {
        m_progressTimer->start();
    } else {
        m_progressTimer->stop();
    }
}

void ListenTracker::playbackStateChanged(bool playing)
{
    if (!m_hasCurrentTrack || m_playing == playing) {
        return;
    }

    if (!playing && m_segmentTimer.isValid()) {
        m_accumulatedMs += m_segmentTimer.elapsed();
    } else if (playing) {
        m_segmentTimer.restart();
    }

    m_playing = playing;
    if (m_playing && !m_listenReached) {
        m_progressTimer->start();
    }
}

void ListenTracker::checkListenProgress()
{
    if (!m_playing || !m_hasCurrentTrack || m_listenReached) {
        return;
    }

    if (playedMs() >= m_requiredMs) {
        m_listenReached = true;
        m_progressTimer->stop();
        emit listenReached(m_currentTrack, m_startedAtSecs);
    }
}

qint64 ListenTracker::requiredListenMs(const Track &track) const
{
    if (track.durationMs <= 0) {
        return maxRequiredListenMs;
    }
    return std::min(track.durationMs / 2, maxRequiredListenMs);
}

qint64 ListenTracker::playedMs() const
{
    return m_accumulatedMs + (m_playing && m_segmentTimer.isValid() ? m_segmentTimer.elapsed() : 0);
}
