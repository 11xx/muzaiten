#include "ui/RowReorderSupport.h"

#include <QDataStream>
#include <QIODevice>
#include <QMimeData>
#include <QModelIndexList>

#include <algorithm>

namespace RowReorder {

QVector<int> rowsFromIndexes(const QModelIndexList &indexes)
{
    QVector<int> rows;
    rows.reserve(indexes.size());
    for (const QModelIndex &index : indexes) {
        if (index.isValid() && !rows.contains(index.row())) {
            rows.push_back(index.row());
        }
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

QMimeData *encode(const QString &mimeType, const QVector<int> &rows)
{
    auto *mime = new QMimeData;
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream << rows;
    mime->setData(mimeType, payload);
    return mime;
}

bool contains(const QString &mimeType, const QMimeData *data)
{
    return data != nullptr && data->hasFormat(mimeType);
}

QVector<int> decode(const QString &mimeType, const QMimeData *data)
{
    QVector<int> rows;
    if (!contains(mimeType, data)) {
        return rows;
    }
    QByteArray payload = data->data(mimeType);
    QDataStream stream(&payload, QIODevice::ReadOnly);
    stream >> rows;
    return rows;
}

} // namespace RowReorder
