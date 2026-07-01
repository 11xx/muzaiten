#pragma once

#include "ui/TrackTableRowStyle.h"

#include <QStyledItemDelegate>

class DenseTableDelegate final : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit DenseTableDelegate(QObject *parent = nullptr);

    void setHoveredRow(int row);
    // Row backing the currently-playing track (the playlist tracklist when the
    // queue is sourced from that playlist). -1 disables the indicator.
    void setPlayingRow(int row);
    void setRowStyle(const TrackTableRowStyle &style);
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    TrackTableRowStyle m_rowStyle;
    int m_hoveredRow = -1;
    int m_playingRow = -1;
};
