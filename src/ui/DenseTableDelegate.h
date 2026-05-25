#pragma once

#include <QStyledItemDelegate>

class DenseTableDelegate final : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit DenseTableDelegate(QObject *parent = nullptr);

    void setHoveredRow(int row);
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    int m_hoveredRow = -1;
};
