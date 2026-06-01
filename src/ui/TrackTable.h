#pragma once

#include <QTableView>
#include <QVector>
#include <QPersistentModelIndex>

#include "core/Track.h"
#include "search/SearchMatcher.h"

class QEvent;
class QMouseEvent;
class QWheelEvent;

class TrackTable final : public QTableView {
    Q_OBJECT

public:
    explicit TrackTable(QWidget *parent = nullptr);

    void setTracks(const QVector<Track> &tracks);
    QString viewSettingsJson() const;
    void applyViewSettingsJson(const QString &json);
    void setHeaderHeight(int height);
    void setNavigationScrollPadding(int rows);
    int sortColumn() const;
    Qt::SortOrder sortOrder() const;
    int verticalScrollValue() const;
    void restoreViewState(int sortColumn, Qt::SortOrder sortOrder, int verticalScrollValue);
    // Selects and scrolls to the row whose track has this path (no-op if absent).
    void selectTrackByPath(const QString &path);
    int rowCount() const;
    int currentRow() const;
    void setCurrentRow(int row);
    void setCurrentRow(int row, int scrollDirection);
    void moveCurrentRow(int delta);
    void activateCurrentTrack();
    QVector<Search::MatchDocument> searchDocuments() const;

signals:
    void trackActivated(const Track &track);
    void playNextRequested(const QVector<Track> &tracks);
    void addToQueueRequested(const QVector<Track> &tracks);
    void findFileRequested(const Track &track);
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
    void setHoveredRow(int row);

    QPersistentModelIndex m_hoverRatingIndex;
    int m_hoveredRow = -1;
    int m_navigationScrollPadding = 3;
};
