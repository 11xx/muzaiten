#pragma once

#include "search/SearchQuery.h"

#include <QSet>
#include <QString>
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
    void setCurrentRow(int row);     // the keyboard "cursor" row (always-on highlight)
    int  currentRow() const { return m_currentRow; }
    void setRowHeight(int height);   // density control (Ctrl+scroll)
    int  rowHeight() const { return m_rowHeight; }
    // The active query/mode, used to compute match highlights for visible rows.
    void setQuery(const Search::SearchQuery &query, bool fuzzyMode);
    // Paths already present in the playlist being added to (empty in normal
    // search): those rows get a distinct wash so the user can spot them.
    void setAddedPaths(const QSet<QString> &paths);

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
    int m_currentRow = -1;
    int m_rowHeight  = 0; // 0 = auto from font
    Search::SearchQuery m_query;
    bool m_fuzzyMode = false;
    QSet<QString> m_addedPaths;
};
