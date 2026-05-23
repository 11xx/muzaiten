#include "ui/AlbumGrid.h"

#include "core/Album.h"
#include "core/Rating.h"

#include <QStandardItemModel>

AlbumGrid::AlbumGrid(QWidget *parent)
    : QListView(parent)
{
    setViewMode(QListView::IconMode);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setUniformItemSizes(false);
    setSpacing(12);
    setIconSize(QSize(180, 180));

    auto *model = new QStandardItemModel(this);
    auto *item = new QStandardItem(QStringLiteral("No library loaded"));
    item->setEditable(false);
    model->appendRow(item);
    setModel(model);
}

void AlbumGrid::setAlbums(const QVector<Album> &albums)
{
    auto *itemModel = qobject_cast<QStandardItemModel *>(model());
    itemModel->clear();

    for (const Album &album : albums) {
        QString label = album.title;
        if (!album.date.isEmpty()) {
            label += QStringLiteral("\n%1").arg(album.date.left(4));
        }
        if (album.averageRating0To100 >= 0) {
            label += QStringLiteral("\n%1").arg(Rating::displayText(album.averageRating0To100));
        }

        auto *item = new QStandardItem(label);
        item->setEditable(false);
        item->setData(album.title, Qt::UserRole);
        item->setToolTip(QStringLiteral("%1 tracks").arg(album.trackCount));
        itemModel->appendRow(item);
    }
}
