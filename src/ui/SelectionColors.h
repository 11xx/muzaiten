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

inline QColor selectedFill(const QStyleOptionViewItem &option)
{
    QColor color = option.palette.color(QPalette::Highlight);
    if (!isActiveMainPanel(option.widget)) {
        color.setAlpha(36);
    }
    return color;
}

inline QColor selectedText(const QStyleOptionViewItem &option)
{
    return isActiveMainPanel(option.widget)
        ? option.palette.color(QPalette::HighlightedText)
        : option.palette.color(QPalette::Text);
}

} // namespace SelectionColors
