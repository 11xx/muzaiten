#include "ui/MenuHighlightStyle.h"
#include "ui/SelectionColors.h"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QStyleOptionMenuItem>
#include <QTest>

#include <array>
#include <cstdlib>

namespace {

using Channels = std::array<int, 3>;

Channels channels(const QColor &color)
{
    return {color.red(), color.green(), color.blue()};
}

// Paints one menu entry through the style and returns the fill, sampled at a
// corner that carries no checkmark and no label.
QColor fillFor(MenuHighlightStyle::Emphasis emphasis, QStyle::State state)
{
    MenuHighlightStyle style(emphasis);
    QStyleOptionMenuItem option;
    option.rect = QRect(0, 0, 120, 24);
    option.state = state;
    option.palette = QApplication::palette();
    option.text = QStringLiteral("Koito");
    option.menuItemType = QStyleOptionMenuItem::Normal;

    QImage canvas(option.rect.size(), QImage::Format_ARGB32);
    canvas.fill(Qt::magenta);   // nothing here should survive unpainted
    QPainter painter(&canvas);
    style.drawControl(QStyle::CE_MenuItem, &option, &painter, nullptr);
    painter.end();
    return canvas.pixelColor(2, 2);
}

}   // namespace

class MenuHighlightStyleTest final : public QObject {
    Q_OBJECT

private slots:
    void solidFillsWithTheHighlightItself();
    void softStopsShortOfTheHighlight();
    void aPressedEntryIsMarkedLikeAPointedAtOne();
    void aDisabledEntryIsLeftToTheTheme();
};

// The application menus have always marked the entry under the cursor with the
// palette's highlight at full strength.
void MenuHighlightStyleTest::solidFillsWithTheHighlightItself()
{
    const QColor painted = fillFor(MenuHighlightStyle::Emphasis::Solid,
                                   QStyle::State_Enabled | QStyle::State_Selected);
    QCOMPARE(painted, QApplication::palette().color(QPalette::Highlight));
}

// The soft fill is the same colour pulled most of the way back towards the
// window, so an entry toggled repeatedly does not shout.
void MenuHighlightStyleTest::softStopsShortOfTheHighlight()
{
    const QColor painted = fillFor(MenuHighlightStyle::Emphasis::Soft,
                                   QStyle::State_Enabled | QStyle::State_Selected);
    const Channels from = channels(QApplication::palette().color(QPalette::Window));
    const Channels towards = channels(QApplication::palette().color(QPalette::Highlight));
    const Channels got = channels(painted);

    bool moved = false;
    for (int index = 0; index < 3; ++index) {
        QVERIFY(got.at(index) >= std::min(from.at(index), towards.at(index)));
        QVERIFY(got.at(index) <= std::max(from.at(index), towards.at(index)));
        if (from.at(index) != towards.at(index)) {
            QVERIFY(std::abs(got.at(index) - from.at(index)) < std::abs(towards.at(index) - from.at(index)));
            moved = moved || got.at(index) != from.at(index);
        }
    }
    QVERIFY(moved);
}

// A theme marks pressed differently from pointed-at, usually lighter still, so
// an entry answering only for the second went pale the moment it was clicked.
void MenuHighlightStyleTest::aPressedEntryIsMarkedLikeAPointedAtOne()
{
    const QStyle::State pointedAt = QStyle::State_Enabled | QStyle::State_Selected;
    const QStyle::State pressed = pointedAt | QStyle::State_Sunken;
    for (const MenuHighlightStyle::Emphasis emphasis :
         {MenuHighlightStyle::Emphasis::Solid, MenuHighlightStyle::Emphasis::Soft}) {
        QCOMPARE(fillFor(emphasis, pressed), fillFor(emphasis, pointedAt));
    }

    // Held down without having been pointed at first is still pressed.
    QCOMPARE(fillFor(MenuHighlightStyle::Emphasis::Soft, QStyle::State_Enabled | QStyle::State_Sunken),
             fillFor(MenuHighlightStyle::Emphasis::Soft, pointedAt));
}

// An entry that cannot be chosen is not marked as the one being chosen, in
// either emphasis: the theme's disabled rendering is left alone.
void MenuHighlightStyleTest::aDisabledEntryIsLeftToTheTheme()
{
    for (const MenuHighlightStyle::Emphasis emphasis :
         {MenuHighlightStyle::Emphasis::Solid, MenuHighlightStyle::Emphasis::Soft}) {
        const QColor painted = fillFor(emphasis, QStyle::State_Selected | QStyle::State_Sunken);
        QVERIFY(painted != QApplication::palette().color(QPalette::Highlight));
        QVERIFY(painted
                != SelectionColors::dimmedHighlight(QApplication::palette().color(QPalette::Window),
                                                    QApplication::palette().color(QPalette::Highlight),
                                                    SelectionColors::kSoftHighlightAlpha));
    }
}

QTEST_MAIN(MenuHighlightStyleTest)
#include "test_menu_highlight_style.moc"
