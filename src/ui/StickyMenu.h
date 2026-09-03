#pragma once

#include <QMenu>

// A menu whose checkable entries can be toggled without dismissing it, so a
// subset is picked in one visit instead of one reopen per item. Entries that
// are not checkable behave as they always do: they act, and the menu closes.
//
// Its entries receive the application's soft palette-highlight wash, so picking
// an entry looks like every other menu selection in the app.
class StickyMenu final : public QMenu {
    Q_OBJECT

public:
    explicit StickyMenu(QWidget *parent = nullptr);
    StickyMenu(const QString &title, QWidget *parent = nullptr);

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
};
