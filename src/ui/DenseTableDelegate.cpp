#include "ui/DenseTableDelegate.h"

#include <QPainter>
#include <QStyleOptionViewItem>
#include <QWidget>

DenseTableDelegate::DenseTableDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

void DenseTableDelegate::setHoveredRow(int row)
{
    m_hoveredRow = row;
}

void DenseTableDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);

    const bool selected = opt.state & QStyle::State_Selected;
    const bool hovered = (m_hoveredRow == index.row()) || (opt.state & QStyle::State_MouseOver);

    if (selected) {
        painter->fillRect(opt.rect, opt.palette.color(QPalette::Highlight));
    } else if (hovered) {
        QColor hover = opt.palette.color(QPalette::Highlight);
        hover.setAlpha(34);
        painter->fillRect(opt.rect, hover);
    } else if (index.row() % 2 == 1) {
        painter->fillRect(opt.rect, opt.palette.color(QPalette::AlternateBase));
    }

    if (selected) {
        opt.palette.setColor(QPalette::Text, opt.palette.color(QPalette::HighlightedText));
    } else {
        opt.state &= ~QStyle::State_MouseOver;
        opt.state &= ~QStyle::State_Selected;
    }
    opt.features &= ~QStyleOptionViewItem::Alternate;
    opt.backgroundBrush = Qt::NoBrush;
    opt.displayAlignment = opt.displayAlignment | Qt::AlignVCenter;
    QStyledItemDelegate::paint(painter, opt, index);
}
