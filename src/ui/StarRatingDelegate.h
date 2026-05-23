#pragma once

#include <QStyledItemDelegate>

class StarRatingDelegate final : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit StarRatingDelegate(QObject *parent = nullptr);

    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;

signals:
    void ratingEdited(const QModelIndex &index, int rating0To100);
};
