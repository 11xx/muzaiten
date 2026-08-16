#pragma once

#include <QColor>
#include <QMenu>
#include <QPalette>

// A menu whose checkable entries can be toggled without dismissing it, so a
// subset is picked in one visit instead of one reopen per item. Entries that
// are not checkable behave as they always do: they act, and the menu closes.
//
// It also marks the entry under the cursor with a wash of the palette's own
// highlight, rather than the flat grey card several themes reach for, so
// picking an entry looks like every other selection in the app.
class StickyMenu final : public QMenu {
    Q_OBJECT

public:
    explicit StickyMenu(QWidget *parent = nullptr);
    StickyMenu(const QString &title, QWidget *parent = nullptr);

    // The fill drawn behind the entry under the cursor: the palette's highlight
    // softened against its window colour, opaque because a menu entry is.
    static QColor highlightWash(const QPalette &palette);

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void applyHighlight();
};
