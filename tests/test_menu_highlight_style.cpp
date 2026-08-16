#include "ui/MenuHighlightStyle.h"
#include "ui/SelectionColors.h"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QProxyStyle>
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

// Stands in for the theme underneath, so a test can see the option the style
// hands down rather than infer it from what was painted.
class RecordingStyle final : public QProxyStyle {
public:
    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter,
                     const QWidget *widget) const override
    {
        if (element == CE_MenuItem) {
            sawMenuItem = true;
            delegated = option->state;
        }
        Q_UNUSED(painter);
        Q_UNUSED(widget);
    }

    mutable bool sawMenuItem = false;
    mutable QStyle::State delegated;
};

// The state the theme underneath is asked to draw with.
QStyle::State delegatedStateFor(MenuHighlightStyle::Emphasis emphasis, QStyle::State state)
{
    MenuHighlightStyle style(emphasis);
    auto *recorder = new RecordingStyle;   // setBaseStyle takes ownership
    style.setBaseStyle(recorder);

    QStyleOptionMenuItem option;
    option.rect = QRect(0, 0, 120, 24);
    option.state = state;
    option.palette = QApplication::palette();
    option.menuItemType = QStyleOptionMenuItem::Normal;

    QImage canvas(option.rect.size(), QImage::Format_ARGB32);
    QPainter painter(&canvas);
    style.drawControl(QStyle::CE_MenuItem, &option, &painter, nullptr);
    painter.end();

    // Never reached without an observation: a recorder that saw nothing returns
    // State_None, and every caller asserts on flags that state does not carry.
    return recorder->delegated;
}

}   // namespace

class MenuHighlightStyleTest final : public QObject {
    Q_OBJECT

private slots:
    void solidFillsWithTheHighlightItself();
    void softStopsShortOfTheHighlight();
    void aPressedEntryIsMarkedLikeAPointedAtOne();
    void aDisabledEntryIsLeftToTheTheme();
    void bothMarksAreClearedBeforeTheThemeDraws();
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

// Painting the fill is only half of it: the theme has to be told the entry is
// neither pointed at nor pressed, or it paints its own mark back over the top.
// Asserting on the delegated option catches that where a fill pixel cannot.
void MenuHighlightStyleTest::bothMarksAreClearedBeforeTheThemeDraws()
{
    const QStyle::State pressed = QStyle::State_Enabled | QStyle::State_Selected | QStyle::State_Sunken;
    for (const MenuHighlightStyle::Emphasis emphasis :
         {MenuHighlightStyle::Emphasis::Solid, MenuHighlightStyle::Emphasis::Soft}) {
        const QStyle::State delegated = delegatedStateFor(emphasis, pressed);
        QVERIFY(!delegated.testFlag(QStyle::State_Selected));
        QVERIFY(!delegated.testFlag(QStyle::State_Sunken));
        // Everything else it was told still holds.
        QVERIFY(delegated.testFlag(QStyle::State_Enabled));
    }

    // An entry the style leaves alone is handed down exactly as it arrived.
    const QStyle::State disabled = QStyle::State_Selected | QStyle::State_Sunken;
    QCOMPARE(delegatedStateFor(MenuHighlightStyle::Emphasis::Soft, disabled), disabled);
}

QTEST_MAIN(MenuHighlightStyleTest)
#include "test_menu_highlight_style.moc"
