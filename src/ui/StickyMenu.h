#pragma once

#include <QMenu>

// A menu whose checkable entries can be toggled without dismissing it, so a
// subset is picked in one visit instead of one reopen per item. Entries that
// are not checkable behave as they always do: they act, and the menu closes.
class StickyMenu final : public QMenu {
    Q_OBJECT

public:
    using QMenu::QMenu;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
};
