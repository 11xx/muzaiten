#include "ui/DenseTableDelegate.h"

#include "ui/SelectionColors.h"

#include <QApplication>
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

void DenseTableDelegate::setPlayingRow(int row)
{
    m_playingRow = row;
}

void DenseTableDelegate::setRowStyle(const TrackTableRowStyle &style)
{
    m_rowStyle = style;
}

void DenseTableDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
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
    } else if (hovered) {
        QColor hover = m_rowStyle.enabled ? m_rowStyle.hover : opt.palette.color(QPalette::Highlight);
        if (!m_rowStyle.enabled) {
            hover.setAlpha(34);
        }
        painter->fillRect(opt.rect, hover);
    } else if (playing) {
        // Always-on now-playing indicator: read from the application palette so it
        // stays visible when the pane loses focus (the view palette dims Highlight
        // to fade remembered selections, which would fade this marker too).
        QColor tint = m_rowStyle.enabled ? m_rowStyle.playing : QApplication::palette().color(QPalette::Highlight);
        if (!m_rowStyle.enabled) {
            tint.setAlpha(48);
        }
        painter->fillRect(opt.rect, tint);
    }

    if (selected) {
        if (m_rowStyle.enabled) {
            const QColor text = SelectionColors::isActiveMainPanel(opt.widget) ? m_rowStyle.selectedText : m_rowStyle.text;
            for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
                opt.palette.setColor(group, QPalette::HighlightedText, text);
                opt.palette.setColor(group, QPalette::Text, text);
            }
        } else {
            SelectionColors::applySelectedPalette(&opt);
        }
    } else {
        if (m_rowStyle.enabled) {
            for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
                opt.palette.setColor(group, QPalette::Text, m_rowStyle.text);
            }
        }
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
