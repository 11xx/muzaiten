#include "ui/ReorderableTableWidget.h"

#include <QApplication>
#include <QHeaderView>
#include <QMouseEvent>
#include <QResizeEvent>
#include <QWidget>

#include <cstdlib>

ReorderableTableWidget::ReorderableTableWidget(int rows, int columns, QWidget *parent)
    : QTableWidget(rows, columns, parent)
{
    // A raised overlay child so the line paints above the cell widgets. A
    // stylesheet background renders reliably regardless of style/palette quirks
    // (an autoFillBackground + palette fill did not show here).
    m_dropLine = new QWidget(viewport());
    m_dropLine->setFixedHeight(2);
    m_dropLine->setStyleSheet(QStringLiteral("background-color: palette(highlight);"));
    m_dropLine->hide();
}

void ReorderableTableWidget::setFillColumn(int column)
{
    m_fillColumn = column;
}

void ReorderableTableWidget::resizeEvent(QResizeEvent *event)
{
    QTableWidget::resizeEvent(event);
    fitFillColumn();
}

void ReorderableTableWidget::fitFillColumn()
{
    if (m_fillColumn < 0 || m_fillColumn >= columnCount()) {
        return;
    }
    int other = 0;
    for (int column = 0; column < columnCount(); ++column) {
        if (column != m_fillColumn) {
            other += columnWidth(column);
        }
    }
    const int available = viewport()->width() - other;
    if (available > 60) {
        setColumnWidth(m_fillColumn, available);
    }
}

void ReorderableTableWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressRow = rowAt(event->position().toPoint().y());
        m_pressY = event->position().toPoint().y();
        m_dragging = false;
    }
    QTableWidget::mousePressEvent(event);
}

void ReorderableTableWidget::mouseMoveEvent(QMouseEvent *event)
{
    if ((event->buttons() & Qt::LeftButton) && m_pressRow >= 0) {
        const int y = event->position().toPoint().y();
        if (!m_dragging && std::abs(y - m_pressY) >= QApplication::startDragDistance()) {
            m_dragging = true;
        }
        if (m_dragging) {
            showDropLine(dropRowAt(y));
            return;
        }
    }
    QTableWidget::mouseMoveEvent(event);
}

void ReorderableTableWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragging) {
        const int target = dropRowAt(event->position().toPoint().y());
        const int source = m_pressRow;
        m_dropLine->hide();
        m_dragging = false;
        m_pressRow = -1;
        if (reorder && source >= 0 && target != source && target != source + 1) {
            reorder(source, target);
        }
        return;
    }
    m_pressRow = -1;
    QTableWidget::mouseReleaseEvent(event);
}

int ReorderableTableWidget::dropRowAt(int y) const
{
    if (rowCount() == 0) {
        return 0;
    }
    const int row = rowAt(y);
    if (row < 0) {
        return y < 0 ? 0 : rowCount();
    }
    const int top = rowViewportPosition(row);
    return y < top + rowHeight(row) / 2 ? row : row + 1;
}

void ReorderableTableWidget::showDropLine(int row)
{
    int y = 0;
    if (rowCount() > 0) {
        const int lastRow = rowCount() - 1;
        y = row > lastRow ? rowViewportPosition(lastRow) + rowHeight(lastRow)
                          : rowViewportPosition(row);
    }
    m_dropLine->setGeometry(0, y - 1, viewport()->width(), 2);
    m_dropLine->raise();
    m_dropLine->show();
}
