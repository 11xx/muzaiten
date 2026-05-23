#pragma once

#include <QTableView>
#include <QVector>

#include "core/Track.h"

class TrackTable final : public QTableView {
    Q_OBJECT

public:
    explicit TrackTable(QWidget *parent = nullptr);

    void setTracks(const QVector<Track> &tracks);

signals:
    void trackActivated(const Track &track);
};
