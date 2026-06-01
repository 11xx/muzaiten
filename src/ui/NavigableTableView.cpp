#include "ui/NavigableTableView.h"

#include "ui/SelectionColors.h"

#include <QAbstractItemModel>
#include <QApplication>
#include <QEvent>
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
        clearSelection();
        setCurrentIndex({});
        return;
    }

    const int safeRow = std::clamp(row, 0, model()->rowCount() - 1);
    const QModelIndex index = model()->index(safeRow, 0);
    const int previousTopRow = verticalScrollBar() != nullptr ? verticalScrollBar()->value() : 0;

    if (selectionModel() != nullptr) {
        selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }
    setCurrentIndex(index);
    scrollNavigationRowToAnchor(safeRow, direction, previousTopRow);
}

void NavigableTableView::setMainPanelActive(bool active)
{
    m_mainPanelActive = active;
    setProperty("mainPanelActive", active);
    viewport()->setProperty("mainPanelActive", active);
    refreshInactiveHighlight();
    viewport()->update();
}

bool NavigableTableView::mainPanelActive() const
{
    return m_mainPanelActive;
}

void NavigableTableView::refreshInactiveHighlight()
{
    // Unlike list views, QTableView paints the selected-row background itself
    // through the palette Highlight role, so clearing State_Selected in the item
    // delegate is not enough to dim a remembered selection when the panel is out
    // of focus. Dim the Highlight role directly while inactive, recomputed from
    // the inherited palette so runtime theme changes are honored.
    QPalette pal = QApplication::palette(this);
    if (!m_mainPanelActive) {
        const QColor dim = SelectionColors::dimmedHighlight(pal.color(QPalette::Base),
                                                            pal.color(QPalette::Highlight));
        for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
            pal.setColor(group, QPalette::Highlight, dim);
        }
    }
    setPalette(pal);
}

void NavigableTableView::changeEvent(QEvent *event)
{
    QTableView::changeEvent(event);
    // Re-derive the dimmed Highlight after an app-wide palette or style change.
    // PaletteChange is excluded so our own setPalette() override does not recurse.
    if (event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::StyleChange) {
        refreshInactiveHighlight();
    }
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
    if (current.isValid()
        && selectionModel() != nullptr
        && !selectionModel()->isRowSelected(current.row(), current.parent())) {
        selectionModel()->select(current, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
    updateRow(previous);
    updateRow(current);
    if (current.isValid() && previous.row() != current.row()) {
        emit navigationRowChanged(current.row());
    }
}

void NavigableTableView::scrollNavigationRowToAnchor(int row, int direction, int previousTopRow)
{
    if (model() == nullptr || verticalScrollBar() == nullptr || viewport() == nullptr) {
        return;
    }

    const int rowHeightPx = std::max(1, rowHeight(row));
    const int visibleRows = std::max(1, viewport()->height() / rowHeightPx);
    const int padding = std::clamp(m_navigationScrollPadding, 0, std::max(0, visibleRows - 1));
    QScrollBar *bar = verticalScrollBar();
    const int topRow = std::clamp(previousTopRow, bar->minimum(), bar->maximum());
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
    if (desiredTop != bar->value()) {
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
