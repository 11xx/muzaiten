#include "ui/QueueStore.h"

#include <QFileInfo>

#include <algorithm>

namespace {

QString displayYear(const Track &track)
{
    for (const QString &candidate : {track.originalDate, track.date}) {
        const QString trimmed = candidate.trimmed();
        if (!trimmed.isEmpty()) {
            return trimmed.left(4);
        }
    }
    return {};
}

QString displayTitle(const Track &track)
{
    if (!track.title.trimmed().isEmpty()) {
        return track.title;
    }
    const QString file = track.filename.isEmpty() ? track.path : track.filename;
    const QString base = QFileInfo(file).completeBaseName();
    return base.isEmpty() ? file : base;
}

} // namespace

QueueStore::QueueStore(QObject *parent)
    : QObject(parent)
{
}

void QueueStore::setSnapshot(const QVector<Track> &tracks, int currentIndex, int playNextBegin, int playNextEnd)
{
    setTracks(tracks);
    setCurrentIndex(currentIndex);
    setPlayNextRange(playNextBegin, playNextEnd);
}

void QueueStore::updateTrack(int row, const Track &track)
{
    if (row < 0 || row >= m_tracks.size()) {
        return;
    }
    m_tracks[row] = track;
    emit trackChanged(row);
}

void QueueStore::updateTrackRating(const QString &path, int rating0To100, bool hasUserRating)
{
    // Per-row patch so a rating edit doesn't reset the whole queue model
    // (which drops scroll position and selection in the queue views).
    for (int row = 0; row < m_tracks.size(); ++row) {
        Track &track = m_tracks[row];
        if (track.path != path) {
            continue;
        }
        track.hasUserRating = hasUserRating;
        track.effectiveRating0To100 = hasUserRating ? rating0To100 : track.rating0To100;
        emit trackChanged(row);
    }
}

void QueueStore::setTracks(const QVector<Track> &tracks)
{
    emit tracksAboutToReset();
    m_tracks = tracks;
    if (m_tracks.isEmpty()) {
        m_currentIndex = -1;
    } else {
        m_currentIndex = std::clamp(m_currentIndex, -1, static_cast<int>(m_tracks.size()) - 1);
    }
    emit tracksReset();
    emit currentIndexChanged(m_currentIndex);
}

void QueueStore::setCurrentIndex(int index)
{
    const int clamped = m_tracks.isEmpty() ? -1 : std::clamp(index, -1, static_cast<int>(m_tracks.size()) - 1);
    if (m_currentIndex == clamped) {
        return;
    }
    m_currentIndex = clamped;
    emit currentIndexChanged(m_currentIndex);
}

void QueueStore::setPlayNextRange(int begin, int end)
{
    if (m_playNextBegin == begin && m_playNextEnd == end) {
        return;
    }
    m_playNextBegin = begin;
    m_playNextEnd = end;
    emit playNextRangeChanged(begin, end);
}

QVector<Search::MatchDocument> QueueStore::searchDocuments() const
{
    QVector<Search::MatchDocument> docs;
    docs.reserve(m_tracks.size());
    for (int row = 0; row < m_tracks.size(); ++row) {
        const Track &track = m_tracks.at(row);
        const QString title = displayTitle(track);
        const QString free = QStringLiteral("%1 %2 %3 %4 %5")
                                 .arg(title,
                                      track.artistName,
                                      track.albumArtistName,
                                      track.albumTitle,
                                      track.path);
        QVector<Search::MatchNumeric> numeric;
        const int year = displayYear(track).toInt();
        if (year > 0) numeric.push_back({Search::TermKind::Year, year});
        if (track.effectiveRating0To100 >= 0) numeric.push_back({Search::TermKind::Rating, track.effectiveRating0To100});
        if (track.durationMs > 0) numeric.push_back({Search::TermKind::DurationMs, track.durationMs});
        docs.push_back({
            row,
            {
                Search::makeField(Search::MatchFieldRole::Title, title, 400),
                Search::makeField(Search::MatchFieldRole::Artist, track.artistName, 300),
                Search::makeField(Search::MatchFieldRole::AlbumArtist, track.albumArtistName, 300),
                Search::makeField(Search::MatchFieldRole::Album, track.albumTitle, 200),
                Search::makeField(Search::MatchFieldRole::Filename, track.filename, 60),
                Search::makeField(Search::MatchFieldRole::Path, track.path, 60),
                Search::makeField(Search::MatchFieldRole::Free, free, 100),
            },
            numeric,
        });
    }
    return docs;
}
