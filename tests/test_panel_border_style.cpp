#include "ui/PanelBorderStyle.h"
#include "ui/UiMetrics.h"

#include <QApplication>
#include <QColor>
#include <QFrame>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QStyle>
#include <QStyleOptionFrame>
#include <QtTest/QtTest>

namespace {

// Independently sample the native separator color by rendering a real, shown
// QFrame::HLine — the exact primitive Breeze uses for separator lines. This is
// the ground truth the helper must match, NOT QStyle::PE_Frame (which paints a
// generic panel frame whose bevel color is one step off the separator's).
QColor nativeSeparatorColor()
{
    const QPalette palette = QApplication::palette();
    const QColor background = palette.color(QPalette::Window);

    QFrame separator;
    separator.setFrameShape(QFrame::HLine);
    separator.setLineWidth(1);
    separator.setMidLineWidth(0);
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
    return best.isValid() ? best : palette.color(QPalette::Light);
}

// Sample the OLD, wrong primitive (PE_Frame on a StyledPanel) so the test can
// prove the helper does NOT regress to it.
QColor peFrameColor()
{
    const QPalette palette = QApplication::palette();
    const QColor background = palette.color(QPalette::Window);
    QImage image(32, 32, QImage::Format_ARGB32_Premultiplied);
    image.fill(background);
    QStyleOptionFrame option;
    option.rect = image.rect();
    option.palette = palette;
    option.state = QStyle::State_Enabled | QStyle::State_Sunken;
    option.lineWidth = 1;
    option.midLineWidth = 0;
    option.frameShape = QFrame::StyledPanel;
    QPainter painter(&image);
    QApplication::style()->drawPrimitive(QStyle::PE_Frame, &option, &painter, nullptr);
    painter.end();
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const QColor c = QColor::fromRgba(image.pixel(x, y));
            if (c.isValid() && c != background) return c;
        }
    return palette.color(QPalette::Light);
}

} // namespace

class PanelBorderStyleTest : public QObject {
    Q_OBJECT

private slots:
    void separatorMatchesNativeHLineSeparator()
    {
        QCOMPARE(panelSeparatorColor(nullptr), nativeSeparatorColor());
    }

    void separatorDoesNotRegressToPanelFramePrimitive()
    {
        // PE_Frame produces a different (one-step-off) bevel color than the
        // HLine separator under Breeze. Lock the fix in: the helper must NOT
        // return the PE_Frame color.
        QVERIFY2(panelSeparatorColor(nullptr) != peFrameColor(),
                 "panelSeparatorColor regressed to the PE_Frame primitive; "
                 "it must sample a QFrame::HLine separator instead.");
    }

    void panelRadiusTracksAlbumGridSelectionRadius()
    {
        QCOMPARE(kAlbumGridSelectionRadius, 6);
        QCOMPARE(kAlbumGridSelectionInnerRadius, 5);
    }
};

QTEST_MAIN(PanelBorderStyleTest)

#include "test_panel_border_style.moc"
