#include "ui/TrackTable.h"

#include "core/Track.h"
#include "ui/StarRatingDelegate.h"

#include <QHeaderView>
#include <QStandardItemModel>
#include <QTime>

TrackTable::TrackTable(QWidget *parent)
    : QTableView(parent)
{
    auto *model = new QStandardItemModel(0, 7, this);
    model->setHorizontalHeaderLabels({
        QStringLiteral("Rating"),
        QStringLiteral("#"),
        QStringLiteral("Title"),
        QStringLiteral("Album"),
        QStringLiteral("Artist"),
        QStringLiteral("Duration"),
        QStringLiteral("Year"),
    });

    setModel(model);
    setItemDelegateForColumn(0, new StarRatingDelegate(this));
    setSortingEnabled(true);
    setAlternatingRowColors(true);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    horizontalHeader()->setStretchLastSection(false);
    horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    verticalHeader()->setVisible(false);
}

void TrackTable::setTracks(const QVector<Track> &tracks)
{
    auto *itemModel = qobject_cast<QStandardItemModel *>(model());
    itemModel->setRowCount(0);

    for (const Track &track : tracks) {
        QList<QStandardItem *> row;

        auto *rating = new QStandardItem(track.rating0To100 >= 0 ? QString::number(track.rating0To100) : QStringLiteral("-"));
        rating->setData(track.rating0To100, Qt::UserRole);
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
