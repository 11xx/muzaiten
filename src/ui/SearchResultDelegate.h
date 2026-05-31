#pragma once

#include <QStyledItemDelegate>

class SearchResultDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    explicit SearchResultDelegate(QObject *parent = nullptr);

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;

    void paint(QPainter *painter,
               const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;

    void setHoveredRow(int row);
    int  hoveredRow() const { return m_hoveredRow; }
    void setRowHeight(int height);   // density control (Ctrl+scroll)
    int  rowHeight() const { return m_rowHeight; }

private:
    // Draw a single line of text with highlighted match positions.
    // matchPositions: character indices within `text` that are highlighted.
    void drawHighlightedText(QPainter *painter,
                              const QRect &rect,
                              Qt::Alignment alignment,
                              const QString &text,
                              const QVector<int> &matchPositions,
                              const QColor &normalColor,
                              const QColor &matchColor,
                              const QFontMetrics &fm) const;

    int m_hoveredRow = -1;
    int m_rowHeight  = 0; // 0 = auto from font
};
