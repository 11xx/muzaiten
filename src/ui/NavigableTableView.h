#pragma once

#include "ui/PanelBorderStyle.h"

#include <QString>
#include <QTableView>
#include <QVector>

class NavigableTableView : public QTableView {
    Q_OBJECT

public:
    explicit NavigableTableView(QWidget *parent = nullptr);
    ~NavigableTableView() override;

    void refreshTheme();
    void setPanelBorders(PanelBorderEdges edges);

    // Opt into the custom internal-move row reorder (queue/playlist style): a
    // single insertion line is drawn between rows while dragging, and a completed
    // drag emits rowsReorderRequested() rather than letting Qt shuffle cells. The
    // backing model must mark its rows drag/drop-enabled and serialize the dragged
    // rows under `mimeType` (see RowReorderSupport.h). The owner performs the move.
    void enableRowReorder(const QString &mimeType);

    void setNavigationScrollPadding(int rows);
    int navigationScrollPadding() const;

    int currentNavigationRow() const;
    void setCurrentNavigationRow(int row, int direction = 0);

    void setMainPanelActive(bool active);
    bool mainPanelActive() const;

signals:
    void navigationRowChanged(int row);
    // A completed reorder drag: `rows` (the dragged source rows) should land at
    // `destinationRow` (an insertion index in 0..rowCount). Only emitted when
    // enableRowReorder() was called.
    void rowsReorderRequested(const QVector<int> &rows, int destinationRow);
    // Emitted after the viewport scrolls (wheel, scrollbar, or keyboard), so
    // owners can re-derive the hovered row from the cursor — a scroll moves a
    // different row under a stationary mouse without firing a mouse-move.
    void contentsScrolled();

protected:
    void rowsInserted(const QModelIndex &parent, int start, int end) override;
    void rowsAboutToBeRemoved(const QModelIndex &parent, int start, int end) override;
    void currentChanged(const QModelIndex &current, const QModelIndex &previous) override;
    void changeEvent(QEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    bool reorderEnabled() const { return !m_reorderMimeType.isEmpty(); }
    int reorderRowForPosition(const QPoint &pos) const;
    int reorderYForRow(int row) const;
    void setReorderDropRow(int row);
    void scrollNavigationRowToAnchor(int row, int direction, int previousTopRow);
    void updateRow(const QModelIndex &index);
    void refreshInactiveHighlight();
    void applyPanelBorderStyle(bool force = false);
    void refreshWidgetTheme(QWidget *widget);

    PanelBorderEdges m_panelBorders;
    QString m_reorderMimeType;       // empty unless enableRowReorder() was called
    int m_reorderDropRow = -1;       // insertion index being previewed, or -1
    int m_navigationScrollPadding = 3;
    bool m_mainPanelActive = false;
    bool m_refreshingPalette = false;
    bool m_hasPanelBorders = false;
    bool m_panelBorderStyleApplied = false;
    bool m_restylingPanelBorders = false;
};
