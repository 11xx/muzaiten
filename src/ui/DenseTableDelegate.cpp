#include "ui/DenseTableDelegate.h"

#include "ui/SelectionColors.h"

#include <QPainter>
#include <QStyleOptionViewItem>
#include <QWidget>

namespace {

constexpr int kCellHorizontalPadding = 3;

} // namespace

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
    // Row-based hover only (m_hoveredRow tracks the cursor on move and scroll);
    // never the lone cell under the pointer.
    const bool hovered = (m_hoveredRow == index.row());

    if (selected) {
        painter->fillRect(opt.rect, SelectionColors::selectedFill(opt));
    } else if (hovered) {
        QColor hover = opt.palette.color(QPalette::Highlight);
        hover.setAlpha(34);
        painter->fillRect(opt.rect, hover);
    } else if (index.row() % 2 == 1) {
        painter->fillRect(opt.rect, opt.palette.color(QPalette::AlternateBase));
    }

    if (selected) {
        SelectionColors::applySelectedPalette(&opt);
    } else {
        opt.state &= ~QStyle::State_MouseOver;
    }
    opt.state &= ~QStyle::State_MouseOver;
    opt.state &= ~QStyle::State_Selected;
    // The style otherwise paints a focus rectangle (a faint rounded outline) on
    // the clicked cell. We draw our own selection/hover fills, so suppress it.
    opt.state &= ~QStyle::State_HasFocus;
    opt.features &= ~QStyleOptionViewItem::Alternate;
    opt.backgroundBrush = Qt::NoBrush;
    opt.displayAlignment = opt.displayAlignment | Qt::AlignVCenter;
    opt.rect.adjust(kCellHorizontalPadding, 0, -kCellHorizontalPadding, 0);
    QStyledItemDelegate::paint(painter, opt, index);
}
