#pragma once

#include <QStyledItemDelegate>

class StarRatingDelegate final : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit StarRatingDelegate(QObject *parent = nullptr);

    void setHoveredRow(int row);
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;

signals:
    void ratingEdited(const QModelIndex &index, int rating0To100);

private:
    int m_hoveredRow = -1;
};
