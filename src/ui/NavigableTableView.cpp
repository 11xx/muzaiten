#include "ui/NavigableTableView.h"

#include <QAbstractItemModel>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QScrollBar>

#include <algorithm>

NavigableTableView::NavigableTableView(QWidget *parent)
    : QTableView(parent)
{
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setVerticalScrollMode(QAbstractItemView::ScrollPerItem);
}

void NavigableTableView::setNavigationScrollPadding(int rows)
{
    m_navigationScrollPadding = std::max(0, rows);
}

int NavigableTableView::navigationScrollPadding() const
{
    return m_navigationScrollPadding;
}

int NavigableTableView::currentNavigationRow() const
{
    const QModelIndex index = currentIndex();
    return index.isValid() ? index.row() : -1;
}

void NavigableTableView::setCurrentNavigationRow(int row, int direction)
{
    if (model() == nullptr || model()->rowCount() == 0) {
        return;
    }

    const int safeRow = std::clamp(row, 0, model()->rowCount() - 1);
    const QModelIndex index = model()->index(safeRow, 0);

    if (selectionModel() != nullptr) {
        selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    setCurrentIndex(index);
    scrollNavigationRowToAnchor(safeRow, direction);

}

void NavigableTableView::setMainPanelActive(bool active)
{
    if (m_mainPanelActive == active
        && property("mainPanelActive").toBool() == active
        && viewport()->property("mainPanelActive").toBool() == active) {
        return;
    }

    m_mainPanelActive = active;
    setProperty("mainPanelActive", active);
    viewport()->setProperty("mainPanelActive", active);
    viewport()->update();
}

bool NavigableTableView::mainPanelActive() const
{
    return m_mainPanelActive;
}

void NavigableTableView::rowsInserted(const QModelIndex &parent, int start, int end)
{
    QTableView::rowsInserted(parent, start, end);
    Q_UNUSED(start)
    Q_UNUSED(end)
    if (!parent.isValid() && currentNavigationRow() < 0 && model() != nullptr && model()->rowCount() > 0) {
        setCurrentNavigationRow(0);
    }
}

void NavigableTableView::rowsAboutToBeRemoved(const QModelIndex &parent, int start, int end)
{
    Q_UNUSED(start)
    Q_UNUSED(end)
    QTableView::rowsAboutToBeRemoved(parent, start, end);
}

void NavigableTableView::currentChanged(const QModelIndex &current, const QModelIndex &previous)
{
    QTableView::currentChanged(current, previous);
    updateRow(previous);
    updateRow(current);
    if (current.isValid() && previous.row() != current.row()) {
        emit navigationRowChanged(current.row());
    }
}

void NavigableTableView::scrollNavigationRowToAnchor(int row, int direction)
{
    if (model() == nullptr || verticalScrollBar() == nullptr || viewport() == nullptr) {
        return;
    }

    const int rowHeightPx = std::max(1, rowHeight(row));
    const int visibleRows = std::max(1, viewport()->height() / rowHeightPx);
    const int padding = std::clamp(m_navigationScrollPadding, 0, std::max(0, visibleRows - 1));
    QScrollBar *bar = verticalScrollBar();
    const int topRow = bar->value();
    int desiredTop = topRow;

    if (direction > 0) {
        const int anchor = visibleRows - 1 - padding;
        if (row - topRow > anchor) {
            desiredTop = row - anchor;
        }
    } else if (direction < 0) {
        const int anchor = padding;
        if (row - topRow < anchor) {
            desiredTop = row - anchor;
        }
    } else {
        if (row < topRow) {
            desiredTop = row;
        } else if (row >= topRow + visibleRows) {
            desiredTop = row - visibleRows + 1;
        }
    }

    desiredTop = std::clamp(desiredTop, bar->minimum(), bar->maximum());
    if (desiredTop != topRow) {
        bar->setValue(desiredTop);
    }
}

void NavigableTableView::updateRow(const QModelIndex &index)
{
    if (!index.isValid() || viewport() == nullptr) {
        return;
    }
    const QRect rect = visualRect(model()->index(index.row(), 0));
    if (rect.isValid()) {
        viewport()->update(QRect(0, rect.top(), viewport()->width(), rect.height()));
    }
}
