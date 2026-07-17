#pragma once
#include <QObject>
#include <QVector>
#include "core/Track.h"
class MainWindow;
class RatingSyncController final : public QObject {
public:
    explicit RatingSyncController(MainWindow &window);
    void applyTrackRating(const Track &, int, const QString &);
    void startRatingTagSync(const QVector<Track> &, int);
    void schedulePendingRatingTagSync();
    void syncCurrentTrackRatingTags();
    void syncCurrentArtistRatingTags();
    void syncAllSavedRatingTags();
    void retryPendingRatingTags();
    void applyAlbumRating(const QString &, const QString &, int);
private:
    MainWindow &m_window;
};
