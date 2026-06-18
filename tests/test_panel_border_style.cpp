#include "ui/PanelBorderStyle.h"
#include "ui/UiMetrics.h"

#include <QApplication>
#include <QColor>
#include <QFrame>
#include <QImage>
#include <QPainter>
#include <QPoint>
#include <QStyle>
#include <QStyleOptionFrame>
#include <QtTest/QtTest>

#include <algorithm>

namespace {

QColor nativePanelSeparatorColor()
{
    const QPalette palette = QApplication::palette();
    const QColor background = palette.color(QPalette::Window);
    QImage image(32, 32, QImage::Format_ARGB32_Premultiplied);
    image.fill(background);

    QStyleOptionFrame option;
    option.rect = image.rect();
    option.palette = palette;
    option.state = QStyle::State_Enabled | QStyle::State_Sunken;
    option.lineWidth = std::max(1, QApplication::style()->pixelMetric(QStyle::PM_DefaultFrameWidth, &option, nullptr));
    option.midLineWidth = 0;
    option.frameShape = QFrame::StyledPanel;

    QPainter painter(&image);
    QApplication::style()->drawPrimitive(QStyle::PE_Frame, &option, &painter, nullptr);
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

} // namespace

class PanelBorderStyleTest : public QObject {
    Q_OBJECT

private slots:
    void separatorMatchesActiveNativePanelFrame()
    {
        QCOMPARE(panelSeparatorColor(nullptr), nativePanelSeparatorColor());
    }

    void panelRadiusTracksAlbumGridSelectionRadius()
    {
        QCOMPARE(kAlbumGridSelectionRadius, 6);
        QCOMPARE(kAlbumGridSelectionInnerRadius, 5);
    }
};

QTEST_MAIN(PanelBorderStyleTest)

#include "test_panel_border_style.moc"
