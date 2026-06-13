#include "player/PlayerCore.h"

#include "playback/PlaybackBackend.h"

#include <QHash>
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

    m_currentTrack = track;
    emit currentTrackChanged(m_currentTrack, notifyScrobbler);
    if (startPaused) {
        m_backend->loadPaused(QUrl::fromLocalFile(playbackPath));
    } else {
        m_backend->play(QUrl::fromLocalFile(playbackPath));
    }
    prepareNext();
}

void PlayerCore::next()
{
    if (m_queue.isEmpty()) {
        return;
    }
    playAt(std::min(static_cast<int>(m_queue.size() - 1), m_queueIndex + 1));
}

void PlayerCore::previous()
{
    if (m_queue.isEmpty()) {
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
            playAt(index);
            return;
        }
    }

    emit aboutToAddTracks(QVector<Track>{track});
    m_queue.push_back(track);
    emit queueChanged();
    playAt(static_cast<int>(m_queue.size() - 1));
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
    prepareNext();
    emit queueChanged();
}

void PlayerCore::removeRows(const QVector<int> &rows)
{
    if (rows.isEmpty() || m_queue.isEmpty()) {
        return;
    }

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
    prepareNext();
    emit queueChanged();
    if (m_queueIndex >= 0 && m_queueIndex < m_queue.size()) {
        presentTrack(m_queue.at(m_queueIndex));
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
}

void PlayerCore::presentTrack(const Track &track)
{
    m_currentTrack = track;
    emit currentTrackChanged(m_currentTrack, /*notifyScrobbler=*/false);
}

void PlayerCore::prepareNext()
{
    if (m_queueIndex < 0 || m_queueIndex + 1 >= m_queue.size()) {
        m_backend->prepareNext({});
        return;
    }
    const Track &nextTrack = m_queue.at(m_queueIndex + 1);
    const QString nextPath = resolvePath(nextTrack);
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

void PlayerCore::onPreparedTrackStarted()
{
    if (m_queueIndex + 1 >= m_queue.size()) {
        return;
    }

    m_backend->onGaplessTrackAdvanced();
    ++m_queueIndex;
    collapsePlayNextIfStale();
    emit currentIndexChanged(m_queueIndex, /*userInitiated=*/false);
    emit playNextRangeChanged();
    m_currentTrack = m_queue.at(m_queueIndex);
    emit currentTrackChanged(m_currentTrack, /*notifyScrobbler=*/true);
    prepareNext();
}

void PlayerCore::onFinished()
{
    if (m_queueIndex + 1 < m_queue.size()) {
        playAt(m_queueIndex + 1);
    } else {
        // End of queue: tear the pipeline down so hasSource() is false and
        // the output device is freed. A later Play will restart cleanly.
        m_backend->stop();
    }
}
