#pragma once

#include <QListView>
#include <QVector>

class Album;

class AlbumGrid final : public QListView {
    Q_OBJECT

public:
    explicit AlbumGrid(QWidget *parent = nullptr);

    void setAlbums(const QVector<Album> &albums);
};
