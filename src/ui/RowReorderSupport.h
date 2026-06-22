#pragma once

#include <QModelIndexList>
#include <QString>
#include <QVector>

class QMimeData;

// Shared serialization for the custom internal-move row reorder used by the queue
// and playlist item tables — a single insertion-line cue instead of Qt's default
// cell-move ghost. The drag payload is just the source row indices; the owning
// widget performs the actual move (read rows -> reorder data -> rebuild).
//
// Each table picks a distinct mime subtype so a drag started in one view can
// never be dropped into another that happens to be visible at the same time.
namespace RowReorder {

inline constexpr auto queueMimeType = "application/x-muzaiten-queue-rows";
inline constexpr auto playlistMimeType = "application/x-muzaiten-playlist-rows";

// Sorted, de-duplicated source rows from a selection.
QVector<int> rowsFromIndexes(const QModelIndexList &indexes);

// A heap QMimeData carrying `rows` under `mimeType` (ownership passes to caller —
// QAbstractItemModel::mimeData() semantics).
QMimeData *encode(const QString &mimeType, const QVector<int> &rows);

bool contains(const QString &mimeType, const QMimeData *data);

QVector<int> decode(const QString &mimeType, const QMimeData *data);

} // namespace RowReorder
