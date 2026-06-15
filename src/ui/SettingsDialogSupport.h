#pragma once

#include <QJsonArray>
#include <QList>

class QTableWidget;

// Small reusable helpers for settings dialogs that persist their own UI state
// (window size, per-table column widths) across sessions. Widget-agnostic so any
// configuration dialog can adopt them.
namespace settingsdialog {

// Apply saved per-column widths to an interactive-header table, falling back to
// `defaults` for columns without a saved value.
void applyColumnWidths(QTableWidget *table, const QList<int> &defaults, const QJsonArray &saved);

// Snapshot a table's current column widths for persistence.
QJsonArray columnWidths(const QTableWidget *table);

} // namespace settingsdialog
