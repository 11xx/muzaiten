#pragma once

#include <QTableView>
#include <QVector>

class Track;

class TrackTable final : public QTableView {
    Q_OBJECT

public:
    explicit TrackTable(QWidget *parent = nullptr);

    void setTracks(const QVector<Track> &tracks);
};
