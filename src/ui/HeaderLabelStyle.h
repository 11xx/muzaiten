#pragma once

#include <QHeaderView>
#include <QApplication>
#include <QBrush>
#include <QFont>
#include <QPalette>
#include <QString>
#include <QVariant>

#include <algorithm>
#include <cmath>

enum class HeaderLabelTone {
    Default,
    Muted,
};

struct HeaderLabelStyle {
    QFont::Weight fontWeight = QFont::Normal;
    bool hasFontWeight = false;
    HeaderLabelTone tone = HeaderLabelTone::Default;
    double contrastBoost = 0.0;
};

struct HeaderViewStyle {
    HeaderLabelStyle labels;
    bool bottomBorderVisible = true;
};

inline QColor blendHeaderColor(const QColor &from, const QColor &to, double amount)
{
    amount = std::clamp(amount, 0.0, 1.0);
    return QColor(static_cast<int>(std::lround(from.red() + (to.red() - from.red()) * amount)),
                  static_cast<int>(std::lround(from.green() + (to.green() - from.green()) * amount)),
                  static_cast<int>(std::lround(from.blue() + (to.blue() - from.blue()) * amount)),
                  static_cast<int>(std::lround(from.alpha() + (to.alpha() - from.alpha()) * amount)));
}

inline QBrush headerLabelBrush(const QPalette &palette, const HeaderLabelStyle &style)
{
    switch (style.tone) {
    case HeaderLabelTone::Default:
        return palette.brush(QPalette::Text);
    case HeaderLabelTone::Muted: {
        const QColor muted = palette.color(QPalette::Disabled, QPalette::Text);
        const QColor normal = palette.color(QPalette::Text);
        return QBrush(blendHeaderColor(muted, normal, style.contrastBoost));
    }
    }

    return palette.brush(QPalette::Text);
}

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
        if (style.tone == HeaderLabelTone::Default) {
            return {};
        }
        return headerLabelBrush(QApplication::palette(), style);
    default:
        return {};
    }
}

inline QString headerViewStyleSheet(const HeaderViewStyle &style)
{
    if (style.bottomBorderVisible) {
        return {};
    }

    return QStringLiteral(
        "QHeaderView { border: 0px; }"
        "QHeaderView::section { border: none; }");
}

inline void applyHeaderViewStyle(QHeaderView *header, const HeaderViewStyle &style)
{
    if (header == nullptr) {
        return;
    }

    header->setStyleSheet(headerViewStyleSheet(style));
}
