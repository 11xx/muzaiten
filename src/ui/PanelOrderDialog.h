#pragma once

#include "ui/MainPanelKeybindings.h"

#include <QDialog>
#include <QVector>

class QListWidget;

// Lets the user reorder the main-view h/l focus panels by drag-and-drop and
// remove panels from the focus order via per-row checkboxes.
class PanelOrderDialog final : public QDialog {
    Q_OBJECT

public:
    // currentOrder is the active focus order (checked panels, in order). Any
    // known panel not present is shown unchecked, appended after.
    explicit PanelOrderDialog(const QVector<MainPanelId> &currentOrder, QWidget *parent = nullptr);

    // Checked rows in their current list order. Never empty (falls back to the
    // default order if the user unchecks everything).
    QVector<MainPanelId> resultOrder() const;

private:
    QListWidget *m_list = nullptr;
};
