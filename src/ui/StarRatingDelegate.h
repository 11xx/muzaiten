#pragma once

#include "ui/TrackTableRowStyle.h"

#include <QStyledItemDelegate>

class StarRatingDelegate final : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit StarRatingDelegate(QObject *parent = nullptr);

    void setHoveredRow(int row);
    // Row backing the currently-playing track; -1 disables the indicator. Keeps
    // the rating cell's row tint in step with the rest of the row.
    void setPlayingRow(int row);
    int playingRow() const { return m_playingRow; }
    void setRowStyle(const TrackTableRowStyle &style);
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;

signals:
    void ratingEdited(const QModelIndex &index, int rating0To100);

private:
    TrackTableRowStyle m_rowStyle;
    int m_hoveredRow = -1;
    int m_playingRow = -1;
};
