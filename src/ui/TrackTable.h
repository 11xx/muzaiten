#pragma once

#include <QTableView>
#include <QVector>
#include <QPersistentModelIndex>

#include "core/Track.h"

class QEvent;
class QMouseEvent;

class TrackTable final : public QTableView {
    Q_OBJECT

public:
    explicit TrackTable(QWidget *parent = nullptr);

    void setTracks(const QVector<Track> &tracks);
    QString viewSettingsJson() const;
    void applyViewSettingsJson(const QString &json);
    int sortColumn() const;
    Qt::SortOrder sortOrder() const;
    int verticalScrollValue() const;
    void restoreViewState(int sortColumn, Qt::SortOrder sortOrder, int verticalScrollValue);

signals:
    void trackActivated(const Track &track);
    void trackRatingChanged(const Track &track, int rating0To100);
    void viewSettingsChanged();

protected:
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void showHeaderMenu(const QPoint &pos);
    void showCellMenu(const QPoint &pos);

    QPersistentModelIndex m_hoverRatingIndex;
};
