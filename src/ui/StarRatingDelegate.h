#pragma once

#include <QStyledItemDelegate>

class StarRatingDelegate final : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit StarRatingDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

