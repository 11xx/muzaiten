#pragma once

#include <QStyledItemDelegate>

class AlbumGridDelegate final : public QStyledItemDelegate {
    Q_OBJECT

public:
    explicit AlbumGridDelegate(QObject *parent = nullptr);

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};

