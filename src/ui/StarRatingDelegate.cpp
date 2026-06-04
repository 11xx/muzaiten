#include "ui/StarRatingDelegate.h"

#include "ui/SelectionColors.h"
#include "ui/StarRating.h"

#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWidget>

StarRatingDelegate::StarRatingDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

void StarRatingDelegate::setHoveredRow(int row)
{
    m_hoveredRow = row;
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
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);

    const bool selected = opt.state & QStyle::State_Selected;
    // Row-based hover only (m_hoveredRow tracks the cursor on move and scroll);
    // never the lone cell under the pointer.
    const bool hovered = (m_hoveredRow == index.row());
    if (selected) {
        painter->fillRect(opt.rect, SelectionColors::selectedFill(opt));
        SelectionColors::applySelectedPalette(&opt);
    } else if (hovered) {
        QColor hover = opt.palette.color(QPalette::Highlight);
        hover.setAlpha(34);
        painter->fillRect(opt.rect, hover);
    } else if (index.row() % 2 == 1) {
        painter->fillRect(opt.rect, opt.palette.color(QPalette::AlternateBase));
    }
    opt.state &= ~QStyle::State_MouseOver;
    opt.state &= ~QStyle::State_Selected;

    const int value = index.data(Qt::UserRole).toInt();
    const int hoverValue = index.data(Qt::UserRole + 2).isValid() ? index.data(Qt::UserRole + 2).toInt() : StarRating::unset;
    StarRating::paint(painter, StarRating::ratingRect(opt.rect, 18), value, hoverValue, opt.palette, 18);
}
