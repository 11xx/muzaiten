#include "ui/TrackTable.h"

#include "core/HumanQuantity.h"
#include "core/MusicSort.h"
#include "core/Track.h"
#include "core/TrackDisplay.h"
#include "ui/DenseTableDelegate.h"
#include "ui/HeaderLabelStyle.h"
#include "ui/NeighborColumnResizer.h"
#include "ui/OverlayScrollBar.h"
#include "ui/ResponsiveColumnLayout.h"
#include "ui/ResponsiveColumnOptionsDialog.h"
#include "ui/RowHeightWheel.h"
#include "ui/StarRating.h"
#include "ui/StarRatingDelegate.h"

#include <QAction>
#include <QActionGroup>
#include <QAbstractTableModel>
#include <QFrame>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QCursor>
#include <QMouseEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QByteArray>
#include <QWheelEvent>

#include <algorithm>
#include <limits>

namespace {

constexpr HeaderViewStyle kTableHeaderStyle{
    HeaderLabelStyle{QFont::Normal, true, HeaderLabelTone::Muted, 0.20},
    false,
};

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
        if (orientation != Qt::Horizontal) {
            return {};
        }
        if (const QVariant styleData = headerLabelStyleData(role, kTableHeaderStyle.labels); styleData.isValid()) {
            return styleData;
        }
        if (role != Qt::DisplayRole) {
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
            return humanquantity::formatClock(track.durationMs);
        case 6:
            return trackdisplay::year(track);
        default:
            return {};
        }
    }

    bool setData(const QModelIndex &index, const QVariant &value, int role) override
    {
        if (!index.isValid() || role != HoverRatingRole || index.column() != 0
            || index.row() < 0 || index.row() >= m_hoverRatings.size()) {
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

    // In-place rating patch: updates every row that matches the path without
    // rebuilding the model, so a rating edit / file-write sync never reloads the
    // whole table. No-op for paths not currently shown.
    void updateRating(const QString &path, int effectiveRating, bool hasUserRating)
    {
        for (int row = 0; row < m_tracks.size(); ++row) {
            if (m_tracks[row].path != path) {
                continue;
            }
            m_tracks[row].effectiveRating0To100 = effectiveRating;
            m_tracks[row].hasUserRating = hasUserRating;
            const QModelIndex idx = index(row, 0);
            emit dataChanged(idx, idx, {Qt::DisplayRole, Qt::EditRole, SortRole, TrackRole});
        }
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
            return trackdisplay::year(track);
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

int minWidthForColumn(int column)
{
    switch (column) {
    case 0:
        return 56;
    case 1:
        return 36;
    case 2:
        return 160;
    case 3:
    case 4:
        return 80;
    case 5:
        return 48;
    case 6:
        return 40;
    default:
        return 36;
    }
}

int preferredWidthForColumn(int column)
{
    switch (column) {
    case 0:
        return 72;
    case 1:
        return 48;
    case 2:
        return 360;
    case 3:
        return 180;
    case 4:
        return 160;
    case 5:
        return 70;
    case 6:
        return 58;
    default:
        return 80;
    }
}

ResponsiveColumnPriority defaultPriorityForColumn(int column)
{
    switch (column) {
    case 2:
        return ResponsiveColumnPriority::Keep;
    case 3:
    case 4:
        return ResponsiveColumnPriority::Normal;
    default:
        return ResponsiveColumnPriority::HideEarly;
    }
}

QVector<int> trackResponsiveDropOrder()
{
    return {6, 1, 5, 0, 3, 4, 2};
}

QVector<int> defaultTrackVisualOrder()
{
    return {1, 2, 3, 4, 5, 6, 0};
}

QVector<ResponsiveColumnSpec> trackResponsiveSpecs()
{
    QVector<ResponsiveColumnSpec> specs;
    for (int column : trackResponsiveDropOrder()) {
        specs.push_back({
            column,
            columnKey(column),
            preferredWidthForColumn(column),
            minWidthForColumn(column),
            defaultPriorityForColumn(column),
            column == 2,
        });
    }
    return specs;
}

QSet<QString> allTrackColumnKeys()
{
    QSet<QString> keys;
    for (const ColumnSpec &spec : columns) {
        keys.insert(QString::fromLatin1(spec.key));
    }
    return keys;
}

QString priorityLabel(ResponsiveColumnPriority priority)
{
    switch (priority) {
    case ResponsiveColumnPriority::Keep:
        return QStringLiteral("Keep");
    case ResponsiveColumnPriority::Normal:
        return QStringLiteral("Hide later");
    case ResponsiveColumnPriority::HideEarly:
        return QStringLiteral("Hide early");
    }
    return QStringLiteral("Hide later");
}

QVector<ResponsiveColumnOption> responsiveColumnOptions()
{
    QVector<ResponsiveColumnOption> options;
    for (const ColumnSpec &spec : columns) {
        options.push_back({QString::fromLatin1(spec.key), QString::fromLatin1(spec.label)});
    }
    return options;
}

void applyDefaultTrackColumnOrder(QHeaderView *header)
{
    if (header == nullptr) {
        return;
    }

    const QVector<int> order = defaultTrackVisualOrder();
    for (int visual = 0; visual < order.size(); ++visual) {
        const int logical = order.at(visual);
        header->moveSection(header->visualIndex(logical), visual);
    }
}

} // namespace

TrackTable::TrackTable(QWidget *parent)
    : NavigableTableView(parent)
{
    setModel(new TrackTableModel(this));
    auto *denseDelegate = new DenseTableDelegate(this);
    setItemDelegate(denseDelegate);
    auto *ratingDelegate = new StarRatingDelegate(this);
    setItemDelegateForColumn(0, ratingDelegate);
    setSortingEnabled(true);
    setAlternatingRowColors(true);
    setShowGrid(false);
    setWordWrap(false);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    // Scrolling moves a different row under a stationary cursor without a
    // mouse-move, so re-derive the hovered row from the cursor on every scroll.
    connect(this, &NavigableTableView::contentsScrolled, this, &TrackTable::updateHoverFromCursor);
    horizontalHeader()->setStretchLastSection(false);
    horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    horizontalHeader()->setSectionsMovable(true);
    horizontalHeader()->setFixedHeight(20);
    horizontalHeader()->setMinimumSectionSize(8);
    horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    applyHeaderViewStyle(horizontalHeader(), kTableHeaderStyle);
    applyDefaultTrackColumnOrder(horizontalHeader());
    verticalHeader()->setDefaultSectionSize(20);
    verticalHeader()->setMinimumSectionSize(20);
    verticalHeader()->setVisible(false);
    setContextMenuPolicy(Qt::CustomContextMenu);

    m_columnLayout = new ResponsiveColumnLayout(this, trackResponsiveSpecs(), this);
    m_columnLayout->resetToDefaults();
    m_columnLayout->setUserVisibleColumns(allTrackColumnKeys());

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
    auto *trackColumnResizer = NeighborColumnResizer::install(
        horizontalHeader(), [](int column) { return minWidthForColumn(column); });
    connect(trackColumnResizer, qOverload<int, int>(&NeighborColumnResizer::columnResized), this, [this](int leftLogical, int rightLogical) {
        m_columnLayout->updateBaselineWidthsForResize(leftLogical, rightLogical);
    });
    connect(m_columnLayout, &ResponsiveColumnLayout::layoutSettingsChanged, this, [this]() {
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

void TrackTable::setChrome(TrackTableChrome chrome)
{
    if (m_chrome == chrome) {
        return;
    }
    m_chrome = chrome;
    if (m_chrome == TrackTableChrome::Inline) {
        setPanelBorders(panelNoBorders());
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setShowGrid(false);
        setAlternatingRowColors(false);
        viewport()->setAutoFillBackground(false);
        setAutoFillBackground(false);
    } else {
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setAlternatingRowColors(true);
        viewport()->setAutoFillBackground(true);
        setAutoFillBackground(true);
    }
    viewport()->update();
}

void TrackTable::setRowStyle(const TrackTableRowStyle &style)
{
    if (auto *denseDelegate = qobject_cast<DenseTableDelegate *>(itemDelegate())) {
        denseDelegate->setRowStyle(style);
    }
    if (auto *ratingDelegate = qobject_cast<StarRatingDelegate *>(itemDelegateForColumn(0))) {
        ratingDelegate->setRowStyle(style);
    }
    viewport()->update();
}

void TrackTable::setAutoHeightToRows(bool autoHeight)
{
    if (m_autoHeightToRows == autoHeight) {
        return;
    }
    m_autoHeightToRows = autoHeight;
    if (!m_autoHeightToRows) {
        setMinimumHeight(0);
        setMaximumHeight(QWIDGETSIZE_MAX);
        updateGeometry();
        return;
    }
    const int rows = model() != nullptr ? model()->rowCount() : 0;
    const int header = horizontalHeader() != nullptr && !horizontalHeader()->isHidden() ? horizontalHeader()->height() : 0;
    const int rowHeight = verticalHeader() != nullptr ? verticalHeader()->defaultSectionSize() : 20;
    const int frame = frameWidth() * 2;
    const int height = header + frame + std::max(1, rows) * rowHeight + 2;
    setMinimumHeight(height);
    setMaximumHeight(height);
    updateGeometry();
}

void TrackTable::updateTrackRating(const QString &path, int effectiveRating, bool hasUserRating)
{
    if (path.isEmpty()) {
        return;
    }
    if (auto *trackModel = static_cast<TrackTableModel *>(model())) {
        trackModel->updateRating(path, effectiveRating, hasUserRating);
    }
}

void TrackTable::selectTrackByPath(const QString &path)
{
    if (path.isEmpty() || model() == nullptr) {
        return;
    }
    for (int row = 0; row < model()->rowCount(); ++row) {
        const Track track = model()->index(row, 0).data(TrackRole).value<Track>();
        if (track.path == path) {
            setCurrentNavigationRow(row);
            return;
        }
    }
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
    const QSet<QString> userVisible = m_columnLayout->userVisibleColumns();
    for (const ColumnSpec &spec : columns) {
        if (userVisible.contains(QString::fromLatin1(spec.key))) {
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
    m_columnLayout->writeSavedWidthsJson(&root);
    m_columnLayout->writePrioritiesJson(&root);
    m_columnLayout->writeMinimumWidthsJson(&root);
    m_columnLayout->writeDropOrderJson(&root);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void TrackTable::applyViewSettingsJson(const QString &json)
{
    if (json.isEmpty()) {
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
    const QJsonArray visible = root.value(QStringLiteral("visibleColumns")).toArray();
    QSet<QString> visibleKeys = m_columnLayout->userVisibleColumns();
    if (!visible.isEmpty()) {
        visibleKeys.clear();
        for (const QJsonValue &value : visible) {
            const QString key = value.toString();
            if (!key.isEmpty()) {
                visibleKeys.insert(key);
            }
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
    horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_columnLayout->applyPrioritiesJson(root);
    m_columnLayout->applyMinimumWidthsJson(root);
    m_columnLayout->applyDropOrderJson(root);
    m_columnLayout->applySavedWidthsJson(root);
    m_columnLayout->setUserVisibleColumns(visibleKeys);
}

void TrackTable::resetViewSettings()
{
    const QSignalBlocker headerBlocker(horizontalHeader());
    for (const ColumnSpec &spec : columns) {
        horizontalHeader()->setSectionResizeMode(spec.index, QHeaderView::Interactive);
    }
    applyDefaultTrackColumnOrder(horizontalHeader());
    m_columnLayout->resetToDefaults();
    m_columnLayout->setUserVisibleColumns(allTrackColumnKeys());
    setHeaderHeight(20);
    verticalHeader()->setDefaultSectionSize(20);
    emit viewSettingsChanged();
}

void TrackTable::setHeaderHeight(int height)
{
    horizontalHeader()->setFixedHeight(std::clamp(height, 18, 40));
    if (m_autoHeightToRows) {
        setAutoHeightToRows(false);
        setAutoHeightToRows(true);
    }
}

void TrackTable::setTracks(const QVector<Track> &tracks)
{
    auto *trackModel = static_cast<TrackTableModel *>(model());
    if (trackModel == nullptr) {
        return;
    }
    const QString currentPath = currentIndex().isValid()
        ? currentIndex().data(TrackRole).value<Track>().path
        : QString();
    trackModel->setTracks(tracks);
    QSet<QString> availablePaths;
    availablePaths.reserve(tracks.size());
    for (const Track &track : tracks) {
        if (!track.path.isEmpty()) {
            availablePaths.insert(track.path);
        }
    }
    for (auto it = m_markedTrackPaths.begin(); it != m_markedTrackPaths.end();) {
        if (!availablePaths.contains(*it)) {
            it = m_markedTrackPaths.erase(it);
        } else {
            ++it;
        }
    }
    sortByColumn(sortColumn(), sortOrder());
    if (!currentPath.isEmpty()) {
        selectTrackByPath(currentPath);
    }
    if (model()->rowCount() > 0) {
        const int row = currentIndex().isValid() ? currentIndex().row() : 0;
        const bool rowSelected = selectionModel() != nullptr
            && currentIndex().isValid()
            && selectionModel()->isRowSelected(currentIndex().row(), currentIndex().parent());
        if (!currentIndex().isValid() || !rowSelected) {
            setCurrentRow(std::clamp(row, 0, model()->rowCount() - 1));
        }
    }
    reselectMarkedRows();
    if (m_autoHeightToRows) {
        setAutoHeightToRows(false);
        setAutoHeightToRows(true);
    }
}

int TrackTable::rowCount() const
{
    return model() != nullptr ? model()->rowCount() : 0;
}

int TrackTable::currentRow() const
{
    return currentNavigationRow();
}

void TrackTable::setCurrentRow(int row)
{
    setCurrentRow(row, 0);
}

void TrackTable::setCurrentRow(int row, int scrollDirection)
{
    setCurrentNavigationRow(row, scrollDirection);
    if (m_autoHeightToRows) {
        if (verticalScrollBar() != nullptr) {
            verticalScrollBar()->setValue(0);
        }
        if (horizontalScrollBar() != nullptr) {
            horizontalScrollBar()->setValue(0);
        }
    }
    reselectMarkedRows();
}

void TrackTable::moveCurrentRow(int delta)
{
    if (rowCount() == 0) {
        return;
    }
    const int row = currentRow() >= 0 ? currentRow() : 0;
    setCurrentRow(std::clamp(row + delta, 0, rowCount() - 1), delta);
}

void TrackTable::activateCurrentTrack()
{
    const QModelIndex index = currentIndex();
    if (!index.isValid()) {
        return;
    }
    const Track track = model()->index(index.row(), 0).data(TrackRole).value<Track>();
    if (!track.path.isEmpty()) {
        emit trackActivated(track);
    }
}

void TrackTable::addCurrentTrackToQueue()
{
    const QModelIndex index = currentIndex();
    if (!index.isValid()) {
        return;
    }
    const QVector<Track> tracks = tracksForActionRow(index.row());
    if (!tracks.isEmpty()) {
        emit addToQueueRequested(tracks);
    }
}

void TrackTable::addCurrentTrackToPlaylist()
{
    const QModelIndex index = currentIndex();
    if (!index.isValid()) {
        return;
    }
    const QVector<Track> tracks = tracksForActionRow(index.row());
    if (!tracks.isEmpty()) {
        emit addToPlaylistRequested(tracks);
    }
}

void TrackTable::playNextCurrentTrack()
{
    const QModelIndex index = currentIndex();
    if (!index.isValid()) {
        return;
    }
    const QVector<Track> tracks = tracksForActionRow(index.row());
    if (!tracks.isEmpty()) {
        emit playNextRequested(tracks);
    }
}

void TrackTable::markCurrentTrack()
{
    setCurrentTrackMarked(true);
}

void TrackTable::markAllTracks()
{
    if (model() == nullptr) {
        return;
    }
    for (int row = 0; row < model()->rowCount(); ++row) {
        const Track track = model()->index(row, 0).data(TrackRole).value<Track>();
        if (!track.path.isEmpty()) {
            m_markedTrackPaths.insert(track.path);
        }
    }
    reselectMarkedRows();
}

void TrackTable::unmarkCurrentTrack()
{
    setCurrentTrackMarked(false);
}

void TrackTable::unmarkAllTracks()
{
    m_markedTrackPaths.clear();
    if (selectionModel() != nullptr) {
        selectionModel()->clearSelection();
    }
    if (currentIndex().isValid()) {
        setCurrentNavigationRow(currentIndex().row());
    }
}

QVector<Search::MatchDocument> TrackTable::searchDocuments() const
{
    QVector<Search::MatchDocument> docs;
    if (model() == nullptr) {
        return docs;
    }
    docs.reserve(model()->rowCount());
    for (int row = 0; row < model()->rowCount(); ++row) {
        const Track track = model()->index(row, 0).data(TrackRole).value<Track>();
        const QString title = track.title.isEmpty() ? track.filename : track.title;
        const QString free = QStringLiteral("%1 %2 %3 %4 %5 %6")
                                 .arg(title,
                                      track.artistName,
                                      track.albumArtistName,
                                      track.albumTitle,
                                      track.filename,
                                      track.path);
        QVector<Search::MatchNumeric> numeric;
        const int year = trackdisplay::year(track).toInt();
        if (year > 0) numeric.push_back({Search::TermKind::Year, year});
        if (track.effectiveRating0To100 >= 0) numeric.push_back({Search::TermKind::Rating, track.effectiveRating0To100});
        if (track.durationMs > 0) numeric.push_back({Search::TermKind::DurationMs, track.durationMs});
        if (track.sampleRateHz > 0) numeric.push_back({Search::TermKind::SampleRateHz, track.sampleRateHz});
        if (track.bitrateKbps > 0) numeric.push_back({Search::TermKind::BitrateKbps, track.bitrateKbps});
        if (track.channels > 0) numeric.push_back({Search::TermKind::Channels, track.channels});
        docs.push_back({
            row,
            {
                Search::makeField(Search::MatchFieldRole::Title, title, 400),
                Search::makeField(Search::MatchFieldRole::Artist, track.artistName, 300),
                Search::makeField(Search::MatchFieldRole::AlbumArtist, track.albumArtistName, 300),
                Search::makeField(Search::MatchFieldRole::Album, track.albumTitle, 200),
                Search::makeField(Search::MatchFieldRole::Filename, track.filename, 60),
                Search::makeField(Search::MatchFieldRole::Path, track.path, 60),
                Search::makeField(Search::MatchFieldRole::Codec, track.codec, 60),
                Search::makeField(Search::MatchFieldRole::Free, free, 100),
            },
            numeric,
        });
    }
    return docs;
}

void TrackTable::changeEvent(QEvent *event)
{
    NavigableTableView::changeEvent(event);
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::StyleChange) {
        applyHeaderViewStyle(horizontalHeader(), kTableHeaderStyle);
        viewport()->update();
        horizontalHeader()->viewport()->update();
    }
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

void TrackTable::updateHoverFromCursor()
{
    const QPoint pos = viewport()->mapFromGlobal(QCursor::pos());
    if (!viewport()->rect().contains(pos)) {
        setHoveredRow(-1);
        return;
    }
    const QModelIndex index = indexAt(pos);
    setHoveredRow(index.isValid() ? index.row() : -1);
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
    QSet<QString> visibleKeys = m_columnLayout->userVisibleColumns();
    for (const ColumnSpec &spec : columns) {
        QAction *action = menu.addAction(QString::fromLatin1(spec.label));
        action->setCheckable(true);
        const QString key = QString::fromLatin1(spec.key);
        action->setChecked(visibleKeys.contains(key));
        connect(action, &QAction::toggled, this, [this, key](bool checked) {
            QSet<QString> keys = m_columnLayout->userVisibleColumns();
            if (checked) {
                keys.insert(key);
            } else {
                keys.remove(key);
            }
            m_columnLayout->setUserVisibleColumns(keys);
            emit viewSettingsChanged();
        });
    }
    menu.addSeparator();
    QMenu *priorityMenu = menu.addMenu(QStringLiteral("Responsive priority"));
    for (const ColumnSpec &spec : columns) {
        const QString key = QString::fromLatin1(spec.key);
        QMenu *columnMenu = priorityMenu->addMenu(QString::fromLatin1(spec.label));
        auto *group = new QActionGroup(columnMenu);
        group->setExclusive(true);
        for (const ResponsiveColumnPriority priority : {ResponsiveColumnPriority::Keep,
                                                        ResponsiveColumnPriority::Normal,
                                                        ResponsiveColumnPriority::HideEarly}) {
            QAction *action = columnMenu->addAction(priorityLabel(priority));
            action->setCheckable(true);
            action->setActionGroup(group);
            action->setChecked(m_columnLayout->columnPriority(key) == priority);
            connect(action, &QAction::triggered, this, [this, key, priority]() {
                m_columnLayout->setColumnPriority(key, priority);
            });
        }
    }
    menu.addSeparator();
    QAction *responsiveOptions = menu.addAction(QStringLiteral("Responsive options..."));
    connect(responsiveOptions, &QAction::triggered, this, [this]() {
        ResponsiveColumnOptionsDialog dialog(m_columnLayout, responsiveColumnOptions(), this);
        dialog.exec();
    });
    menu.addSeparator();
    QAction *resetLayout = menu.addAction(QStringLiteral("Reset table layout to defaults"));
    connect(resetLayout, &QAction::triggered, this, &TrackTable::resetViewSettings);
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
    if (m_queueIsPlaylistSourced) {
        QAction *playNextTemp = menu.addAction(QStringLiteral("Play next (don't save to playlist)"));
        connect(playNextTemp, &QAction::triggered, this, [this, tracks]() {
            emit playNextTemporaryRequested(tracks);
        });
        QAction *addToQueueTemp = menu.addAction(QStringLiteral("Add to queue (don't save to playlist)"));
        connect(addToQueueTemp, &QAction::triggered, this, [this, tracks]() {
            emit addToQueueTemporaryRequested(tracks);
        });
    }
    QAction *addToPlaylist = menu.addAction(QStringLiteral("Add to playlist…"));
    connect(addToPlaylist, &QAction::triggered, this, [this, tracks]() {
        emit addToPlaylistRequested(tracks);
    });

    const Track track = model()->index(index.row(), 0).data(TrackRole).value<Track>();
    menu.addSeparator();
    QAction *findFile = menu.addAction(QStringLiteral("Open containing directory"));
    connect(findFile, &QAction::triggered, this, [this, track]() {
        emit findFileRequested(track);
    });
    QAction *properties = menu.addAction(QStringLiteral("Properties"));
    connect(properties, &QAction::triggered, this, [this, track]() {
        emit propertiesRequested(track);
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

QVector<Track> TrackTable::tracksForActionRow(int row) const
{
    if (model() == nullptr) {
        return {};
    }
    if (m_markedTrackPaths.isEmpty()) {
        const Track track = model()->index(row, 0).data(TrackRole).value<Track>();
        return track.path.isEmpty() ? QVector<Track>() : QVector<Track>{track};
    }

    QVector<Track> tracks;
    tracks.reserve(m_markedTrackPaths.size());
    for (int currentRow = 0; currentRow < model()->rowCount(); ++currentRow) {
        const Track track = model()->index(currentRow, 0).data(TrackRole).value<Track>();
        if (!track.path.isEmpty() && m_markedTrackPaths.contains(track.path)) {
            tracks.push_back(track);
        }
    }
    return tracks;
}

void TrackTable::reselectMarkedRows()
{
    if (model() == nullptr || selectionModel() == nullptr || m_markedTrackPaths.isEmpty()) {
        return;
    }
    for (int row = 0; row < model()->rowCount(); ++row) {
        const Track track = model()->index(row, 0).data(TrackRole).value<Track>();
        if (!track.path.isEmpty() && m_markedTrackPaths.contains(track.path)) {
            selectionModel()->select(model()->index(row, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
    }
}

void TrackTable::setCurrentTrackMarked(bool marked)
{
    if (model() == nullptr || !currentIndex().isValid()) {
        return;
    }
    const Track track = model()->index(currentIndex().row(), 0).data(TrackRole).value<Track>();
    if (track.path.isEmpty()) {
        return;
    }
    if (marked) {
        m_markedTrackPaths.insert(track.path);
    } else {
        m_markedTrackPaths.remove(track.path);
    }
    reselectMarkedRows();
    if (!marked && selectionModel() != nullptr) {
        selectionModel()->select(currentIndex(), QItemSelectionModel::Deselect | QItemSelectionModel::Rows);
        selectionModel()->select(currentIndex(), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
}

void TrackTable::wheelEvent(QWheelEvent *event)
{
    if (ui::applyCtrlWheelRowHeight(event, verticalHeader()->defaultSectionSize(), 20, 48, [this](int h) {
            verticalHeader()->setDefaultSectionSize(h);
            emit viewSettingsChanged();
        })) {
        return;
    }
    QTableView::wheelEvent(event);
}
