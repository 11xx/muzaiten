#include "ui/StickyMenu.h"

#include "ui/SelectionColors.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QProxyStyle>
#include <QStyleOptionMenuItem>

namespace {

// Several themes mark the entry under the cursor with a flat grey card, and a
// lighter grey once it is pressed, which reads as nothing in particular and
// matches nothing else in the app. They paint it from their own colours rather
// than from QPalette::Highlight, so neither a palette nor a stylesheet reaches
// it; taking over the fill is what is left.
//
// Everything else about the entry stays the theme's, including its metrics, its
// checkmark and its text, and the colour is the palette's own accent read at
// paint time. Clearing the selected state before delegating is what stops the
// theme painting its grey over the wash.
class MenuHighlightStyle final : public QProxyStyle {
public:
    // Default-constructed, so it proxies the application style without taking
    // ownership of it.
    using QProxyStyle::QProxyStyle;

    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter,
                     const QWidget *widget) const override
    {
        const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option);
        if (element != CE_MenuItem || item == nullptr || (option->state & State_Selected) == 0
            || (option->state & State_Enabled) == 0) {
            QProxyStyle::drawControl(element, option, painter, widget);
            return;
        }

        QStyleOptionMenuItem adjusted(*item);
        painter->fillRect(adjusted.rect, StickyMenu::highlightWash(QApplication::palette(widget)));
        adjusted.state &= ~State_Selected;
        QProxyStyle::drawControl(element, &adjusted, painter, widget);
    }
};

}   // namespace

QColor StickyMenu::highlightWash(const QPalette &palette)
{
    return SelectionColors::dimmedHighlight(palette.color(QPalette::Active, QPalette::Window),
                                            palette.color(QPalette::Active, QPalette::Highlight),
                                            SelectionColors::kSoftHighlightAlpha);
}

StickyMenu::StickyMenu(QWidget *parent)
    : QMenu(parent)
{
    applyHighlight();
}

StickyMenu::StickyMenu(const QString &title, QWidget *parent)
    : QMenu(title, parent)
{
    applyHighlight();
}

void StickyMenu::mouseReleaseEvent(QMouseEvent *event)
{
    QAction *action = actionAt(event->position().toPoint());
    if (action != nullptr && action->isCheckable() && action->isEnabled()) {
        action->trigger();
        return;
    }
    QMenu::mouseReleaseEvent(event);
}

void StickyMenu::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Space) {
        QAction *action = activeAction();
        if (action != nullptr && action->isCheckable() && action->isEnabled()) {
            action->trigger();
            return;
        }
    }
    QMenu::keyPressEvent(event);
}

void StickyMenu::applyHighlight()
{
    // Installed once, from the constructor. Doing it from changeEvent instead
    // would not terminate: setStyle posts a StyleChange, which would install
    // another style, which posts another. Reinstalling buys nothing anyway,
    // because the colour is read from the palette at paint time.
    auto *highlight = new MenuHighlightStyle;
    highlight->setParent(this);
    setStyle(highlight);
}
