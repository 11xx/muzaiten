#pragma once

#include <QColor>
#include <QPalette>
#include <QStyleOptionViewItem>
#include <QVariant>
#include <QWidget>

namespace SelectionColors {

inline bool isActiveMainPanel(const QWidget *widget)
{
    for (const QWidget *current = widget; current != nullptr; current = current->parentWidget()) {
        const QVariant active = current->property("mainPanelActive");
        if (active.isValid()) {
            return active.toBool();
        }
    }
    return widget != nullptr && widget->hasFocus();
}

// Alpha used to dim the selection highlight of an out-of-focus main panel.
inline constexpr int kInactiveHighlightAlpha = 36;

inline QColor selectedFill(const QStyleOptionViewItem &option)
{
    QColor color = option.palette.color(QPalette::Highlight);
    if (!isActiveMainPanel(option.widget)) {
        color.setAlpha(kInactiveHighlightAlpha);
    }
    return color;
}

// Opaque equivalent of the translucent selectedFill, for views that paint their
// own selection background through the palette Highlight role (e.g. QTableView)
// rather than deferring entirely to the item delegate. Blending the highlight
// over the base matches the dimmed look the delegate draws for list views.
inline QColor dimmedHighlight(const QColor &base, const QColor &highlight)
{
    const qreal a = kInactiveHighlightAlpha / 255.0;
    return QColor(qRound(base.red() * (1 - a) + highlight.red() * a),
                  qRound(base.green() * (1 - a) + highlight.green() * a),
                  qRound(base.blue() * (1 - a) + highlight.blue() * a));
}

inline QColor selectedText(const QStyleOptionViewItem &option)
{
    return isActiveMainPanel(option.widget)
        ? option.palette.color(QPalette::HighlightedText)
        : option.palette.color(QPalette::Text);
}

inline void applySelectedPalette(QStyleOptionViewItem *option)
{
    if (option == nullptr) {
        return;
    }

    const QColor fill = selectedFill(*option);
    const QColor text = selectedText(*option);
    for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        option->palette.setColor(group, QPalette::Highlight, fill);
        option->palette.setColor(group, QPalette::HighlightedText, text);
        option->palette.setColor(group, QPalette::Text, text);
    }
}

} // namespace SelectionColors
