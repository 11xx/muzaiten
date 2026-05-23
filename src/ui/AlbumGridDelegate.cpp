#include "ui/AlbumGridDelegate.h"

#include <QApplication>
#include <QPainter>
#include <QTextLayout>

namespace {
constexpr int cellWidth = 220;
constexpr int cellHeight = 292;
constexpr int artSize = 184;
constexpr int padding = 10;
}

AlbumGridDelegate::AlbumGridDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

QSize AlbumGridDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return {cellWidth, cellHeight};
}

void AlbumGridDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    painter->save();

    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);

    QApplication::style()->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, opt.widget);

    const QRect content = option.rect.adjusted(padding, padding, -padding, -padding);
    const QRect artRect(content.left() + ((content.width() - artSize) / 2), content.top(), artSize, artSize);

    const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
    icon.paint(painter, artRect, Qt::AlignCenter, QIcon::Normal, QIcon::Off);

    QRect textRect(content.left(), artRect.bottom() + 8, content.width(), content.bottom() - artRect.bottom() - 8);
    painter->setPen(opt.palette.color(QPalette::Text));
    painter->drawText(textRect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, index.data(Qt::DisplayRole).toString());

    painter->restore();
}

