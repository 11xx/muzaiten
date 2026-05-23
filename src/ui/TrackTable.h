#pragma once

#include <QTableView>
#include <QVector>

#include "core/Track.h"

class TrackTable final : public QTableView {
    Q_OBJECT

public:
    explicit TrackTable(QWidget *parent = nullptr);

    void setTracks(const QVector<Track> &tracks);
    int sortColumn() const;
    Qt::SortOrder sortOrder() const;
    int verticalScrollValue() const;
    void restoreViewState(int sortColumn, Qt::SortOrder sortOrder, int verticalScrollValue);

signals:
    void trackActivated(const Track &track);
};
