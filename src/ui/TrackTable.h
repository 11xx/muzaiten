#pragma once

#include <QTableView>
#include <QVector>
#include <QPersistentModelIndex>

#include "core/Track.h"

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
    int sortColumn() const;
    Qt::SortOrder sortOrder() const;
    int verticalScrollValue() const;
    void restoreViewState(int sortColumn, Qt::SortOrder sortOrder, int verticalScrollValue);

signals:
    void trackActivated(const Track &track);
    void playNextRequested(const QVector<Track> &tracks);
    void addToQueueRequested(const QVector<Track> &tracks);
    void findFileRequested(const Track &track);
    void trackRatingChanged(const Track &track, int rating0To100);
    void viewSettingsChanged();

protected:
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    void showHeaderMenu(const QPoint &pos);
    void showCellMenu(const QPoint &pos);
    QVector<Track> tracksForContextRow(int row) const;

    QPersistentModelIndex m_hoverRatingIndex;
};
