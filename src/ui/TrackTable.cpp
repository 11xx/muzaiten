#include "ui/TrackTable.h"

#include "core/Track.h"
#include "ui/StarRating.h"
#include "ui/StarRatingDelegate.h"

#include <QAction>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QTime>
#include <QByteArray>

namespace {

struct ColumnSpec {
    const char *key;
    const char *label;
    int index;
};

constexpr ColumnSpec columns[] = {
    {"rating", "Rating", 0},
    {"track", "Track", 1},
    {"title", "Title", 2},
    {"album", "Album", 3},
    {"artist", "Artist", 4},
    {"duration", "Duration", 5},
    {"year", "Year", 6},
};

QString columnKey(int column)
{
    for (const ColumnSpec &spec : columns) {
        if (spec.index == column) {
            return QString::fromLatin1(spec.key);
        }
    }
    return QStringLiteral("rating");
}

int columnFromKey(const QString &key)
{
    for (const ColumnSpec &spec : columns) {
        if (key == QLatin1String(spec.key)) {
            return spec.index;
        }
    }
    return 0;
}

} // namespace

TrackTable::TrackTable(QWidget *parent)
    : QTableView(parent)
{
    auto *itemModel = new QStandardItemModel(0, 7, this);
    itemModel->setSortRole(Qt::UserRole);
    itemModel->setHorizontalHeaderLabels({
        QStringLiteral("Rating"),
        QStringLiteral("#"),
        QStringLiteral("Title"),
        QStringLiteral("Album"),
        QStringLiteral("Artist"),
        QStringLiteral("Duration"),
        QStringLiteral("Year"),
    });

    setModel(itemModel);
    auto *ratingDelegate = new StarRatingDelegate(this);
    setItemDelegateForColumn(0, ratingDelegate);
    setSortingEnabled(true);
    setAlternatingRowColors(true);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    horizontalHeader()->setStretchLastSection(false);
    horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    horizontalHeader()->setSectionsMovable(true);
    horizontalHeader()->setFixedHeight(22);
    horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    verticalHeader()->setDefaultSectionSize(24);
    verticalHeader()->setMinimumSectionSize(22);
    verticalHeader()->setVisible(false);
    setContextMenuPolicy(Qt::CustomContextMenu);

    connect(ratingDelegate, &StarRatingDelegate::ratingEdited, this, [this](const QModelIndex &index, int rating) {
        const Track track = index.data(Qt::UserRole + 1).value<Track>();
        if (!track.path.isEmpty()) {
            emit trackRatingChanged(track, rating);
        }
    });

    connect(horizontalHeader(), &QHeaderView::customContextMenuRequested, this, &TrackTable::showHeaderMenu);
    connect(this, &QTableView::customContextMenuRequested, this, &TrackTable::showCellMenu);
    connect(horizontalHeader(), &QHeaderView::sortIndicatorChanged, this, [this]() {
        emit viewSettingsChanged();
    });
    connect(horizontalHeader(), &QHeaderView::sectionMoved, this, [this]() {
        emit viewSettingsChanged();
    });
    connect(horizontalHeader(), &QHeaderView::sectionResized, this, [this]() {
        emit viewSettingsChanged();
    });

    connect(this, &QTableView::doubleClicked, this, [this](const QModelIndex &index) {
        const QModelIndex ratingIndex = this->model()->index(index.row(), 0);
        const Track track = ratingIndex.data(Qt::UserRole + 1).value<Track>();
        if (!track.path.isEmpty()) {
            emit trackActivated(track);
        }
    });
}

int TrackTable::sortColumn() const
{
    return horizontalHeader()->sortIndicatorSection();
}

Qt::SortOrder TrackTable::sortOrder() const
{
    return horizontalHeader()->sortIndicatorOrder();
}

int TrackTable::verticalScrollValue() const
{
    return verticalScrollBar()->value();
}

void TrackTable::restoreViewState(int sortColumn, Qt::SortOrder sortOrder, int verticalScrollValue)
{
    if (sortColumn >= 0 && sortColumn < model()->columnCount()) {
        sortByColumn(sortColumn, sortOrder);
    }
    verticalScrollBar()->setValue(std::clamp(verticalScrollValue, verticalScrollBar()->minimum(), verticalScrollBar()->maximum()));
}

QString TrackTable::viewSettingsJson() const
{
    QJsonArray visibleColumns;
    for (const ColumnSpec &spec : columns) {
        if (!isColumnHidden(spec.index)) {
            visibleColumns.append(QString::fromLatin1(spec.key));
        }
    }

    QJsonObject root;
    root.insert(QStringLiteral("visibleColumns"), visibleColumns);
    root.insert(QStringLiteral("sortColumn"), columnKey(sortColumn()));
    root.insert(QStringLiteral("sortOrder"), sortOrder() == Qt::DescendingOrder ? QStringLiteral("descending") : QStringLiteral("ascending"));
    root.insert(QStringLiteral("rowHeight"), verticalHeader()->defaultSectionSize());
    root.insert(QStringLiteral("headerHeight"), horizontalHeader()->height());
    root.insert(QStringLiteral("headerState"), QString::fromLatin1(horizontalHeader()->saveState().toBase64()));
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void TrackTable::applyViewSettingsJson(const QString &json)
{
    if (json.isEmpty()) {
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
    const QJsonArray visible = root.value(QStringLiteral("visibleColumns")).toArray();
    if (!visible.isEmpty()) {
        QStringList visibleKeys;
        for (const QJsonValue &value : visible) {
            visibleKeys.push_back(value.toString());
        }
        for (const ColumnSpec &spec : columns) {
            setColumnHidden(spec.index, !visibleKeys.contains(QString::fromLatin1(spec.key)));
        }
    }

    const int rowHeight = root.value(QStringLiteral("rowHeight")).toInt(24);
    verticalHeader()->setDefaultSectionSize(std::clamp(rowHeight, 22, 48));
    setHeaderHeight(root.value(QStringLiteral("headerHeight")).toInt(22));

    const int column = columnFromKey(root.value(QStringLiteral("sortColumn")).toString(QStringLiteral("rating")));
    const Qt::SortOrder order = root.value(QStringLiteral("sortOrder")).toString() == QStringLiteral("descending") ? Qt::DescendingOrder : Qt::AscendingOrder;
    sortByColumn(column, order);
    const QByteArray headerState = QByteArray::fromBase64(root.value(QStringLiteral("headerState")).toString().toLatin1());
    if (!headerState.isEmpty()) {
        horizontalHeader()->restoreState(headerState);
    }
}

void TrackTable::setHeaderHeight(int height)
{
    horizontalHeader()->setFixedHeight(std::clamp(height, 20, 40));
}

void TrackTable::setTracks(const QVector<Track> &tracks)
{
    auto *itemModel = qobject_cast<QStandardItemModel *>(model());
    itemModel->setRowCount(0);

    for (const Track &track : tracks) {
        QList<QStandardItem *> row;

        auto *rating = new QStandardItem;
        rating->setData(track.effectiveRating0To100, Qt::EditRole);
        rating->setData(track.effectiveRating0To100, Qt::UserRole);
        rating->setData(QVariant::fromValue(track), Qt::UserRole + 1);
        rating->setData(-1, Qt::UserRole + 2);
        row << rating;

        auto *trackNumber = new QStandardItem(QString::number(track.trackNumber));
        trackNumber->setData(track.trackNumber, Qt::UserRole);
        row << trackNumber;

        auto *title = new QStandardItem(track.title);
        title->setData(track.title, Qt::UserRole);
        row << title;
        auto *album = new QStandardItem(track.albumTitle);
        album->setData(track.albumTitle, Qt::UserRole);
        row << album;
        auto *artist = new QStandardItem(track.artistName);
        artist->setData(track.artistName, Qt::UserRole);
        row << artist;

        const QTime duration = QTime(0, 0).addMSecs(static_cast<int>(track.durationMs));
        auto *durationItem = new QStandardItem(duration.hour() > 0 ? duration.toString(QStringLiteral("h:mm:ss")) : duration.toString(QStringLiteral("m:ss")));
        durationItem->setData(track.durationMs, Qt::UserRole);
        row << durationItem;
        auto *year = new QStandardItem(track.date.left(4));
        year->setData(track.date.left(4), Qt::UserRole);
        row << year;

        for (QStandardItem *item : row) {
            item->setEditable(false);
        }
        itemModel->appendRow(row);
    }
}

void TrackTable::mouseMoveEvent(QMouseEvent *event)
{
    const QModelIndex index = indexAt(event->pos());
    const QModelIndex ratingIndex = index.isValid() && index.column() == 0 ? index : QModelIndex();
    if (m_hoverRatingIndex.isValid() && m_hoverRatingIndex != ratingIndex) {
        model()->setData(m_hoverRatingIndex, -1, Qt::UserRole + 2);
    }
    m_hoverRatingIndex = ratingIndex;
    QTableView::mouseMoveEvent(event);
}

void TrackTable::leaveEvent(QEvent *event)
{
    if (m_hoverRatingIndex.isValid()) {
        model()->setData(m_hoverRatingIndex, -1, Qt::UserRole + 2);
        m_hoverRatingIndex = QModelIndex();
    }
    QTableView::leaveEvent(event);
}

void TrackTable::showHeaderMenu(const QPoint &pos)
{
    QMenu menu(this);
    for (const ColumnSpec &spec : columns) {
        QAction *action = menu.addAction(QString::fromLatin1(spec.label));
        action->setCheckable(true);
        action->setChecked(!isColumnHidden(spec.index));
        connect(action, &QAction::toggled, this, [this, column = spec.index](bool checked) {
            setColumnHidden(column, !checked);
            emit viewSettingsChanged();
        });
    }
    menu.exec(horizontalHeader()->mapToGlobal(pos));
}

void TrackTable::showCellMenu(const QPoint &pos)
{
    const QModelIndex index = indexAt(pos);
    if (!index.isValid()) {
        return;
    }

    const QVector<Track> tracks = tracksForContextRow(index.row());
    if (tracks.isEmpty()) {
        return;
    }

    QMenu menu(this);
    QAction *playNext = menu.addAction(QStringLiteral("Play next"));
    connect(playNext, &QAction::triggered, this, [this, tracks]() {
        emit playNextRequested(tracks);
    });
    QAction *addToQueue = menu.addAction(QStringLiteral("Add to queue"));
    connect(addToQueue, &QAction::triggered, this, [this, tracks]() {
        emit addToQueueRequested(tracks);
    });

    const Track track = model()->index(index.row(), 0).data(Qt::UserRole + 1).value<Track>();
    menu.addSeparator();
    QAction *findFile = menu.addAction(QStringLiteral("Find file"));
    connect(findFile, &QAction::triggered, this, [this, track]() {
        emit findFileRequested(track, false);
    });
    QAction *findWritableFile = menu.addAction(QStringLiteral("Find writable file"));
    connect(findWritableFile, &QAction::triggered, this, [this, track]() {
        emit findFileRequested(track, true);
    });

    if (index.column() == 0 && track.hasUserRating) {
        menu.addSeparator();
        QAction *clear = menu.addAction(QStringLiteral("Clear rating override"));
        connect(clear, &QAction::triggered, this, [this, track]() {
            emit trackRatingChanged(track, StarRating::unset);
        });
    }
    menu.exec(viewport()->mapToGlobal(pos));
}

QVector<Track> TrackTable::tracksForContextRow(int row) const
{
    QVector<int> rows;
    const QModelIndexList selected = selectionModel() != nullptr ? selectionModel()->selectedRows(0) : QModelIndexList();
    const bool rowIsSelected = std::any_of(selected.begin(), selected.end(), [row](const QModelIndex &index) {
        return index.row() == row;
    });

    if (rowIsSelected) {
        rows.reserve(selected.size());
        for (const QModelIndex &index : selected) {
            rows.push_back(index.row());
        }
        std::sort(rows.begin(), rows.end());
    } else {
        rows.push_back(row);
    }

    QVector<Track> tracks;
    tracks.reserve(rows.size());
    for (int selectedRow : rows) {
        const Track track = model()->index(selectedRow, 0).data(Qt::UserRole + 1).value<Track>();
        if (!track.path.isEmpty()) {
            tracks.push_back(track);
        }
    }
    return tracks;
}
