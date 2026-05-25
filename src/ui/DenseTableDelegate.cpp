#include "ui/DenseTableDelegate.h"

#include <QPainter>
#include <QWidget>

DenseTableDelegate::DenseTableDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

void DenseTableDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyleOptionViewItem opt(option);
    if ((opt.state & QStyle::State_MouseOver) && !(opt.state & QStyle::State_Selected) && opt.widget != nullptr) {
        QColor hover = opt.palette.color(QPalette::Highlight);
        hover.setAlpha(34);
        painter->fillRect(QRect(0, opt.rect.top(), opt.widget->width(), opt.rect.height()), hover);
        opt.state &= ~QStyle::State_MouseOver;
    }
    opt.displayAlignment = opt.displayAlignment | Qt::AlignVCenter;
    QStyledItemDelegate::paint(painter, opt, index);
}
