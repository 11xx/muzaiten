#pragma once

#include <QListView>
#include <QVector>

class Album;
class QEvent;
class QMouseEvent;

class AlbumGrid final : public QListView {
    Q_OBJECT

public:
    explicit AlbumGrid(QWidget *parent = nullptr);

    void setArtworkCacheRoot(const QString &cacheRoot);
    void setAlbums(const QVector<Album> &albums);
    void setSelectedAlbumTitle(const QString &albumTitle);
    QString viewSettingsJson() const;
    void applyViewSettingsJson(const QString &json);

signals:
    void albumSelectionToggled(const QString &albumTitle);
    void albumPlayNextRequested(const QString &albumTitle);
    void albumAddToQueueRequested(const QString &albumTitle);
    void albumRatingChanged(const QString &albumArtistName, const QString &albumTitle, int rating0To100);
    void viewSettingsChanged();

protected:
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QRect ratingRectForIndex(const QModelIndex &index) const;
    void showContextMenu(const QPoint &pos);
    void applySettingsToView();
    void applySettingsToItems();

private:
    QString m_artworkCacheRoot;
    QString m_selectedAlbumTitle;
    int m_cellWidth = 204;
    int m_cellHeight = 278;
    int m_artSize = 176;
    int m_spacing = 6;
    int m_padding = 8;
    int m_starSize = 18;
    Qt::Alignment m_textAlignment = Qt::AlignHCenter;
};
