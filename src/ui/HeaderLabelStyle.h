#pragma once

#include <QApplication>
#include <QBrush>
#include <QFont>
#include <QPalette>
#include <QVariant>

enum class HeaderLabelTone {
    Default,
    Muted,
};

struct HeaderLabelStyle {
    QFont::Weight fontWeight = QFont::Normal;
    bool hasFontWeight = false;
    HeaderLabelTone tone = HeaderLabelTone::Default;
};

inline QVariant headerLabelStyleData(int role, const HeaderLabelStyle &style)
{
    switch (role) {
    case Qt::FontRole: {
        if (!style.hasFontWeight) {
            return {};
        }

        QFont font = QApplication::font("QHeaderView");
        font.setWeight(style.fontWeight);
        return font;
    }
    case Qt::ForegroundRole:
        if (style.tone != HeaderLabelTone::Muted) {
            return {};
        }
        return QApplication::palette().brush(QPalette::Disabled, QPalette::Text);
    default:
        return {};
    }
}
