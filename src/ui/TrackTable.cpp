#include "ui/TrackTable.h"

#include "ui/StarRatingDelegate.h"

#include <QHeaderView>
#include <QStandardItemModel>

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

