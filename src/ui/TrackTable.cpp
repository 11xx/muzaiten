#include "ui/TrackTable.h"

#include "core/MusicSort.h"
#include "core/Track.h"
#include "ui/DenseTableDelegate.h"
#include "ui/OverlayScrollBar.h"
#include "ui/StarRating.h"
#include "ui/StarRatingDelegate.h"

#include <QAction>
#include <QAbstractTableModel>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTime>
#include <QByteArray>
#include <QWheelEvent>

#include <limits>

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

enum TrackRoles {
    SortRole = Qt::UserRole,
    TrackRole = Qt::UserRole + 1,
    HoverRatingRole = Qt::UserRole + 2,
};

QString formatDuration(qint64 durationMs)
{
    const QTime duration = QTime(0, 0).addMSecs(static_cast<int>(durationMs));
    return duration.hour() > 0 ? duration.toString(QStringLiteral("h:mm:ss")) : duration.toString(QStringLiteral("m:ss"));
}

QString displayYear(const Track &track)
{
    for (const QString &candidate : {track.originalDate, track.date}) {
        const QString trimmed = candidate.trimmed();
        if (!trimmed.isEmpty()) {
            return trimmed.left(4);
        }
    }
    return {};
}

class TrackTableModel final : public QAbstractTableModel {
public:
    explicit TrackTableModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent)
    {
    }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(std::min<qsizetype>(m_tracks.size(), std::numeric_limits<int>::max()));
    }

    int columnCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : 7;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return {};
        }
        for (const ColumnSpec &spec : columns) {
            if (spec.index == section) {
                return QString::fromLatin1(spec.label);
            }
        }
        return {};
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_tracks.size()) {
            return {};
        }

        const Track &track = m_tracks.at(index.row());
        if (role == TrackRole) {
            return QVariant::fromValue(track);
        }
        if (role == HoverRatingRole && index.column() == 0) {
            return m_hoverRatings.value(index.row(), StarRating::unset);
        }
        if (role == Qt::EditRole && index.column() == 0) {
            return track.effectiveRating0To100;
        }
        if (role == SortRole) {
            return sortValue(track, index.column());
        }
        if (role != Qt::DisplayRole && role != Qt::UserRole) {
            return {};
        }

        switch (index.column()) {
        case 0:
            return track.effectiveRating0To100;
        case 1:
            return track.trackNumber > 0 ? QString::number(track.trackNumber) : QString();
        case 2:
            return track.title;
        case 3:
            return track.albumTitle;
        case 4:
            return track.artistName;
        case 5:
            return formatDuration(track.durationMs);
        case 6:
            return displayYear(track);
        default:
            return {};
        }
    }

    bool setData(const QModelIndex &index, const QVariant &value, int role) override
    {
        if (!index.isValid() || role != HoverRatingRole || index.column() != 0 || index.row() >= m_hoverRatings.size()) {
            return false;
        }
        m_hoverRatings[index.row()] = value.toInt();
        emit dataChanged(index, index, {HoverRatingRole});
        return true;
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        return index.isValid() ? Qt::ItemIsEnabled | Qt::ItemIsSelectable : Qt::NoItemFlags;
    }

    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override
    {
        if (column < 0 || column >= columnCount()) {
            return;
        }

        // Route the clicked column through the shared sort/grouping chains so
        // ties (e.g. equal ratings) fall into a logical order rather than an
        // arbitrary one. Header clicks have no "reverse groups" control.
        const MusicSort::SortField field = sortFieldForColumn(column);
        const auto dir = order == Qt::AscendingOrder
            ? MusicSort::SortDirection::Ascending
            : MusicSort::SortDirection::Descending;

        beginResetModel();
        std::stable_sort(m_tracks.begin(), m_tracks.end(),
                         MusicSort::makeComparator<Track>(field, dir, /*reverseGroups=*/false));
        m_hoverRatings.fill(StarRating::unset, m_tracks.size());
        endResetModel();
    }

    void setTracks(const QVector<Track> &tracks)
    {
        beginResetModel();
        m_tracks = tracks;
        m_hoverRatings.fill(StarRating::unset, m_tracks.size());
        endResetModel();
    }

    Track trackAt(int row) const
    {
        return row >= 0 && row < m_tracks.size() ? m_tracks.at(row) : Track{};
    }

private:
    static MusicSort::SortField sortFieldForColumn(int column)
    {
        switch (column) {
        case 0:  return MusicSort::SortField::Rating;
        case 1:  return MusicSort::SortField::TrackNumber;
        case 2:  return MusicSort::SortField::Title;
        case 3:  return MusicSort::SortField::AlbumTitle;
        case 4:  return MusicSort::SortField::Artist;
        case 5:  return MusicSort::SortField::Duration;
        case 6:  return MusicSort::SortField::Year;
        default: return MusicSort::SortField::Title;
        }
    }

    static QVariant sortValue(const Track &track, int column)
    {
        switch (column) {
        case 0:
            return track.effectiveRating0To100;
        case 1:
            return track.trackNumber;
        case 2:
            return track.title;
        case 3:
            return track.albumTitle;
        case 4:
            return track.artistName;
        case 5:
            return track.durationMs;
        case 6:
            return displayYear(track);
        default:
            return {};
        }
    }

    QVector<Track> m_tracks;
    QVector<int> m_hoverRatings;
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
    setModel(new TrackTableModel(this));
    auto *denseDelegate = new DenseTableDelegate(this);
    setItemDelegate(denseDelegate);
    auto *ratingDelegate = new StarRatingDelegate(this);
    setItemDelegateForColumn(0, ratingDelegate);
    setSortingEnabled(true);
    setAlternatingRowColors(true);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setShowGrid(false);
    setWordWrap(false);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    horizontalHeader()->setStretchLastSection(false);
    horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    horizontalHeader()->setSectionsMovable(true);
    horizontalHeader()->setFixedHeight(20);
    horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    verticalHeader()->setDefaultSectionSize(20);
    verticalHeader()->setMinimumSectionSize(20);
    verticalHeader()->setVisible(false);
    setStyleSheet(QStringLiteral("QTableView::item { padding: 0 3px; }"));
    setContextMenuPolicy(Qt::CustomContextMenu);

    connect(ratingDelegate, &StarRatingDelegate::ratingEdited, this, [this](const QModelIndex &index, int rating) {
        const Track track = index.data(TrackRole).value<Track>();
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
        const Track track = ratingIndex.data(TrackRole).value<Track>();
        if (!track.path.isEmpty()) {
            emit trackActivated(track);
        }
    });

    OverlayScrollBar::install(this);
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

    const int rowHeight = root.value(QStringLiteral("rowHeight")).toInt(20);
    verticalHeader()->setDefaultSectionSize(std::clamp(rowHeight, 20, 48));
    setHeaderHeight(root.value(QStringLiteral("headerHeight")).toInt(20));

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
    horizontalHeader()->setFixedHeight(std::clamp(height, 18, 40));
}

void TrackTable::setTracks(const QVector<Track> &tracks)
{
    auto *trackModel = static_cast<TrackTableModel *>(model());
    if (trackModel == nullptr) {
        return;
    }
    trackModel->setTracks(tracks);
    sortByColumn(sortColumn(), sortOrder());
}

void TrackTable::mouseMoveEvent(QMouseEvent *event)
{
    const QModelIndex index = indexAt(event->pos());
    setHoveredRow(index.isValid() ? index.row() : -1);
    const QModelIndex ratingIndex = index.isValid() && index.column() == 0 ? index : QModelIndex();
    if (m_hoverRatingIndex.isValid() && m_hoverRatingIndex != ratingIndex) {
        model()->setData(m_hoverRatingIndex, StarRating::unset, HoverRatingRole);
    }
    m_hoverRatingIndex = ratingIndex;
    QTableView::mouseMoveEvent(event);
}

void TrackTable::leaveEvent(QEvent *event)
{
    setHoveredRow(-1);
    if (m_hoverRatingIndex.isValid()) {
        model()->setData(m_hoverRatingIndex, StarRating::unset, HoverRatingRole);
        m_hoverRatingIndex = QModelIndex();
    }
    QTableView::leaveEvent(event);
}

void TrackTable::setHoveredRow(int row)
{
    if (m_hoveredRow == row) {
        return;
    }

    const int previous = m_hoveredRow;
    m_hoveredRow = row;
    if (auto *denseDelegate = qobject_cast<DenseTableDelegate *>(itemDelegate())) {
        denseDelegate->setHoveredRow(row);
    }
    if (auto *ratingDelegate = qobject_cast<StarRatingDelegate *>(itemDelegateForColumn(0))) {
        ratingDelegate->setHoveredRow(row);
    }
    if (previous >= 0) {
        const QRect rect = visualRect(model()->index(previous, 0));
        viewport()->update(QRect(0, rect.top(), viewport()->width(), rect.height()));
    }
    if (row >= 0) {
        const QRect rect = visualRect(model()->index(row, 0));
        viewport()->update(QRect(0, rect.top(), viewport()->width(), rect.height()));
    }
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

    const Track track = model()->index(index.row(), 0).data(TrackRole).value<Track>();
    menu.addSeparator();
    QAction *findFile = menu.addAction(QStringLiteral("Find file"));
    connect(findFile, &QAction::triggered, this, [this, track]() {
        emit findFileRequested(track);
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
        const Track track = model()->index(selectedRow, 0).data(TrackRole).value<Track>();
        if (!track.path.isEmpty()) {
            tracks.push_back(track);
        }
    }
    return tracks;
}

void TrackTable::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        const int step = event->angleDelta().y() > 0 ? 2 : -2;
        const int rowHeight = std::clamp(verticalHeader()->defaultSectionSize() + step, 20, 48);
        verticalHeader()->setDefaultSectionSize(rowHeight);
        emit viewSettingsChanged();
        event->accept();
        return;
    }
    QTableView::wheelEvent(event);
}
