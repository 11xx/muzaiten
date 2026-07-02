#include "player/PlayerCore.h"

#include "playback/PlaybackBackend.h"

#include <QHash>
#include <QRandomGenerator>
#include <QUrl>

#include <algorithm>

PlayerCore::PlayerCore(PlaybackBackend *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
{
    Q_ASSERT(m_backend != nullptr);
    m_backend->setParent(this);
    connect(m_backend, &PlaybackBackend::preparedTrackStarted, this, &PlayerCore::onPreparedTrackStarted);
    connect(m_backend, &PlaybackBackend::finished, this, &PlayerCore::onFinished);
    connect(m_backend, &PlaybackBackend::stateChanged, this, [this](PlaybackBackend::State state) {
        // A successful start ends the one-prompt-per-DSD-block suppression. Do
        // not reset it merely because a track was selected or preroll began.
        if (state == PlaybackBackend::State::Playing) {
            m_skipDsdTakeoverBlock = false;
        }
    });
}

QString PlayerCore::resolvePath(const Track &track) const
{
    return m_resolvePath ? m_resolvePath(track) : track.path;
}

void PlayerCore::playAt(int index, bool notifyScrobbler, bool startPaused, bool explicitJump)
{
    if (index < 0 || index >= m_queue.size()) {
        return;
    }

    // A user may move on while the status-bar takeover question is visible. The
    // timer/button callback is intentionally allowed to arrive later, but it
    // must not start the abandoned track.
    m_pendingDsdTakeover.active = false;

    const int previousIndex = m_queueIndex;
    m_queueIndex = index;
    if (explicitJump) {
        // A manual jump to any row (forward or backward) clears the play-next
        // batch: the user picking a new current track supersedes whatever was
        // queued to play next. Collapsing the region to m_queueIndex+1 drops the
        // play-next marking while leaving every track in its natural queue order
        // (nothing is moved). A backward jump must NOT turn the skipped tracks
        // into a new play-next span — that was misleading and is intentionally
        // not done here.
        m_playNextInsertIndex = m_queueIndex + 1;
        refreshShuffleForManualPick(previousIndex, m_queueIndex);
    } else {
        collapsePlayNextIfStale();
    }
    emit currentIndexChanged(m_queueIndex, /*userInitiated=*/true);
    emit playNextRangeChanged();
    playCurrent(notifyScrobbler, startPaused);
}

void PlayerCore::playCurrent(bool notifyScrobbler, bool startPaused)
{
    const Track track = m_queue.at(m_queueIndex);
    if (track.path.isEmpty()) {
        return;
    }
    const QString playbackPath = resolvePath(track);
    if (playbackPath.isEmpty()) {
        emit trackUnresolvable(track);
        return;
    }

    if (isDsdTrack(track) && m_skipDsdTakeoverBlock) {
        emit trackStartSkipped(track, QStringLiteral("DSD device takeover was declined for this queue block"));
        skipCurrentTrack();
        return;
    }

    const PlaybackStartPlan plan = m_playbackStartPlanner
        ? m_playbackStartPlanner(track) : PlaybackStartPlan{};
    if (plan.action == PlaybackStartPlan::Action::Skip) {
        emit trackStartSkipped(track, plan.reason);
        skipCurrentTrack();
        return;
    }
    if (plan.action == PlaybackStartPlan::Action::DeferForDsdTakeover) {
        // Stop a manually selected previous source while waiting; for
        // auto-advance the old source is already at EOS. Do not present or
        // scrobble the new track until the user accepts the takeover.
        m_backend->prepareNext({});
        m_backend->stop();
        m_pendingDsdTakeover = {true, track, playbackPath, plan.device, notifyScrobbler, startPaused};
        emit dsdTakeoverRequested(track, plan.device);
        return;
    }

    startTrack(track, playbackPath, notifyScrobbler, startPaused, plan);
}

void PlayerCore::startTrack(const Track &track, const QString &playbackPath, bool notifyScrobbler,
                            bool startPaused, const PlaybackStartPlan &plan)
{
    m_backend->setOutputMode(plan.action == PlaybackStartPlan::Action::NativeDsd
                                 ? PlaybackBackend::OutputMode::NativeDsd
                                 : PlaybackBackend::OutputMode::Normal,
                             plan.device);

    m_currentTrack = track;
    markVisited(m_queueIndex);
    emit currentTrackChanged(m_currentTrack, notifyScrobbler);
    if (startPaused) {
        m_backend->loadPaused(QUrl::fromLocalFile(playbackPath));
    } else {
        m_backend->play(QUrl::fromLocalFile(playbackPath));
    }
    prepareNext();
}

void PlayerCore::resolveDsdTakeover(bool accepted)
{
    if (!m_pendingDsdTakeover.active) {
        return;
    }
    const PendingDsdTakeover pending = m_pendingDsdTakeover;
    m_pendingDsdTakeover.active = false;
    if (!accepted) {
        m_skipDsdTakeoverBlock = true;
        emit trackStartSkipped(pending.track, QStringLiteral("DSD device takeover was declined"));
        skipCurrentTrack();
        return;
    }

    PlaybackStartPlan plan;
    plan.action = PlaybackStartPlan::Action::NativeDsd;
    plan.device = pending.device;
    startTrack(pending.track, pending.playbackPath, pending.notifyScrobbler, pending.startPaused, plan);
}

void PlayerCore::skipCurrentTrack()
{
    // This is deliberately not onFinished(): a declined DSD must not be replayed
    // by Repeat One, and an unplayable row should not become shuffle history.
    //
    // The advance below routes through playAt()→playCurrent(), which may decide
    // the new row is *also* skippable (a contiguous DSD block past a declined
    // takeover, a run of unresolvable files) and call back in here. Recursing one
    // stack frame per skipped track would overflow on a long block — and never
    // terminate under Repeat All, which always offers another row. Flatten the
    // cascade into a single loop: a re-entrant call just flags "go again", and a
    // queue-length guard stops an all-skippable queue instead of spinning.
    if (m_skipInProgress) {
        m_skipPending = true;
        return;
    }
    m_skipInProgress = true;
    int guard = static_cast<int>(m_queue.size()) + 1;
    do {
        m_skipPending = false;
        m_backend->prepareNext({});
        const AutoNext target = decideAutoNext();
        if (!target.injected.path.isEmpty()) {
            emit aboutToInjectLibraryTrack(target.injected);
            m_queue.push_back(target.injected);
            emit queueChanged();
            playAt(static_cast<int>(m_queue.size()) - 1);
        } else if (target.index >= 0 && target.index < m_queue.size()) {
            playAt(target.index);
        } else {
            m_backend->stop();
            break;
        }
    } while (m_skipPending && --guard > 0);
    // Everything reachable was skippable (e.g. an all-DSD queue under Repeat All
    // with the takeover declined). Stop rather than loop forever.
    if (m_skipPending && guard <= 0) {
        m_backend->stop();
    }
    m_skipInProgress = false;
    m_skipPending = false;
}

void PlayerCore::next()
{
    if (m_queue.isEmpty()) {
        return;
    }
    // Shuffle, repeat-all and radio change what "next" means; a manual skip
    // honours the same policy as auto-advance (a fresh decision, so library
    // shuffle can still inject, shuffle re-rolls, and radio extends past the
    // queue end). Repeat-one is intentionally excluded: an explicit Next moves on
    // rather than re-looping the current track.
    if (m_shuffleMode != ShuffleMode::Off || m_repeatMode == RepeatMode::All || m_radioActive) {
        const AutoNext target = decideAutoNext();
        if (target.index >= 0 || !target.injected.path.isEmpty()) {
            applyAutoNext(target);
            return;
        }
        // Shuffle cycle exhausted with no repeat: nothing left to advance to.
        return;
    }
    playAt(std::min(static_cast<int>(m_queue.size() - 1), m_queueIndex + 1));
}

void PlayerCore::previous()
{
    if (m_queue.isEmpty()) {
        return;
    }
    if (m_shuffleMode != ShuffleMode::Off && !m_shuffleHistory.isEmpty()) {
        int previousIndex = m_shuffleHistory.takeLast();
        previousIndex = std::clamp(previousIndex, 0, static_cast<int>(m_queue.size()) - 1);
        // Remember the row we're leaving so a subsequent Next/auto-advance replays
        // it forward (linear retrace) instead of re-rolling a fresh shuffle pick.
        if (m_queueIndex >= 0 && m_queueIndex < m_queue.size()) {
            m_shuffleForward.push_back(m_queueIndex);
        }
        playShuffleJump(previousIndex);
        return;
    }
    playAt(std::max(0, m_queueIndex - 1));
}

void PlayerCore::togglePlayPause()
{
    if (!m_backend->hasSource()) {
        if (!m_queue.isEmpty()) {
            playAt(m_queueIndex >= 0 ? m_queueIndex : 0);
        }
        return;
    }

    if (m_backend->state() == PlaybackBackend::State::Playing) {
        m_backend->pause();
    } else {
        m_backend->resume();
    }
}

void PlayerCore::play()
{
    if (m_backend->hasSource()) {
        m_backend->resume();
        return;
    }
    if (!m_queue.isEmpty()) {
        playAt(m_queueIndex >= 0 ? m_queueIndex : 0);
    }
}

void PlayerCore::seekRelative(qint64 offsetMs)
{
    const qint64 duration = std::max<qint64>(0, m_backend->duration());
    const qint64 requested = m_backend->position() + offsetMs;
    m_backend->seek(duration > 0 ? std::clamp<qint64>(requested, 0, duration) : std::max<qint64>(0, requested));
}

void PlayerCore::setVolume(double volume0To1)
{
    m_volume = std::clamp(volume0To1, 0.0, 1.0);
    m_backend->setVolume(m_volume);
    emit volumeChanged(m_volume);
}

void PlayerCore::appendAndPlay(const Track &track)
{
    if (track.path.isEmpty()) {
        return;
    }

    for (int index = 0; index < m_queue.size(); ++index) {
        if (m_queue.at(index).path == track.path) {
            // Reset before jumping so playAt's guard always clears any stale
            // play-next boundary. Without this, a boundary that coincides with
            // m_queue.size() (left over from playing the last track) would
            // survive the guard check and spuriously badge all trailing entries.
            m_playNextInsertIndex = -1;
            playAt(index, true, false, /*explicitJump=*/true);
            return;
        }
    }

    emit aboutToAddTracks(QVector<Track>{track});
    m_queue.push_back(track);
    emit queueChanged();
    playAt(static_cast<int>(m_queue.size() - 1), true, false, /*explicitJump=*/true);
}

void PlayerCore::playTracksNext(const QVector<Track> &tracks)
{
    if (tracks.isEmpty()) {
        return;
    }
    emit aboutToAddTracks(tracks);

    if (m_queueIndex < 0 || m_queue.isEmpty()) {
        const int start = static_cast<int>(m_queue.size());
        for (const Track &track : tracks) {
            if (!track.path.isEmpty()) {
                m_queue.push_back(track);
            }
        }
        if (m_queueIndex < 0 && start < m_queue.size()) {
            // Show the new rows, then start playback (playAt prepares next).
            emit queueChanged();
            playAt(start);
        } else {
            prepareNext();
            emit queueChanged();
        }
        return;
    }

    int insertAt = m_playNextInsertIndex;
    if (insertAt <= m_queueIndex || insertAt > m_queue.size()) {
        insertAt = m_queueIndex + 1;
    }

    int inserted = 0;
    for (const Track &track : tracks) {
        if (track.path.isEmpty()) {
            continue;
        }
        m_queue.insert(insertAt + inserted, track);
        ++inserted;
    }

    m_playNextInsertIndex = insertAt + inserted;
    // Inserting mid-queue shifts later rows, invalidating the shuffle bag/history.
    resetShuffleState();
    prepareNext();
    emit queueChanged();
}

void PlayerCore::appendTracks(const QVector<Track> &tracks)
{
    emit aboutToAddTracks(tracks);
    for (const Track &track : tracks) {
        if (!track.path.isEmpty()) {
            m_queue.push_back(track);
        }
    }
    collapsePlayNextIfStale();
    prepareNext();
    emit queueChanged();
}

void PlayerCore::injectTracks(const QVector<Track> &tracks)
{
    bool appended = false;
    for (const Track &track : tracks) {
        if (track.path.isEmpty()) {
            continue;
        }
        emit aboutToInjectLibraryTrack(track);
        m_queue.push_back(track);
        appended = true;
    }
    if (!appended) {
        return;
    }
    collapsePlayNextIfStale();
    prepareNext();
    emit queueChanged();
}

void PlayerCore::moveRows(const QVector<int> &rows, int destinationRow)
{
    if (rows.isEmpty() || m_queue.isEmpty()) {
        return;
    }

    QVector<int> sortedRows = rows;
    std::sort(sortedRows.begin(), sortedRows.end());
    sortedRows.erase(std::unique(sortedRows.begin(), sortedRows.end()), sortedRows.end());
    sortedRows.erase(std::remove_if(sortedRows.begin(), sortedRows.end(), [this](int row) {
                         return row < 0 || row >= m_queue.size();
                     }),
                     sortedRows.end());
    if (sortedRows.isEmpty()) {
        return;
    }

    // Allow dropping anywhere, including above the current track: newQueueIndex
    // below tracks where the playing track lands, so playback stays correct.
    destinationRow = std::clamp(destinationRow, 0, static_cast<int>(m_queue.size()));
    QVector<Track> moving;
    moving.reserve(sortedRows.size());
    int removedBeforeDestination = 0;
    for (int row : sortedRows) {
        moving.push_back(m_queue.at(row));
        if (row < destinationRow) {
            ++removedBeforeDestination;
        }
    }
    const int adjustedDestination = destinationRow - removedBeforeDestination;

    QVector<Track> remaining;
    remaining.reserve(m_queue.size() - moving.size());
    int removeCursor = 0;
    for (int row = 0; row < m_queue.size(); ++row) {
        if (removeCursor < sortedRows.size() && sortedRows.at(removeCursor) == row) {
            ++removeCursor;
            continue;
        }
        remaining.push_back(m_queue.at(row));
    }

    m_queue = remaining;
    for (int offset = 0; offset < moving.size(); ++offset) {
        m_queue.insert(adjustedDestination + offset, moving.at(offset));
    }

    int newQueueIndex = -1;
    if (m_queueIndex >= 0) {
        const auto movedCurrent = std::find(sortedRows.cbegin(), sortedRows.cend(), m_queueIndex);
        if (movedCurrent != sortedRows.cend()) {
            newQueueIndex = adjustedDestination + static_cast<int>(std::distance(sortedRows.cbegin(), movedCurrent));
        } else {
            const int removedBeforeCurrent = static_cast<int>(std::count_if(sortedRows.cbegin(), sortedRows.cend(), [this](int row) {
                return row < m_queueIndex;
            }));
            newQueueIndex = m_queueIndex - removedBeforeCurrent;
            if (adjustedDestination <= newQueueIndex) {
                newQueueIndex += static_cast<int>(moving.size());
            }
        }
    }
    m_queueIndex = newQueueIndex;
    // Manually reordering the upcoming tracks collapses the play-next priority
    // block (its ordinals no longer reflect a single "play next" batch).
    m_playNextInsertIndex = std::clamp(m_queueIndex + 1, 0, static_cast<int>(m_queue.size()));
    resetShuffleState();
    prepareNext();
    emit queueChanged();
}

void PlayerCore::removeRows(const QVector<int> &rows)
{
    if (rows.isEmpty() || m_queue.isEmpty()) {
        return;
    }

    // Capture playback state before mutating: if the current track is among the
    // removed rows we advance the backend onto its successor instead of leaving
    // the (now-removed) audio playing.
    const bool wasPlaying = m_backend->state() == PlaybackBackend::State::Playing;
    const bool hadSource = m_backend->hasSource();

    QVector<int> sortedRows = rows;
    std::sort(sortedRows.begin(), sortedRows.end());
    sortedRows.erase(std::unique(sortedRows.begin(), sortedRows.end()), sortedRows.end());

    // The play-next region is [m_queueIndex+1, m_playNextInsertIndex). Removing
    // rows shifts its end left by the number of removed rows that sat before it,
    // preserving the order/size of the surviving play-next tracks (instead of
    // collapsing the region). It only empties when every play-next track is gone.
    const int oldPlayNextInsertIndex = m_playNextInsertIndex;
    QVector<Track> remaining;
    remaining.reserve(m_queue.size());
    int newQueueIndex = -1;
    int removedBeforeCurrent = 0;
    int removedBeforePlayNextEnd = 0;
    bool removedCurrent = false;
    int removeCursor = 0;
    for (int row = 0; row < m_queue.size(); ++row) {
        const bool remove = removeCursor < sortedRows.size() && sortedRows.at(removeCursor) == row;
        if (remove) {
            if (row < m_queueIndex) {
                ++removedBeforeCurrent;
            } else if (row == m_queueIndex) {
                removedCurrent = true;
            }
            if (row < oldPlayNextInsertIndex) {
                ++removedBeforePlayNextEnd;
            }
            ++removeCursor;
            continue;
        }
        remaining.push_back(m_queue.at(row));
    }

    if (!removedCurrent && m_queueIndex >= 0) {
        newQueueIndex = m_queueIndex - removedBeforeCurrent;
    } else if (!remaining.isEmpty()) {
        newQueueIndex = std::min(m_queueIndex - removedBeforeCurrent, static_cast<int>(remaining.size()) - 1);
    }

    m_queue = remaining;
    m_queueIndex = std::clamp(newQueueIndex, -1, static_cast<int>(m_queue.size()) - 1);
    // Shift the play-next end by removals before it, then clamp into the valid
    // range; if it lands at/below m_queueIndex+1 the region empties on its own.
    m_playNextInsertIndex = std::clamp(oldPlayNextInsertIndex - removedBeforePlayNextEnd,
                                       m_queueIndex + 1, static_cast<int>(m_queue.size()));
    resetShuffleState();
    prepareNext();
    emit queueChanged();
    if (m_queueIndex >= 0 && m_queueIndex < m_queue.size()) {
        if (removedCurrent) {
            // The track playing/loaded was removed; m_queueIndex now points at the
            // one that shifted into its slot — the next in queue order (even under
            // shuffle, since this is a direct advance, not an auto-next decision).
            // Match the prior transport state: keep playing, or stay paused on the
            // new track so the backend's source doesn't lag behind the queue.
            if (wasPlaying) {
                playCurrent(/*notifyScrobbler=*/true, /*startPaused=*/false);
            } else if (hadSource) {
                playCurrent(/*notifyScrobbler=*/false, /*startPaused=*/true);
            } else {
                presentTrack(m_queue.at(m_queueIndex));
            }
        } else {
            presentTrack(m_queue.at(m_queueIndex));
        }
    } else {
        m_currentTrack = {};
        m_backend->stop();
        emit playbackCleared();
    }
}

void PlayerCore::clearKeepingCurrent()
{
    const bool keepCurrent = m_queueIndex >= 0 && m_queueIndex < m_queue.size();
    if (!keepCurrent) {
        clearAll();
        return;
    }
    const Track current = m_queue.at(m_queueIndex);
    m_queue.clear();
    m_queue.push_back(current);
    m_queueIndex = 0;
    m_playNextInsertIndex = 1;
    resetShuffleState();
    prepareNext();
    emit queueChanged();
    presentTrack(current);
}

void PlayerCore::clearAll()
{
    m_queue.clear();
    m_queueIndex = -1;
    m_playNextInsertIndex = -1;
    m_currentTrack = {};
    resetShuffleState();
    prepareNext();
    emit queueChanged();
    m_backend->stop();
    emit playbackCleared();
}

void PlayerCore::collapsePlayNext()
{
    m_playNextInsertIndex = m_queueIndex >= 0 ? m_queueIndex + 1 : -1;
    emit playNextRangeChanged();
}

void PlayerCore::patchTracksFromMetadata(const QVector<Track> &tracks)
{
    if (tracks.isEmpty() || m_queue.isEmpty()) {
        return;
    }

    QHash<QString, Track> byPath;
    byPath.reserve(tracks.size());
    for (const Track &track : tracks) {
        if (!track.path.isEmpty()) {
            byPath.insert(track.path, track);
        }
    }
    if (byPath.isEmpty()) {
        return;
    }

    QVector<int> changedRows;
    bool currentTrackChangedAny = false;
    for (int row = 0; row < m_queue.size(); ++row) {
        Track &queuedTrack = m_queue[row];
        const auto it = byPath.constFind(queuedTrack.path);
        if (it == byPath.cend()) {
            continue;
        }
        queuedTrack = it.value();
        changedRows.push_back(row);
        if (m_currentTrack.path == queuedTrack.path) {
            m_currentTrack = queuedTrack;
            currentTrackChangedAny = true;
        }
    }

    if (changedRows.isEmpty()) {
        return;
    }
    // Paths are unchanged, so the prepared gapless "next" stays valid; only the
    // displayed metadata refreshes.
    emit queueTracksChanged(changedRows);
    if (currentTrackChangedAny) {
        emit currentTrackUpdated(m_currentTrack);
    }
}

void PlayerCore::markTracksMissing(const QStringList &paths)
{
    if (paths.isEmpty() || m_queue.isEmpty()) {
        return;
    }

    QSet<QString> missingPaths;
    missingPaths.reserve(paths.size());
    for (const QString &path : paths) {
        missingPaths.insert(path);
    }
    QVector<int> changedRows;
    bool currentUpdated = false;
    for (int row = 0; row < m_queue.size(); ++row) {
        Track &track = m_queue[row];
        if (!missingPaths.contains(track.path) || track.missing) {
            continue;
        }
        track.missing = true;
        changedRows.push_back(row);
        if (m_currentTrack.path == track.path) {
            m_currentTrack.missing = true;
            currentUpdated = true;
        }
    }

    if (changedRows.isEmpty()) {
        return;
    }
    emit queueTracksChanged(changedRows);
    if (currentUpdated) {
        emit currentTrackUpdated(m_currentTrack);
    }
}

void PlayerCore::updateTrackRating(const QString &path, int rating0To100, bool hasUserRating)
{
    for (Track &queuedTrack : m_queue) {
        if (queuedTrack.path != path) {
            continue;
        }
        queuedTrack.hasUserRating = hasUserRating;
        queuedTrack.effectiveRating0To100 = hasUserRating ? rating0To100 : queuedTrack.rating0To100;
    }
    if (m_currentTrack.path == path) {
        m_currentTrack.hasUserRating = hasUserRating;
        m_currentTrack.effectiveRating0To100 = hasUserRating ? rating0To100 : m_currentTrack.rating0To100;
    }
}

bool PlayerCore::applyRatingSync(const QString &path, int effectiveRating0To100)
{
    const bool hasUserRating = effectiveRating0To100 >= 0;
    for (Track &queuedTrack : m_queue) {
        if (queuedTrack.path != path) {
            continue;
        }
        queuedTrack.rating0To100 = effectiveRating0To100;
        queuedTrack.hasUserRating = hasUserRating;
        queuedTrack.effectiveRating0To100 = effectiveRating0To100;
    }
    if (m_currentTrack.path != path) {
        return false;
    }
    m_currentTrack.rating0To100 = effectiveRating0To100;
    m_currentTrack.hasUserRating = hasUserRating;
    m_currentTrack.effectiveRating0To100 = effectiveRating0To100;
    return true;
}

void PlayerCore::resetQueue(const QVector<Track> &tracks, int index, int playNextInsertIndex)
{
    m_queue.clear();
    m_queue.reserve(tracks.size());
    for (const Track &track : tracks) {
        if (!track.path.isEmpty()) {
            m_queue.push_back(track);
        }
    }
    m_queueIndex = std::clamp(index, -1, static_cast<int>(m_queue.size()) - 1);
    m_playNextInsertIndex = m_queue.isEmpty() || m_queueIndex < 0
        ? -1
        : std::clamp(playNextInsertIndex, m_queueIndex + 1, static_cast<int>(m_queue.size()));
    resetShuffleState();
}

void PlayerCore::presentTrack(const Track &track)
{
    m_currentTrack = track;
    emit currentTrackChanged(m_currentTrack, /*notifyScrobbler=*/false);
}

void PlayerCore::prepareNext()
{
    m_preparedNext = {};
    if (m_queueIndex < 0 || m_queueIndex >= m_queue.size()) {
        m_backend->prepareNext({});
        return;
    }
    // Repeat-one loops via onFinished (a re-play), not gapless preloading: a
    // seamless self-loop is niche and fiddly to drive through the backend.
    if (m_repeatMode == RepeatMode::One) {
        m_backend->prepareNext({});
        return;
    }
    m_preparedNext = decideAutoNext();
    // Only an in-queue follow-up can be gaplessly preloaded; library injections
    // (and end-of-queue) preload nothing and are handled in onFinished.
    const bool gapless = m_preparedNext.index >= 0 && m_preparedNext.index < m_queue.size()
        && m_preparedNext.injected.path.isEmpty();
    if (!gapless) {
        m_backend->prepareNext({});
        return;
    }
    // A native-DSD boundary needs a different top-level pipeline, and a shared
    // DSD start may wait on the asynchronous takeover prompt. Never preload
    // through playbin across either side of that boundary.
    if (isDsdTrack(m_currentTrack) || isDsdTrack(m_queue.at(m_preparedNext.index))) {
        m_backend->prepareNext({});
        return;
    }
    const QString nextPath = resolvePath(m_queue.at(m_preparedNext.index));
    m_backend->prepareNext(nextPath.isEmpty() ? QUrl() : QUrl::fromLocalFile(nextPath));
}

void PlayerCore::collapsePlayNextIfStale()
{
    if (m_queueIndex < 0) {
        m_playNextInsertIndex = -1;
    } else if (m_playNextInsertIndex <= m_queueIndex || m_playNextInsertIndex > m_queue.size()) {
        m_playNextInsertIndex = m_queueIndex + 1;
    }
}

void PlayerCore::setRepeatMode(RepeatMode mode)
{
    if (m_repeatMode == mode) {
        return;
    }
    m_repeatMode = mode;
    emit repeatModeChanged(m_repeatMode);
    // Re-derive the gapless preload: e.g. enabling repeat-all on the last track
    // must now preload the first track for a seamless wrap.
    prepareNext();
}

void PlayerCore::setShuffleMode(ShuffleMode mode)
{
    if (m_shuffleMode == mode) {
        return;
    }
    m_shuffleMode = mode;
    if (m_shuffleMode != ShuffleMode::Off) {
        resetShuffleState();
    }
    emit shuffleModeChanged(m_shuffleMode);
    prepareNext();
}

void PlayerCore::setLibraryShufflePercent(int percent)
{
    percent = std::clamp(percent, 0, 100);
    if (m_libraryShufflePercent == percent) {
        return;
    }
    m_libraryShufflePercent = percent;
    emit libraryShufflePercentChanged(m_libraryShufflePercent);
}

void PlayerCore::setRadioShufflePercent(int percent)
{
    percent = std::clamp(percent, 0, 100);
    if (m_radioShufflePercent == percent) {
        return;
    }
    m_radioShufflePercent = percent;
    emit radioShufflePercentChanged(m_radioShufflePercent);
}

void PlayerCore::setRadioActive(bool active)
{
    if (m_radioActive == active) {
        return;
    }
    m_radioActive = active;
    // Deactivating clears only the flag: the queue, shuffle/repeat state and the
    // installed radio provider are left as-is for the owner to tear down.
    emit radioActiveChanged(m_radioActive);
}

PlayerCore::AutoNext PlayerCore::decideAutoNext()
{
    if (m_queue.isEmpty() || m_queueIndex < 0) {
        return {};
    }
    // A remembered forward trail (built by Previous) wins over any re-roll or
    // library injection: Next/auto-advance must retrace the exact order the user
    // already heard until the trail is spent, then resume fresh randomness.
    if (m_shuffleMode != ShuffleMode::Off && !m_shuffleForward.isEmpty()) {
        return {std::clamp(m_shuffleForward.last(), 0, static_cast<int>(m_queue.size()) - 1), {}};
    }
    // Radio session: a scored auto-queue. Queued rows still win — a mid-queue
    // advance plays the plain next row (play-next / user appends keep priority) —
    // but past the queue's end we extend with a fresh recommendation pick. This
    // deliberately precedes the library-shuffle percent roll below: a radio
    // session must not be diluted by random library pulls. If the provider yields
    // nothing, fall through to the normal shuffle/end-of-queue/repeat handling.
    if (m_radioActive && m_radioTracks) {
        if (m_queueIndex + 1 < m_queue.size()) {
            return {m_queueIndex + 1, {}};
        }
        QSet<QString> exclude;
        exclude.reserve(m_queue.size());
        for (const Track &track : m_queue) {
            exclude.insert(track.path);
        }
        const QVector<Track> picks = m_radioTracks(1, exclude);
        if (!picks.isEmpty() && !picks.first().path.isEmpty()) {
            return {-1, picks.first()};
        }
    }
    // Library-wide shuffle: with the configured probability, pull a fresh track
    // from the whole library instead of advancing within the queue.
    if (m_shuffleMode == ShuffleMode::Library && m_randomTracks && m_libraryShufflePercent > 0) {
        if (QRandomGenerator::global()->bounded(100) < m_libraryShufflePercent) {
            QSet<QString> exclude;
            exclude.reserve(m_queue.size());
            for (const Track &track : m_queue) {
                exclude.insert(track.path);
            }
            const QVector<Track> picks = m_randomTracks(1, exclude);
            if (!picks.isEmpty() && !picks.first().path.isEmpty()) {
                return {-1, picks.first()};
            }
        }
    }
    // Ambient Radio shuffle: same queue-vs-library semantics as Library shuffle,
    // but taste-aware pulls use the radio provider. Explicit Start Radio is
    // handled above by m_radioActive and always takes precedence.
    if (m_shuffleMode == ShuffleMode::Radio && m_radioTracks && m_radioShufflePercent > 0) {
        if (QRandomGenerator::global()->bounded(100) < m_radioShufflePercent) {
            QSet<QString> exclude;
            exclude.reserve(m_queue.size());
            for (const Track &track : m_queue) {
                exclude.insert(track.path);
            }
            const QVector<Track> picks = m_radioTracks(1, exclude);
            if (!picks.isEmpty() && !picks.first().path.isEmpty()) {
                return {-1, picks.first()};
            }
        }
    }
    if (m_shuffleMode != ShuffleMode::Off) {
        return {pickShuffleIndex(), {}};
    }
    if (m_queueIndex + 1 < m_queue.size()) {
        return {m_queueIndex + 1, {}};
    }
    if (m_repeatMode == RepeatMode::All) {
        return {0, {}};
    }
    return {};
}

int PlayerCore::pickShuffleIndex()
{
    const int size = static_cast<int>(m_queue.size());
    if (size <= 1) {
        // A single-track queue can only loop (repeat-all) or stop.
        return m_repeatMode == RepeatMode::All ? m_queueIndex : -1;
    }

    QVector<int> candidates;
    candidates.reserve(size);
    for (int index = 0; index < size; ++index) {
        if (index != m_queueIndex && !m_shuffleVisited.contains(index)) {
            candidates.push_back(index);
        }
    }
    if (candidates.isEmpty()) {
        // Every other track has played this cycle. Repeat-all reshuffles from a
        // clean slate; otherwise the shuffle is exhausted.
        if (m_repeatMode != RepeatMode::All) {
            return -1;
        }
        m_shuffleVisited.clear();
        m_shuffleVisited.insert(m_queueIndex);
        for (int index = 0; index < size; ++index) {
            if (index != m_queueIndex) {
                candidates.push_back(index);
            }
        }
    }
    return candidates.at(QRandomGenerator::global()->bounded(static_cast<int>(candidates.size())));
}

void PlayerCore::applyAutoNext(const AutoNext &next)
{
    if (!next.injected.path.isEmpty()) {
        // Library-wide injection: append the fresh track, then play it. playAt
        // re-presents, re-prepares and marks it visited. A fresh injection
        // diverges from any remembered forward trail.
        emit aboutToInjectLibraryTrack(next.injected);
        m_queue.push_back(next.injected);
        pushHistory(m_queueIndex);
        m_shuffleForward.clear();
        emit queueChanged();
        playAt(static_cast<int>(m_queue.size()) - 1);
        return;
    }
    if (next.index >= 0 && next.index < m_queue.size()) {
        // A plain non-shuffle step to the very next row consumes the play-next
        // region in order; every other move (shuffle pick, remembered retrace,
        // repeat-all wrap) is a jump that must collapse the region so the rows it
        // skips over aren't spuriously badged as "play next".
        const bool linearConsume = m_shuffleMode == ShuffleMode::Off && next.index == m_queueIndex + 1;
        recordForwardStep(m_queueIndex, next.index);
        if (!linearConsume) {
            m_playNextInsertIndex = -1;
        }
        playAt(next.index);
    }
}

void PlayerCore::markVisited(int index)
{
    if (index >= 0 && index < m_queue.size()) {
        m_shuffleVisited.insert(index);
    }
}

void PlayerCore::pushHistory(int index)
{
    if (index >= 0 && index < m_queue.size()) {
        m_shuffleHistory.push_back(index);
    }
}

void PlayerCore::recordForwardStep(int fromIndex, int toIndex)
{
    pushHistory(fromIndex);
    // If the move matches the trail's top, it's a remembered Next: consume it.
    // Otherwise it's a fresh pick that diverges from the trail, which is no longer
    // valid forward navigation, so discard it.
    if (!m_shuffleForward.isEmpty()
        && std::clamp(m_shuffleForward.last(), 0, static_cast<int>(m_queue.size()) - 1) == toIndex) {
        m_shuffleForward.removeLast();
    } else {
        m_shuffleForward.clear();
    }
}

void PlayerCore::playShuffleJump(int index)
{
    // An internal shuffle jump is never a linear consume: force the play-next
    // region to collapse to the new current+1 (playAt's stale-collapse will turn
    // the -1 sentinel into queueIndex+1) so the rows between the old and new
    // positions aren't badged. The forward trail is intentionally preserved.
    m_playNextInsertIndex = -1;
    playAt(index);
}

void PlayerCore::refreshShuffleForManualPick(int previousIndex, int selectedIndex)
{
    if (m_shuffleMode == ShuffleMode::Off) {
        m_shuffleForward.clear();
        return;
    }
    if (previousIndex >= 0 && previousIndex < m_queue.size() && previousIndex != selectedIndex) {
        m_shuffleHistory.push_back(previousIndex);
    }
    m_shuffleForward.clear();
    m_shuffleVisited.clear();
    if (selectedIndex >= 0 && selectedIndex < m_queue.size()) {
        m_shuffleVisited.insert(selectedIndex);
    }
}

void PlayerCore::resetShuffleState()
{
    m_shuffleHistory.clear();
    m_shuffleForward.clear();
    m_shuffleVisited.clear();
    if (m_queueIndex >= 0 && m_queueIndex < m_queue.size()) {
        m_shuffleVisited.insert(m_queueIndex);
    }
}

void PlayerCore::onPreparedTrackStarted()
{
    // The backend started the track we gaplessly preloaded; commit the same row
    // it prepared (which, under shuffle/repeat-all, is not simply index + 1).
    int target = (m_preparedNext.index >= 0 && m_preparedNext.injected.path.isEmpty())
        ? m_preparedNext.index
        : m_queueIndex + 1;
    if (target < 0 || target >= m_queue.size()) {
        return;
    }

    // The outgoing track reached its natural end: the backend never emits
    // finished() when a gaplessly-preloaded track takes over, so signal it here
    // before any state mutation swaps m_currentTrack out.
    if (!m_currentTrack.path.isEmpty()) {
        emit trackFinished(m_currentTrack);
    }

    m_backend->onGaplessTrackAdvanced();
    // Mirror applyAutoNext: only a plain non-shuffle step to the next row consumes
    // the play-next region; a shuffle pick, remembered retrace or repeat-all wrap
    // is a jump that collapses it (so skipped rows aren't badged "play next").
    const bool linearConsume = m_shuffleMode == ShuffleMode::Off && target == m_queueIndex + 1;
    recordForwardStep(m_queueIndex, target);
    m_queueIndex = target;
    if (linearConsume) {
        collapsePlayNextIfStale();
    } else {
        m_playNextInsertIndex = m_queueIndex + 1;
    }
    emit currentIndexChanged(m_queueIndex, /*userInitiated=*/false);
    emit playNextRangeChanged();
    m_currentTrack = m_queue.at(m_queueIndex);
    markVisited(m_queueIndex);
    emit currentTrackChanged(m_currentTrack, /*notifyScrobbler=*/true);
    prepareNext();
}

void PlayerCore::onFinished()
{
    if (m_queue.isEmpty() || m_queueIndex < 0) {
        m_backend->stop();
        return;
    }
    // The backend reported the current track played out to its natural end.
    if (!m_currentTrack.path.isEmpty()) {
        emit trackFinished(m_currentTrack);
    }
    if (m_repeatMode == RepeatMode::One) {
        // Re-play the current track from the top.
        playAt(m_queueIndex);
        return;
    }
    // Decide fresh at finish time: the queue (or shuffle bag) may have changed
    // since prepareNext, and this path runs only when nothing was preloaded.
    const AutoNext target = decideAutoNext();
    if (target.index >= 0 || !target.injected.path.isEmpty()) {
        applyAutoNext(target);
        return;
    }
    // End of queue: tear the pipeline down so hasSource() is false and
    // the output device is freed. A later Play will restart cleanly.
    m_backend->stop();
}
