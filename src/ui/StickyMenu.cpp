#include "ui/StickyMenu.h"

#include "ui/MenuHighlightStyle.h"
#include "ui/SelectionColors.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>

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
    auto *highlight = new MenuHighlightStyle(MenuHighlightStyle::Emphasis::Soft);
    highlight->setParent(this);
    setStyle(highlight);
}
