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

struct PanelBorderEdges {
    bool top = false;
    bool right = false;
    bool bottom = false;
    bool left = false;
};

inline constexpr PanelBorderEdges panelTopBorder()
{
    return PanelBorderEdges{true, false, false, false};
}

inline constexpr PanelBorderEdges panelBottomBorder()
{
    return PanelBorderEdges{false, false, true, false};
}

inline constexpr PanelBorderEdges panelAllBorders()
{
    return PanelBorderEdges{true, true, true, true};
}

inline QColor panelSeparatorColor(const QWidget *widget)
{
    const QPalette palette = widget != nullptr ? widget->palette() : QApplication::palette();
    const QColor background = palette.color(QPalette::Window);
    QImage image(9, 9, QImage::Format_ARGB32_Premultiplied);
    image.fill(background);

    QStyleOptionFrame option;
    option.rect = image.rect();
    option.palette = palette;
    option.state = QStyle::State_Enabled;
    option.lineWidth = QApplication::style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &option, widget);
    option.midLineWidth = 0;
    option.frameShape = QFrame::StyledPanel;

    QPainter painter(&image);
    QApplication::style()->drawPrimitive(QStyle::PE_Frame, &option, &painter, widget);
    painter.end();

    const QPoint samples[] = {
        QPoint(image.width() / 2, 0),
        QPoint(0, image.height() / 2),
        QPoint(image.width() - 1, image.height() / 2),
        QPoint(image.width() / 2, image.height() - 1),
    };
    for (const QPoint &sample : samples) {
        const QColor color = QColor::fromRgba(image.pixel(sample));
        if (color.isValid() && color != background) {
            return color;
        }
    }

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
             QString::number(edges.top ? 1 : 0),
             QString::number(edges.right ? 1 : 0),
             QString::number(edges.bottom ? 1 : 0),
             QString::number(edges.left ? 1 : 0));
}
