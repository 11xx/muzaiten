#pragma once

#include <QVector>
#include <QString>
#include <QtCore/Qt>

#include <optional>

class QTableView;

// Restores a table's selection, keyboard cursor, and top-visible row across a
// model reset by matching rows on identity instead of row number. Models opt in
// by returning a stable, unique, non-empty string for IdentityRole on column 0;
// rows without one are left unrestored.
class TableViewState final {
public:
    static constexpr int IdentityRole = Qt::UserRole + 900;

    struct Row {
        QString identity;
        int oldRow = -1;
    };

    struct ViewportAnchor {
        QString identity;
        int pixelOffset = 0;
    };

    struct Snapshot {
        QVector<Row> selectedRows;
        std::optional<Row> currentRow;
        std::optional<ViewportAnchor> viewportAnchor;
    };

    static Snapshot capture(const QTableView &view);
    static void restore(QTableView &view, const Snapshot &snapshot);
};
