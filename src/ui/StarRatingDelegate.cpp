#include "ui/StarRatingDelegate.h"

#include "ui/SelectionColors.h"
#include "ui/StarRating.h"

#include <QApplication>
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

void StarRatingDelegate::setPlayingRow(int row)
{
    m_playingRow = row;
}

void StarRatingDelegate::setRowStyle(const TrackTableRowStyle &style)
{
    m_rowStyle = style;
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
    const bool playing = (m_playingRow == index.row());
    const QColor baseFill = trackTableRowBaseFill(m_rowStyle, opt, index.row());
    if (baseFill.isValid()) {
        painter->fillRect(opt.rect, baseFill);
    }
    if (selected) {
        painter->fillRect(opt.rect, m_rowStyle.enabled
                                        ? (SelectionColors::isActiveMainPanel(opt.widget) ? m_rowStyle.selected : m_rowStyle.inactiveSelected)
                                        : SelectionColors::selectedFill(opt));
        if (m_rowStyle.enabled) {
            const QColor text = SelectionColors::isActiveMainPanel(opt.widget) ? m_rowStyle.selectedText : m_rowStyle.text;
            for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
                opt.palette.setColor(group, QPalette::HighlightedText, text);
                opt.palette.setColor(group, QPalette::Text, text);
            }
        } else {
            SelectionColors::applySelectedPalette(&opt);
        }
    } else if (hovered) {
        QColor hover = m_rowStyle.enabled ? m_rowStyle.hover : opt.palette.color(QPalette::Highlight);
        if (!m_rowStyle.enabled) {
            hover.setAlpha(34);
        }
        painter->fillRect(opt.rect, hover);
    } else if (playing) {
        QColor tint = m_rowStyle.enabled ? m_rowStyle.playing : QApplication::palette().color(QPalette::Highlight);
        if (!m_rowStyle.enabled) {
            tint.setAlpha(48);
        }
        painter->fillRect(opt.rect, tint);
    }
    if (!selected && m_rowStyle.enabled) {
        for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
            opt.palette.setColor(group, QPalette::Text, m_rowStyle.text);
        }
    }
    opt.state &= ~QStyle::State_MouseOver;
    opt.state &= ~QStyle::State_Selected;

    const int value = index.data(Qt::UserRole).toInt();
    const int hoverValue = index.data(Qt::UserRole + 2).isValid() ? index.data(Qt::UserRole + 2).toInt() : StarRating::unset;
    StarRating::paint(painter, StarRating::ratingRect(opt.rect, 18), value, hoverValue, opt.palette, 18);
}
