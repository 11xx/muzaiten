#pragma once

#include <QStyledItemDelegate>

class DenseTableDelegate final : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit DenseTableDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};
