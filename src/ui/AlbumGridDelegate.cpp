#include "ui/AlbumGridDelegate.h"

#include "ui/StarRating.h"

#include <QApplication>
#include <QPainter>
#include <QTextLayout>

namespace {
enum Roles {
    AlbumTitleRole = Qt::UserRole,
    AlbumArtistRole = Qt::UserRole + 1,
    RatingRole = Qt::UserRole + 2,
    HoverRatingRole = Qt::UserRole + 3,
    SelectedRole = Qt::UserRole + 4,
    TextAlignmentRole = Qt::UserRole + 5,
    ArtSizeRole = Qt::UserRole + 6,
    CellPaddingRole = Qt::UserRole + 7,
    StarSizeRole = Qt::UserRole + 8,
};

QRect alignedRatingCell(const QRect &content, const QRect &textRect, int starSize, Qt::Alignment alignment)
{
    const int width = starSize * 5 + 12;
    int left = content.left();
    if (alignment & Qt::AlignRight) {
        left = content.right() - width + 1;
    } else if (alignment & Qt::AlignHCenter) {
        left = content.left() + ((content.width() - width) / 2);
    }
    return {left, textRect.bottom() + 4, width, starSize};
}
}

AlbumGridDelegate::AlbumGridDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

QSize AlbumGridDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const
{
    const QSize size = index.data(Qt::SizeHintRole).toSize();
    return size.isValid() ? size : QSize(204, 278);
}

void AlbumGridDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    painter->save();

    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);

    QApplication::style()->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, opt.widget);
    if (index.data(SelectedRole).toBool()) {
        painter->setPen(QPen(opt.palette.color(QPalette::Highlight), 2));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(option.rect.adjusted(2, 2, -3, -3), 6, 6);
    }

    const int padding = index.data(CellPaddingRole).toInt() > 0 ? index.data(CellPaddingRole).toInt() : 8;
    const int artSize = index.data(ArtSizeRole).toInt() > 0 ? index.data(ArtSizeRole).toInt() : 176;
    const int starSize = index.data(StarSizeRole).toInt() > 0 ? index.data(StarSizeRole).toInt() : 16;
    const QRect content = option.rect.adjusted(padding, padding, -padding, -padding);
    const QRect artRect(content.left() + ((content.width() - artSize) / 2), content.top(), artSize, artSize);

    const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
    icon.paint(painter, artRect, Qt::AlignCenter, QIcon::Normal, QIcon::Off);

    QRect textRect(content.left(), artRect.bottom() + 6, content.width(), 44);
    painter->setPen(opt.palette.color(QPalette::Text));
    const auto alignment = static_cast<Qt::Alignment>(index.data(TextAlignmentRole).toInt());
    painter->drawText(textRect, alignment | Qt::AlignTop | Qt::TextWordWrap, index.data(Qt::DisplayRole).toString());

    const QRect ratingCell = alignedRatingCell(content, textRect, starSize, alignment);
    StarRating::paint(painter,
                      StarRating::ratingRect(ratingCell, starSize),
                      index.data(RatingRole).toInt(),
                      index.data(HoverRatingRole).toInt(),
                      opt.palette,
                      starSize);

    painter->restore();
}
