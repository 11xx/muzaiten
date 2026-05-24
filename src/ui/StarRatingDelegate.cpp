#include "ui/StarRatingDelegate.h"

#include "ui/StarRating.h"

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWidget>

StarRatingDelegate::StarRatingDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

bool StarRatingDelegate::editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index)
{
    if (!index.isValid() || model == nullptr) {
        return false;
    }

    const QRect rect = StarRating::ratingRect(option.rect, 18);
    if (event->type() == QEvent::MouseMove) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        const int hoverRating = StarRating::ratingFromPosition(rect, mouse->pos());
        model->setData(index, hoverRating, Qt::UserRole + 2);
        return true;
    }

    if (event->type() == QEvent::MouseButtonRelease) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton) {
            return false;
        }
        const int rating = StarRating::ratingFromPosition(rect, mouse->pos());
        if (rating >= 0) {
            emit ratingEdited(index, rating);
            return true;
        }
    }

    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

void StarRatingDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    if ((option.state & QStyle::State_MouseOver) && !(option.state & QStyle::State_Selected) && option.widget != nullptr) {
        painter->fillRect(QRect(0, option.rect.top(), option.widget->width(), option.rect.height()), option.palette.color(QPalette::AlternateBase));
    }
    const int value = index.data(Qt::UserRole).toInt();
    const int hoverValue = index.data(Qt::UserRole + 2).isValid() ? index.data(Qt::UserRole + 2).toInt() : StarRating::unset;
    StarRating::paint(painter, StarRating::ratingRect(option.rect, 18), value, hoverValue, option.palette, 18);
}
