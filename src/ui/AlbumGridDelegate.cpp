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

QString titleFromDisplay(const QString &display)
{
    return display.section(QLatin1Char('\n'), 0, 0);
}

QString yearFromDisplay(const QString &display)
{
    return display.section(QLatin1Char('\n'), 1, 1);
}

QRect alignedRatingCell(const QRect &anchorRect, int top, int starSize, Qt::Alignment alignment)
{
    const int starsWidth = starSize * 5;
    const int hitPadding = 6;
    int left = anchorRect.left() - hitPadding;
    if (alignment & Qt::AlignRight) {
        left = anchorRect.right() - starsWidth + 1 - hitPadding;
    } else if (alignment & Qt::AlignHCenter) {
        left = anchorRect.left() + ((anchorRect.width() - starsWidth) / 2) - hitPadding;
    }
    return {left, top, starsWidth + (hitPadding * 2), starSize};
}

QString elidedToTwoLines(const QString &text, const QFont &font, const QFontMetrics &metrics, int width)
{
    QTextLayout layout(text);
    layout.setFont(font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    layout.beginLayout();

    QStringList lines;
    for (int lineNumber = 0; lineNumber < 2; ++lineNumber) {
        QTextLine line = layout.createLine();
        if (!line.isValid()) {
            break;
        }
        line.setLineWidth(width);
        const int start = line.textStart();
        const int length = line.textLength();
        QString lineText = text.mid(start, length).trimmed();
        if (lineNumber == 1 && start + length < text.size()) {
            lineText = metrics.elidedText(text.mid(start).trimmed(), Qt::ElideRight, width);
        }
        lines.push_back(lineText);
    }

    layout.endLayout();
    return lines.join(QLatin1Char('\n'));
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
    opt.state &= ~QStyle::State_Selected;

    QApplication::style()->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, painter, opt.widget);
    if (index.data(SelectedRole).toBool()) {
        painter->setPen(QPen(opt.palette.color(QPalette::Highlight), 2));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(option.rect.adjusted(2, 2, -3, -3), 6, 6);
    }

    const int padding = index.data(CellPaddingRole).toInt() > 0 ? index.data(CellPaddingRole).toInt() : 8;
    const int artSize = index.data(ArtSizeRole).toInt() > 0 ? index.data(ArtSizeRole).toInt() : 176;
    const int starSize = index.data(StarSizeRole).toInt() > 0 ? index.data(StarSizeRole).toInt() : 18;
    const QRect content = option.rect.adjusted(padding, padding, -padding, -padding);
    const QRect artRect(content.left() + ((content.width() - artSize) / 2), content.top(), artSize, artSize);

    const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
    icon.paint(painter, artRect, Qt::AlignCenter, QIcon::Normal, QIcon::Off);

    const QRect titleRect(artRect.left(), artRect.bottom() + 5, artRect.width(), opt.fontMetrics.height() * 2 + 2);
    const QRect yearRect(artRect.left(), titleRect.bottom() + 1, artRect.width(), opt.fontMetrics.height());
    painter->setPen(opt.palette.color(QPalette::Text));
    const auto alignment = static_cast<Qt::Alignment>(index.data(TextAlignmentRole).toInt());
    const QString display = index.data(Qt::DisplayRole).toString();
    painter->drawText(titleRect, alignment | Qt::AlignTop, elidedToTwoLines(titleFromDisplay(display), opt.font, opt.fontMetrics, titleRect.width()));

    const QString year = yearFromDisplay(display);
    if (!year.isEmpty()) {
        painter->setPen(opt.palette.color(QPalette::Disabled, QPalette::Text));
        painter->drawText(yearRect, alignment | Qt::AlignVCenter, year);
    }

    const QRect ratingCell = alignedRatingCell(artRect, yearRect.bottom() + 4, starSize, alignment);
    StarRating::paint(painter,
                      StarRating::ratingRect(ratingCell, starSize),
                      index.data(RatingRole).toInt(),
                      index.data(HoverRatingRole).toInt(),
                      opt.palette,
                      starSize);

    painter->restore();
}
