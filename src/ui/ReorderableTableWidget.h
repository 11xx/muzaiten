#pragma once

#include <QTableWidget>

#include <functional>

class QMouseEvent;
class QResizeEvent;

// A QTableWidget whose rows can be reordered by dragging, showing a single
// insertion line between rows (the same above/below cue the queue uses) instead
// of Qt's default cell-move drag. Reordering is delegated to `reorder` so the
// owner can rebuild rows from their data rather than juggling cell-widget
// pointers (removeCellWidget would delete them out from under a pointer swap).
//
// Reusable by any multi-column settings table that needs drag reordering plus a
// column that stretches to fill the viewport.
class ReorderableTableWidget final : public QTableWidget {
public:
    ReorderableTableWidget(int rows, int columns, QWidget *parent = nullptr);

    // Called with (source row, insertion index) once a drag completes; the owner
    // performs the actual move (typically read rows -> reorder data -> rebuild).
    std::function<void(int from, int to)> reorder;

    // The column that absorbs leftover width so the columns always span the
    // viewport, letting the user-resizable columns trade against it.
    void setFillColumn(int column);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void fitFillColumn();
    int dropRowAt(int y) const;
    void showDropLine(int row);

    QWidget *m_dropLine = nullptr;
    int m_pressRow = -1;
    int m_pressY = 0;
    int m_fillColumn = -1;
    bool m_dragging = false;
};
