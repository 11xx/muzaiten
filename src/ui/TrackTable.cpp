#include "ui/TrackTable.h"

#include "core/Track.h"
#include "ui/StarRatingDelegate.h"

#include <QHeaderView>
#include <QStandardItemModel>
#include <QTime>

TrackTable::TrackTable(QWidget *parent)
    : QTableView(parent)
{
    auto *itemModel = new QStandardItemModel(0, 7, this);
    itemModel->setHorizontalHeaderLabels({
        QStringLiteral("Rating"),
        QStringLiteral("#"),
        QStringLiteral("Title"),
        QStringLiteral("Album"),
        QStringLiteral("Artist"),
        QStringLiteral("Duration"),
        QStringLiteral("Year"),
    });

    setModel(itemModel);
    setItemDelegateForColumn(0, new StarRatingDelegate(this));
    setSortingEnabled(true);
    setAlternatingRowColors(true);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    horizontalHeader()->setStretchLastSection(false);
    horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    verticalHeader()->setVisible(false);

    connect(this, &QTableView::doubleClicked, this, [this](const QModelIndex &index) {
        const QModelIndex ratingIndex = this->model()->index(index.row(), 0);
        const Track track = ratingIndex.data(Qt::UserRole + 1).value<Track>();
        if (!track.path.isEmpty()) {
            emit trackActivated(track);
        }
    });
}

void TrackTable::setTracks(const QVector<Track> &tracks)
{
    auto *itemModel = qobject_cast<QStandardItemModel *>(model());
    itemModel->setRowCount(0);

    for (const Track &track : tracks) {
        QList<QStandardItem *> row;

        auto *rating = new QStandardItem(track.rating0To100 >= 0 ? QString::number(track.rating0To100) : QStringLiteral("-"));
        rating->setData(track.rating0To100, Qt::UserRole);
        rating->setData(QVariant::fromValue(track), Qt::UserRole + 1);
        row << rating;

        auto *trackNumber = new QStandardItem(QString::number(track.trackNumber));
        trackNumber->setData(track.trackNumber, Qt::EditRole);
        row << trackNumber;

        row << new QStandardItem(track.title);
        row << new QStandardItem(track.albumTitle);
        row << new QStandardItem(track.artistName);

        const QTime duration = QTime(0, 0).addMSecs(static_cast<int>(track.durationMs));
        row << new QStandardItem(duration.hour() > 0 ? duration.toString(QStringLiteral("h:mm:ss")) : duration.toString(QStringLiteral("m:ss")));
        row << new QStandardItem(track.date.left(4));

        for (QStandardItem *item : row) {
            item->setEditable(false);
        }
        itemModel->appendRow(row);
    }
}
