#include "ui/StickyMenu.h"

#include <QKeyEvent>
#include <QMouseEvent>

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
