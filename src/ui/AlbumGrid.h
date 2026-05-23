#pragma once

#include <QListView>
#include <QVector>

class Album;

class AlbumGrid final : public QListView {
    Q_OBJECT

public:
    explicit AlbumGrid(QWidget *parent = nullptr);

    void setArtworkCacheRoot(const QString &cacheRoot);
    void setAlbums(const QVector<Album> &albums);

signals:
    void albumSelected(const QString &albumTitle);

private:
    QString m_artworkCacheRoot;
};
