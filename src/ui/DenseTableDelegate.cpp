#include "ui/DenseTableDelegate.h"

#include <QHeaderView>
#include <QPainter>
#include <QStyleOptionViewItem>
#include <QTableView>
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

    auto *view = qobject_cast<const QTableView *>(opt.widget);
    bool firstVisibleColumn = true;
    if (view != nullptr && view->horizontalHeader() != nullptr) {
        firstVisibleColumn = false;
        for (int visual = 0; visual < view->horizontalHeader()->count(); ++visual) {
            const int logical = view->horizontalHeader()->logicalIndex(visual);
            if (!view->horizontalHeader()->isSectionHidden(logical)) {
                firstVisibleColumn = logical == index.column();
                break;
            }
        }
    }
    const bool selected = opt.state & QStyle::State_Selected;
    const bool hovered = (m_hoveredRow == index.row()) || (opt.state & QStyle::State_MouseOver);

    if (opt.widget != nullptr && firstVisibleColumn) {
        const QRect rowRect(0, opt.rect.top(), opt.widget->width(), opt.rect.height());
        if (selected) {
            painter->fillRect(rowRect, opt.palette.color(QPalette::Highlight));
        } else if (hovered) {
            QColor hover = opt.palette.color(QPalette::Highlight);
            hover.setAlpha(34);
            painter->fillRect(rowRect, hover);
        } else if (index.row() % 2 == 1) {
            painter->fillRect(rowRect, opt.palette.color(QPalette::AlternateBase));
        }
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
