#include "ui/SettingsDialogSupport.h"

#include <QJsonValue>
#include <QTableWidget>

#include <algorithm>

namespace settingsdialog {

void applyColumnWidths(QTableWidget *table, const QList<int> &defaults, const QJsonArray &saved)
{
    for (int column = 0; column < table->columnCount(); ++column) {
        const int fallback = defaults.value(column, 80);
        const int width = column < saved.size() ? saved.at(column).toInt(fallback) : fallback;
        table->setColumnWidth(column, std::max(40, width));
    }
}

QJsonArray columnWidths(const QTableWidget *table)
{
    QJsonArray widths;
    for (int column = 0; column < table->columnCount(); ++column) {
        widths.append(table->columnWidth(column));
    }
    return widths;
}

} // namespace settingsdialog
