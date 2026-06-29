#pragma once

#include <QSet>
#include <QVector>
#include <QPersistentModelIndex>

#include "core/Track.h"
#include "search/SearchMatcher.h"
#include "ui/NavigableTableView.h"

class QEvent;
class QMouseEvent;
class QWheelEvent;
class ResponsiveColumnLayout;

enum class TrackTableChrome {
    Panel,
    Inline,
};

class TrackTable final : public NavigableTableView {
    Q_OBJECT

public:
    explicit TrackTable(QWidget *parent = nullptr);

    void setTracks(const QVector<Track> &tracks);
    void setChrome(TrackTableChrome chrome);
    TrackTableChrome chrome() const { return m_chrome; }
    void setAutoHeightToRows(bool autoHeight);
    QString viewSettingsJson() const;
    void applyViewSettingsJson(const QString &json);
    void resetViewSettings();
    void setHeaderHeight(int height);
    int sortColumn() const;
    Qt::SortOrder sortOrder() const;
    int verticalScrollValue() const;
    void restoreViewState(int sortColumn, Qt::SortOrder sortOrder, int verticalScrollValue);
    // In-place rating patch for the row(s) matching this path; avoids a full
    // setTracks() rebuild when a rating is edited or synced. No-op if absent.
    void updateTrackRating(const QString &path, int effectiveRating, bool hasUserRating);
    // Selects and scrolls to the row whose track has this path (no-op if absent).
    void selectTrackByPath(const QString &path);
    int rowCount() const;
    int currentRow() const;
    void setCurrentRow(int row);
    void setCurrentRow(int row, int scrollDirection);
    void moveCurrentRow(int delta);
    void activateCurrentTrack();
    void addCurrentTrackToQueue();
    void addCurrentTrackToPlaylist();
    void playNextCurrentTrack();
    void markCurrentTrack();
    void markAllTracks();
    void unmarkCurrentTrack();
    void unmarkAllTracks();
    QVector<Search::MatchDocument> searchDocuments() const;
    // When true, the context menu also offers "(don't save to playlist)" queue
    // adds (the queue is mirroring a playlist).
    void setQueueIsPlaylistSourced(bool sourced) { m_queueIsPlaylistSourced = sourced; }

signals:
    void trackActivated(const Track &track);
    void playNextRequested(const QVector<Track> &tracks);
    void addToQueueRequested(const QVector<Track> &tracks);
    // "(don't save to playlist)" variants: add to the queue only, never mirroring
    // into the playlist that backs the queue.
    void playNextTemporaryRequested(const QVector<Track> &tracks);
    void addToQueueTemporaryRequested(const QVector<Track> &tracks);
    void addToPlaylistRequested(const QVector<Track> &tracks);
    void findFileRequested(const Track &track);
    void propertiesRequested(const Track &track);
    void trackRatingChanged(const Track &track, int rating0To100);
    void viewSettingsChanged();

protected:
    void changeEvent(QEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    void showHeaderMenu(const QPoint &pos);
    void showCellMenu(const QPoint &pos);
    QVector<Track> tracksForContextRow(int row) const;
    QVector<Track> tracksForActionRow(int row) const;
    void reselectMarkedRows();
    void setCurrentTrackMarked(bool marked);
    void setHoveredRow(int row);
    void updateHoverFromCursor();

    QPersistentModelIndex m_hoverRatingIndex;
    QSet<QString> m_markedTrackPaths;
    ResponsiveColumnLayout *m_columnLayout = nullptr;
    int m_hoveredRow = -1;
    bool m_queueIsPlaylistSourced = false;
    TrackTableChrome m_chrome = TrackTableChrome::Panel;
    bool m_autoHeightToRows = false;
};
