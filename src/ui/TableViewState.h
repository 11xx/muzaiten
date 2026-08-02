#pragma once

#include <QVector>
#include <QString>
#include <QtCore/Qt>

#include <optional>

class QTableView;

class TableViewState final {
public:
    static constexpr int IdentityRole = Qt::UserRole + 3;

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
