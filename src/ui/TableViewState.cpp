#include "ui/TableViewState.h"

#include <QAbstractItemModel>
#include <QHash>
#include <QItemSelectionModel>
#include <QScrollBar>
#include <QTableView>

#include <algorithm>
#include <cstdlib>

TableViewState::Snapshot TableViewState::capture(const QTableView &view)
{
    Snapshot snapshot;
    const QAbstractItemModel *model = view.model();
    if (model == nullptr) {
        return snapshot;
    }

    if (const QItemSelectionModel *selection = view.selectionModel(); selection != nullptr) {
        for (const QModelIndex &index : selection->selectedRows(0)) {
            const QString identity = model->index(index.row(), 0).data(IdentityRole).toString();
            if (!identity.isEmpty()) {
                snapshot.selectedRows.push_back({identity, index.row()});
            }
        }
    }
    std::sort(snapshot.selectedRows.begin(), snapshot.selectedRows.end(), [](const Row &left, const Row &right) {
        return left.oldRow < right.oldRow;
    });

    const QModelIndex current = view.currentIndex();
    if (current.isValid()) {
        const QString identity = model->index(current.row(), 0).data(IdentityRole).toString();
        if (!identity.isEmpty()) {
            snapshot.currentRow = Row{identity, current.row()};
        }
    }

    if (view.viewport() != nullptr && !view.viewport()->rect().isEmpty()) {
        const QModelIndex topLeft = view.indexAt(view.viewport()->rect().topLeft());
        if (topLeft.isValid()) {
            const QString identity = model->index(topLeft.row(), 0).data(IdentityRole).toString();
            if (!identity.isEmpty()) {
                snapshot.viewportAnchor = ViewportAnchor{identity, view.visualRect(topLeft).top()};
            }
        }
    }

    return snapshot;
}

void TableViewState::restore(QTableView &view, const Snapshot &snapshot)
{
    QAbstractItemModel *model = view.model();
    if (model == nullptr) {
        return;
    }

    QHash<QString, int> rows;
    rows.reserve(model->rowCount());
    for (int row = 0; row < model->rowCount(); ++row) {
        const QString identity = model->index(row, 0).data(IdentityRole).toString();
        if (!identity.isEmpty()) {
            rows.insert(identity, row);
        }
    }

    QItemSelectionModel *selection = view.selectionModel();
    if (selection != nullptr) {
        selection->clearSelection();
        selection->clearCurrentIndex();
        for (const Row &captured : snapshot.selectedRows) {
            const auto row = rows.constFind(captured.identity);
            if (row != rows.constEnd()) {
                selection->select(model->index(row.value(), 0),
                                  QItemSelectionModel::Select | QItemSelectionModel::Rows);
            }
        }
    }

    std::optional<int> restoredCurrentRow;
    if (snapshot.currentRow.has_value()) {
        const auto row = rows.constFind(snapshot.currentRow->identity);
        if (row != rows.constEnd()) {
            restoredCurrentRow = row.value();
        }
    }

    if (!restoredCurrentRow.has_value()) {
        const Row *best = nullptr;
        int bestDistance = 0;
        for (const Row &captured : snapshot.selectedRows) {
            if (rows.constFind(captured.identity) == rows.constEnd()) {
                continue;
            }
            if (!snapshot.currentRow.has_value()) {
                if (best == nullptr || captured.oldRow < best->oldRow) {
                    best = &captured;
                }
                continue;
            }
            const int distance = std::abs(captured.oldRow - snapshot.currentRow->oldRow);
            if (best == nullptr || distance < bestDistance
                || (distance == bestDistance && captured.oldRow < best->oldRow)) {
                best = &captured;
                bestDistance = distance;
            }
        }
        if (best != nullptr) {
            restoredCurrentRow = rows.value(best->identity);
        }
    }

    if (selection != nullptr && restoredCurrentRow.has_value()) {
        selection->setCurrentIndex(model->index(restoredCurrentRow.value(), 0), QItemSelectionModel::NoUpdate);
    } else if (selection != nullptr && model->rowCount() > 0) {
        selection->setCurrentIndex(model->index(0, 0),
                                   QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    }

    QScrollBar *verticalScrollBar = view.verticalScrollBar();
    if (verticalScrollBar == nullptr) {
        return;
    }
    if (snapshot.viewportAnchor.has_value()) {
        const auto row = rows.constFind(snapshot.viewportAnchor->identity);
        if (row != rows.constEnd() && model->columnCount() > 0) {
            int anchorColumn = 0;
            while (anchorColumn < model->columnCount() && view.isColumnHidden(anchorColumn)) {
                ++anchorColumn;
            }
            if (anchorColumn == model->columnCount()) {
                anchorColumn = 0;
            }
            const QModelIndex anchor = model->index(row.value(), anchorColumn);
            view.scrollTo(anchor, QAbstractItemView::PositionAtTop);
            if (view.verticalScrollMode() == QAbstractItemView::ScrollPerPixel) {
                const QRect anchorRect = view.visualRect(anchor);
                verticalScrollBar->setValue(verticalScrollBar->value()
                                             + anchorRect.top() - snapshot.viewportAnchor->pixelOffset);
            }
            return;
        }
    }
    verticalScrollBar->setValue(verticalScrollBar->minimum());
}
