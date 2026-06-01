#pragma once

#include <QScrollBar>
#include <QTableView>

#include <algorithm>

namespace TableNavigationScroll {

inline constexpr int kDefaultPaddingRows = 3;

inline void ensureRowVisible(QTableView *table, int row, int paddingRows = kDefaultPaddingRows)
{
    if (table == nullptr || table->model() == nullptr || row < 0 || row >= table->model()->rowCount()) {
        return;
    }

    const int rowHeight = std::max(1, table->rowHeight(row));
    const int padding = std::max(0, paddingRows) * rowHeight;
    const int viewportHeight = table->viewport() != nullptr ? table->viewport()->height() : 0;
    if (viewportHeight <= 0) {
        table->scrollTo(table->model()->index(row, 0), QAbstractItemView::EnsureVisible);
        return;
    }

    const int top = table->rowViewportPosition(row);
    const int bottom = top + rowHeight;
    QScrollBar *bar = table->verticalScrollBar();
    if (bar == nullptr) {
        return;
    }

    if (top < padding) {
        bar->setValue(bar->value() + top - padding);
    } else if (bottom > viewportHeight - padding) {
        bar->setValue(bar->value() + bottom - (viewportHeight - padding));
    }
}

} // namespace TableNavigationScroll
