#pragma once

#include <QStyledItemDelegate>

class DenseTableDelegate final : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit DenseTableDelegate(QObject *parent = nullptr);

    void setHoveredRow(int row);
    // Row backing the currently-playing track (the playlist tracklist when the
    // queue is sourced from that playlist). -1 disables the indicator.
    void setPlayingRow(int row);
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    int m_hoveredRow = -1;
    int m_playingRow = -1;
};
