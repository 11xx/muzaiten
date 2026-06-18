#pragma once

#include <QApplication>
#include <QColor>
#include <QFrame>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QPoint>
#include <QStyle>
#include <QStyleOptionFrame>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <vector>

struct PanelBorderEdges {
    int top = 0;
    int right = 0;
    int bottom = 0;
    int left = 0;

    constexpr bool operator==(const PanelBorderEdges &) const = default;
};

inline constexpr PanelBorderEdges panelNoBorders()
{
    return PanelBorderEdges{};
}

inline constexpr PanelBorderEdges panelBorders(int top, int right, int bottom, int left)
{
    return PanelBorderEdges{top, right, bottom, left};
}

inline constexpr PanelBorderEdges panelTopBorder(int width = 1)
{
    return PanelBorderEdges{width, 0, 0, 0};
}

inline constexpr PanelBorderEdges panelBottomBorder(int width = 1)
{
    return PanelBorderEdges{0, 0, width, 0};
}

inline constexpr PanelBorderEdges panelRightBorder(int width = 1)
{
    return PanelBorderEdges{0, width, 0, 0};
}

inline constexpr PanelBorderEdges panelLeftBorder(int width = 1)
{
    return PanelBorderEdges{0, 0, 0, width};
}

inline constexpr PanelBorderEdges panelHorizontalBorders(int width = 1)
{
    return PanelBorderEdges{width, 0, width, 0};
}

inline constexpr PanelBorderEdges panelAllBorders(int width = 1)
{
    return PanelBorderEdges{width, width, width, width};
}

// Sample the native separator color the same way a real QFrame::HLine renders
// it. Breeze (and most styles) paint a 2-pixel bevel for HLine/VLine separators
// — a light line and a dark line — whose colors are NOT exposed by any palette
// role and are NOT reproduced by QStyle::PE_Frame (which renders a generic
// panel frame, not a separator line). Sampling PE_Frame therefore returns the
// wrong color (#5D5D5D instead of #5E5E5D under Breeze+KvGnomeDark) — exactly
// the one-step-off mismatch that plagued the earlier implementation.
//
// To get the true separator color we realize a QFrame::HLine offscreen
// (WA_DontShowOnScreen keeps it invisible, even on Wayland), grab its pixels,
// and return the bevel color with the greatest contrast against the window
// background — i.e. the line the eye actually reads as "the separator".
inline QColor panelSeparatorColor(const QWidget *widget)
{
    const QPalette palette = widget != nullptr ? widget->palette() : QApplication::palette();
    const QColor background = palette.color(QPalette::Window);

    QFrame separator;
    separator.setFrameShape(QFrame::HLine);
    separator.setLineWidth(1);
    separator.setMidLineWidth(0);
    separator.setPalette(palette);
    separator.resize(64, 4);
    separator.setAttribute(Qt::WA_DontShowOnScreen);
    separator.show();
    QApplication::processEvents();
    const QImage image = separator.grab().toImage();
    separator.hide();

    const auto distance = [&background](QRgb c) {
        const QColor cc = QColor::fromRgba(c);
        const int dr = int(cc.red()) - background.red();
        const int dg = int(cc.green()) - background.green();
        const int db = int(cc.blue()) - background.blue();
        return dr * dr + dg * dg + db * db;
    };

    const QRgb bg = background.rgba();
    QColor best;
    int bestDist = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb c = image.pixel(x, y);
            if (c == bg) continue;
            const int d = distance(c);
            if (d > bestDist) {
                bestDist = d;
                best = QColor::fromRgba(c);
            }
        }
    }
    if (best.isValid()) return best;

    return palette.color(QPalette::Light);
}

inline QString panelSeparatorColorCss(const QWidget *widget)
{
    return panelSeparatorColor(widget).name(QColor::HexRgb);
}

inline QString panelBorderStyleSheet(const QString &selector,
                                     PanelBorderEdges edges,
                                     const QWidget *widget,
                                     const QString &extraDeclarations = {})
{
    return QStringLiteral("%1 {%2 border-style: solid; border-color: %3; border-width: %4px %5px %6px %7px; }")
        .arg(selector,
             extraDeclarations,
             panelSeparatorColorCss(widget),
             QString::number(std::max(0, edges.top)),
             QString::number(std::max(0, edges.right)),
             QString::number(std::max(0, edges.bottom)),
             QString::number(std::max(0, edges.left)));
}

inline bool applyPanelBorderStyleSheet(QWidget *widget,
                                       const QString &selector,
                                       PanelBorderEdges edges,
                                       const QString &extraDeclarations = {},
                                       bool force = false)
{
    if (widget == nullptr) {
        return false;
    }

    const QString style = panelBorderStyleSheet(selector, edges, widget, extraDeclarations);
    if (force && widget->styleSheet() == style) {
        widget->setStyleSheet(QString());
    }
    if (widget->styleSheet() == style) {
        return false;
    }
    widget->setStyleSheet(style);
    return true;
}
