#pragma once

#include <QWidget>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include "core/Track.h"
#include "search/SearchMatcher.h"
#include "ui/QueueTable.h"

class Database;
class QImage;
class QLabel;
class QueueStore;
class QSplitter;
class TrackInfoPanel;
class QWidget;

class RightSidebar final : public QWidget {
    Q_OBJECT

public:
    explicit RightSidebar(QWidget *parent = nullptr);

    void setDatabase(const Database *db) { m_database = db; }
    void setQueueStore(QueueStore *store);
    void setQueue(const QVector<Track> &tracks);
    void setPlayNextRange(int begin, int end);
    void setCurrentIndex(int index, bool reveal = false);
    void setAlbumArt(const QString &imagePath);
    void setAlbumArt(const QImage &image);
    void setTrackInfo(const Track &track);
    void setTrackInfoVisible(bool visible);
    void configureTrackInfoPanel(QWidget *parent);
    QString viewSettingsJson() const;
    void applyViewSettingsJson(const QString &json);
    void resetViewSettings();
    void setHeaderHeight(int height);
    void setNavigationScrollPadding(int rows);
    QWidget *queueNavigationWidget() const;
    int queueRowCount() const;
    int queueCurrentRow() const;
    void setQueueCurrentRow(int row);
    void setQueueCurrentRow(int row, int scrollDirection);
    void moveQueueCurrentRow(int delta);
    void activateCurrentQueueTrack();
    void setQueueIsPlaylistSourced(bool sourced);
    QVector<Search::MatchDocument> queueSearchDocuments() const;

signals:
    void queueTrackActivated(int index);
    void queueTrackRatingChanged(const Track &track, int rating0To100);
    void queueRowsMoveRequested(const QVector<int> &rows, int destinationRow);
    void queueRowsRemoveRequested(const QVector<int> &rows);
    void removeAllMissingTracksRequested();
    void queueClearRequested();
    void clearPlayNextPriorityRequested();
    void saveQueueAsRequested();
    void restorePreviousQueueRequested();
    void unlinkQueueFromPlaylistRequested();
    void artistRequested(const QString &artistName);
    void albumRequested(const QString &artistName, const QString &albumTitle);
    void findFileRequested(const Track &track);
    void propertiesRequested(const Track &track);
    void startRadioRequested(const Track &track);
    void trackLibraryRequested(const Track &track);
    void viewSettingsChanged();

protected:
    void changeEvent(QEvent *event) override;

private:
    const Database *m_database = nullptr;
    QueueTable *m_queueTable = nullptr;
    QueueStore *m_queueStore = nullptr;
    QLabel *m_albumArt = nullptr;
    TrackInfoPanel *m_trackInfoPanel = nullptr;
    QSplitter *m_splitter = nullptr;
    // Sizes persisted to settings — only updated by a real splitterMoved (user
    // drag), never by programmatic setSizes() or transient/relaid-out
    // distributions. See SplitterPersistence.h.
    QList<int> m_userSplitterSizes;
    QVector<Track> m_tracks;
    int m_queueHoveredRow = -1;
    int m_queueDropIndicatorRow = -1;
    int m_currentQueueIndex = -1;
    int m_playNextBegin = -1;
    int m_playNextEnd = -1;
    bool m_showPlayNextBadge = true;
    bool m_showPlayNextTitleAccent = false;
    bool m_usingArtFallback = true;
};
