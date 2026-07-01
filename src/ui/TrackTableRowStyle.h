#pragma once

#include <QColor>
#include <QPalette>
#include <QStyleOptionViewItem>

struct TrackTableRowStyle {
    bool enabled = false;
    QColor primary;
    QColor alternate;
    QColor hover;
    QColor playing;
    QColor selected;
    QColor inactiveSelected;
    QColor text;
    QColor selectedText;
};

inline QColor trackTableRowBaseFill(const TrackTableRowStyle &style, const QStyleOptionViewItem &option, int row)
{
    if (style.enabled) {
        return row % 2 == 1 ? style.alternate : style.primary;
    }
    return row % 2 == 1 ? option.palette.color(QPalette::AlternateBase) : QColor();
}
