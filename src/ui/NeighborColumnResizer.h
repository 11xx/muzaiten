#pragma once

#include <QObject>

#include <functional>

class QHeaderView;

// Constrains interactive column resizing to a two-column trade: dragging a
// column boundary resizes only the two columns that share it, transferring width
// from one to the other so the total stays constant and every other column keeps
// its size. This avoids the "resize one column, squish all the others" behaviour
// of a global redistribute.
//
// Reusable for any QHeaderView (QTableView or QTreeView). Programmatic section
// changes are ignored: the trade only runs while the user is actively dragging a
// handle (tracked via the header's mouse events), so callers never need to guard
// their own setColumnWidth/restoreState calls.
class NeighborColumnResizer final : public QObject {
    Q_OBJECT

public:
    // minWidthFor returns a logical column's minimum width. If null, the header's
    // minimumSectionSize() is used. The resizer parents itself to the header.
    static NeighborColumnResizer *install(QHeaderView *header,
                                          std::function<int(int)> minWidthFor = {});

signals:
    // Emitted once per user-driven trade (not for the internal neighbor update).
    void columnResized();
    void columnResized(int leftLogical, int rightLogical);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    explicit NeighborColumnResizer(QHeaderView *header, std::function<int(int)> minWidthFor);
    void onSectionResized(int logicalIndex, int oldSize, int newSize);
    int minWidthFor(int logicalIndex) const;
    int nextVisibleToRight(int logicalIndex) const;
    bool isOnHandle(int x) const;

    QHeaderView *m_header = nullptr;
    std::function<int(int)> m_minWidthFor;
    bool m_userResizing = false;
    bool m_adjusting = false;
};
