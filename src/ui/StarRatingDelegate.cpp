#include "ui/StarRatingDelegate.h"

#include "core/Rating.h"

#include <QPainter>

StarRatingDelegate::StarRatingDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

void StarRatingDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const int value = index.data(Qt::UserRole).toInt();
    if (value < 0) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->drawText(option.rect.adjusted(6, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, Rating::displayText(value));
    painter->restore();
}

