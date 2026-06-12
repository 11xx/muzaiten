#pragma once

#include "core/Track.h"
#include "search/SearchMatcher.h"

#include <QObject>
#include <QVector>

class QueueStore final : public QObject {
    Q_OBJECT

public:
    explicit QueueStore(QObject *parent = nullptr);

    void setSnapshot(const QVector<Track> &tracks, int currentIndex, int playNextBegin, int playNextEnd);
    void updateTrackRating(const QString &path, int rating0To100, bool hasUserRating);
    void setTracks(const QVector<Track> &tracks);
    void setCurrentIndex(int index);
    void setPlayNextRange(int begin, int end);

    const QVector<Track> &tracks() const { return m_tracks; }
    int currentIndex() const { return m_currentIndex; }
    int playNextBegin() const { return m_playNextBegin; }
    int playNextEnd() const { return m_playNextEnd; }
    QVector<Search::MatchDocument> searchDocuments() const;

signals:
    void tracksAboutToReset();
    void tracksReset();
    void trackChanged(int row);
    void currentIndexChanged(int currentIndex);
    void playNextRangeChanged(int begin, int end);

private:
    QVector<Track> m_tracks;
    int m_currentIndex = -1;
    int m_playNextBegin = -1;
    int m_playNextEnd = -1;
};
