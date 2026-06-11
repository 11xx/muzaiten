#include "ui/PlaylistView.h"

#include "db/PlaylistDatabase.h"
#include "ui/DenseTableDelegate.h"
#include "ui/NavigableTableView.h"
#include "ui/NeighborColumnResizer.h"
#include "ui/OverlayScrollBar.h"

#include <QAbstractTableModel>
#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QSplitter>
#include <QTextStream>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QString statusLabel(PlaylistItemStatus status)
{
    switch (status) {
    case PlaylistItemStatus::Matched:    return QString();
    case PlaylistItemStatus::Missing:    return QStringLiteral("missing");
    case PlaylistItemStatus::Pending:    return QStringLiteral("pending");
    case PlaylistItemStatus::MultiMatch: return QStringLiteral("multi");
    }
    return QString();
}

QString durationText(qint64 ms)
{
    if (ms <= 0) {
        return QString();
    }
    const qint64 totalSeconds = ms / 1000;
    return QStringLiteral("%1:%2")
        .arg(totalSeconds / 60)
        .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'));
}

enum PlaylistItemColumn {
    OrdinalColumn,
    TitleColumn,
    ArtistColumn,
    AlbumColumn,
    LengthColumn,
    PlaylistItemColumnCount,
};

int minWidthForPlaylistItemColumn(int column)
{
    switch (column) {
    case OrdinalColumn: return 38;
    case TitleColumn: return 120;
    case ArtistColumn: return 100;
    case AlbumColumn: return 100;
    case LengthColumn: return 64;
    default: return 40;
    }
}

int defaultWidthForPlaylistItemColumn(int column)
{
    switch (column) {
    case OrdinalColumn: return 48;
    case TitleColumn: return 260;
    case ArtistColumn: return 190;
    case AlbumColumn: return 220;
    case LengthColumn: return 76;
    default: return 100;
    }
}

} // namespace

class PlaylistItemTableModel final : public QAbstractTableModel {
public:
    explicit PlaylistItemTableModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent)
    {
    }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
    }

    int columnCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : PlaylistItemColumnCount;
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
            return {};
        }

        const PlaylistItem &item = m_rows.at(index.row());
        if (role == Qt::UserRole) {
            return item.id;
        }
        if (role == Qt::FontRole && item.status != PlaylistItemStatus::Matched) {
            QFont font;
            font.setItalic(true);
            return font;
        }
        if (role == Qt::TextAlignmentRole) {
            return index.column() == OrdinalColumn || index.column() == LengthColumn
                ? QVariant(Qt::AlignRight | Qt::AlignVCenter)
                : QVariant(Qt::AlignLeft | Qt::AlignVCenter);
        }
        if (role != Qt::DisplayRole) {
            return {};
        }

        switch (index.column()) {
        case OrdinalColumn:
            return item.ordinal + 1;
        case TitleColumn: {
            const QString status = statusLabel(item.status);
            return status.isEmpty()
                ? item.titleSnapshot
                : QStringLiteral("%1  [%2]").arg(item.titleSnapshot, status);
        }
        case ArtistColumn:
            return item.artistSnapshot;
        case AlbumColumn:
            return item.albumSnapshot;
        case LengthColumn:
            return durationText(item.durationMs);
        default:
            return {};
        }
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return {};
        }
        switch (section) {
        case OrdinalColumn: return QStringLiteral("#");
        case TitleColumn: return QStringLiteral("Title");
        case ArtistColumn: return QStringLiteral("Artist");
        case AlbumColumn: return QStringLiteral("Album");
        case LengthColumn: return QStringLiteral("Length");
        default: return {};
        }
    }

    void setItems(const QVector<PlaylistItem> &rows)
    {
        beginResetModel();
        m_rows = rows;
        endResetModel();
    }

    const PlaylistItem *itemAt(int row) const
    {
        if (row < 0 || row >= m_rows.size()) {
            return nullptr;
        }
        return &m_rows.at(row);
    }

private:
    QVector<PlaylistItem> m_rows;
};

PlaylistView::PlaylistView(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_header = new QLabel(this);
    m_header->setContentsMargins(8, 4, 8, 4);
    layout->addWidget(m_header);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    layout->addWidget(splitter, 1);

    m_playlistList = new QListWidget(splitter);
    m_playlistList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_playlistList->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_playlistList->setWordWrap(true);
    m_playlistList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_playlistList->installEventFilter(this);

    m_itemModel = new PlaylistItemTableModel(this);
    m_itemTable = new NavigableTableView(splitter);
    m_itemTable->setModel(m_itemModel);
    m_itemTable->setItemDelegate(new DenseTableDelegate(this));
    m_itemTable->verticalHeader()->setVisible(false);
    m_itemTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_itemTable->setAlternatingRowColors(true);
    m_itemTable->setShowGrid(false);
    m_itemTable->setWordWrap(false);
    m_itemTable->horizontalHeader()->setStretchLastSection(false);
    m_itemTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_itemTable->horizontalHeader()->setSectionsMovable(false);
    m_itemTable->horizontalHeader()->setFixedHeight(20);
    m_itemTable->horizontalHeader()->setMinimumSectionSize(8);
    m_itemTable->verticalHeader()->setDefaultSectionSize(20);
    m_itemTable->verticalHeader()->setMinimumSectionSize(20);
    for (int col = 0; col < PlaylistItemColumnCount; ++col) {
        m_itemTable->setColumnWidth(col, defaultWidthForPlaylistItemColumn(col));
    }
    m_itemTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_itemTable->installEventFilter(this);
    OverlayScrollBar::install(m_itemTable);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);

    connect(m_playlistList, &QListWidget::currentRowChanged, this, [this](int) {
        m_currentPlaylistId = currentPlaylistId();
        reloadItems();
    });
    connect(m_itemTable->horizontalHeader(), &QHeaderView::sectionClicked,
            this, &PlaylistView::sortByColumn);
    NeighborColumnResizer::install(
        m_itemTable->horizontalHeader(), [](int column) { return minWidthForPlaylistItemColumn(column); });
    connect(m_playlistList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) {
        playCurrentPlaylist();
    });
    connect(m_itemTable, &QTableView::doubleClicked, this, [this](const QModelIndex &) {
        playCurrentItem();
    });
    connect(m_playlistList, &QWidget::customContextMenuRequested, this, &PlaylistView::showPlaylistMenu);
    connect(m_itemTable, &QWidget::customContextMenuRequested, this, &PlaylistView::showItemMenu);

    updateHeader();
}

void PlaylistView::setDatabase(PlaylistDatabase *db)
{
    m_db = db;
    reloadPlaylists();
}

qint64 PlaylistView::currentPlaylistId() const
{
    const QListWidgetItem *item = m_playlistList->currentItem();
    return item != nullptr ? item->data(Qt::UserRole).toLongLong() : 0;
}

void PlaylistView::focusPlaylistList()
{
    m_playlistList->setFocus(Qt::OtherFocusReason);
}

void PlaylistView::reloadPlaylists()
{
    if (m_db == nullptr) {
        return;
    }
    const qint64 keep = m_currentPlaylistId;
    m_playlistList->clear();
    for (const Playlist &playlist : m_db->playlists()) {
        QString text = QStringLiteral("%1  (%2)").arg(playlist.name).arg(playlist.itemCount);
        if (m_showCreatedDate && playlist.createdAt > 0) {
            const QString date = QDateTime::fromSecsSinceEpoch(playlist.createdAt).toString(QStringLiteral("d MMM yyyy"));
            text += QStringLiteral("\n%1").arg(date);
        }
        auto *item = new QListWidgetItem(text, m_playlistList);
        item->setData(Qt::UserRole, playlist.id);
        if (m_showCreatedDate && playlist.createdAt > 0) {
            item->setSizeHint(QSize(0, 40));
        }
    }
    if (keep > 0) {
        selectPlaylist(keep);
    } else if (m_playlistList->count() > 0) {
        m_playlistList->setCurrentRow(0);
    } else {
        m_currentPlaylistId = 0;
        reloadItems();
    }
    updateHeader();
}

void PlaylistView::selectPlaylist(qint64 playlistId)
{
    for (int row = 0; row < m_playlistList->count(); ++row) {
        if (m_playlistList->item(row)->data(Qt::UserRole).toLongLong() == playlistId) {
            m_playlistList->setCurrentRow(row);
            return;
        }
    }
}

void PlaylistView::reloadItems()
{
    m_items.clear();
    if (m_db != nullptr && m_currentPlaylistId > 0) {
        m_items = m_db->items(m_currentPlaylistId);
    }
    populateItems();
}

QVector<PlaylistItem> PlaylistView::displayItems() const
{
    QVector<PlaylistItem> sorted = m_items;
    if (m_sortKey == SortKey::Ordinal) {
        // Canonical order; m_items already arrives ordered by ordinal.
        if (m_sortDescending) {
            std::reverse(sorted.begin(), sorted.end());
        }
        return sorted;
    }
    const auto cmp = [this](const PlaylistItem &a, const PlaylistItem &b) {
        int c = 0;
        switch (m_sortKey) {
        case SortKey::AddedAt:
            c = a.addedAt < b.addedAt ? -1 : (a.addedAt > b.addedAt ? 1 : 0);
            break;
        case SortKey::Title:
            c = a.titleSnapshot.localeAwareCompare(b.titleSnapshot);
            break;
        case SortKey::Artist:
            c = a.artistSnapshot.localeAwareCompare(b.artistSnapshot);
            break;
        case SortKey::Album:
            c = a.albumSnapshot.localeAwareCompare(b.albumSnapshot);
            break;
        case SortKey::Duration:
            c = a.durationMs < b.durationMs ? -1 : (a.durationMs > b.durationMs ? 1 : 0);
            break;
        case SortKey::Ordinal:
            break;
        }
        if (c == 0) {
            c = a.ordinal - b.ordinal;  // stable tiebreak on canonical position
        }
        return m_sortDescending ? c > 0 : c < 0;
    };
    std::stable_sort(sorted.begin(), sorted.end(), cmp);
    return sorted;
}

void PlaylistView::populateItems()
{
    const int keepRow = currentItemRow();
    const QVector<PlaylistItem> rows = displayItems();
    m_itemModel->setItems(rows);
    if (m_itemModel->rowCount() > 0) {
        setCurrentItemRow(std::clamp(keepRow, 0, m_itemModel->rowCount() - 1));
    }
    updateHeader();
}

void PlaylistView::cycleAddedSort()
{
    // Toggle Newest -> Oldest -> back to canonical ordinal.
    if (m_sortKey != SortKey::AddedAt) {
        m_sortKey = SortKey::AddedAt;
        m_sortDescending = true;  // Newest first
    } else if (m_sortDescending) {
        m_sortDescending = false; // Oldest first
    } else {
        m_sortKey = SortKey::Ordinal;
        m_sortDescending = false;
    }
    populateItems();
}

void PlaylistView::sortByColumn(int column)
{
    const SortKey key = [column]() {
        switch (column) {
        case 1: return SortKey::Title;
        case 2: return SortKey::Artist;
        case 3: return SortKey::Album;
        case 4: return SortKey::Duration;
        default: return SortKey::Ordinal;  // "#" column restores canonical order
        }
    }();
    if (key == m_sortKey && key != SortKey::Ordinal) {
        m_sortDescending = !m_sortDescending;  // re-click flips direction
    } else {
        m_sortKey = key;
        m_sortDescending = false;
    }
    populateItems();
}

void PlaylistView::updateHeader()
{
    if (m_currentPlaylistId <= 0) {
        m_header->setText(QStringLiteral(
            "Playlists — a: new   R: rename   D: delete   x: export   =/+: add song   T: toggle date   Enter: play   l: open   "
            "(inside) =/+: add   a: add-to-playlist   e: edit   d: remove   s: sort   Enter: play"));
        return;
    }
    const QListWidgetItem *current = m_playlistList->currentItem();
    const QString name = current != nullptr ? current->text() : QString();
    m_header->setText(QStringLiteral("%1 — %2 items").arg(name).arg(m_items.size()));
}

const PlaylistItem *PlaylistView::itemForDisplayRow(int row) const
{
    return m_itemModel != nullptr ? m_itemModel->itemAt(row) : nullptr;
}

QStringList PlaylistView::selectedItemPaths(int *startIndex) const
{
    QStringList paths;
    int first = -1;
    const QModelIndexList selected = m_itemTable->selectionModel() != nullptr
        ? m_itemTable->selectionModel()->selectedRows(0)
        : QModelIndexList();
    if (selected.size() > 1) {
        QVector<int> rows;
        for (const QModelIndex &index : selected) {
            rows.push_back(index.row());
        }
        std::sort(rows.begin(), rows.end());  // honor on-screen (display) order
        for (int row : rows) {
            const PlaylistItem *item = itemForDisplayRow(row);
            if (item != nullptr && !item->trackPath.isEmpty()) {
                paths << item->trackPath;
            }
        }
        if (startIndex != nullptr) {
            *startIndex = 0;
        }
        return paths;
    }
    // Single selection: hand the whole playlist (in display order) over so
    // playback continues past the chosen row, with startIndex pointing at it.
    const int current = currentItemRow();
    for (int row = 0; row < m_itemModel->rowCount(); ++row) {
        const PlaylistItem *item = itemForDisplayRow(row);
        if (item == nullptr || item->trackPath.isEmpty()) {
            continue;
        }
        if (row == current) {
            first = static_cast<int>(paths.size());
        }
        paths << item->trackPath;
    }
    if (startIndex != nullptr) {
        *startIndex = std::max(0, first);
    }
    return paths;
}

QStringList PlaylistView::selectedOnlyItemPaths() const
{
    QStringList paths;
    QVector<int> rows;
    const QModelIndexList selected = m_itemTable->selectionModel() != nullptr
        ? m_itemTable->selectionModel()->selectedRows(0)
        : QModelIndexList();
    if (selected.isEmpty()) {
        rows.push_back(currentItemRow());
    } else {
        rows.reserve(selected.size());
        for (const QModelIndex &index : selected) {
            rows.push_back(index.row());
        }
        std::sort(rows.begin(), rows.end());
    }

    for (int row : rows) {
        const PlaylistItem *item = itemForDisplayRow(row);
        if (item != nullptr && !item->trackPath.isEmpty()) {
            paths << item->trackPath;
        }
    }
    return paths;
}

void PlaylistView::playCurrentPlaylist()
{
    if (m_db == nullptr || m_currentPlaylistId <= 0) {
        return;
    }
    QStringList paths;
    for (const PlaylistItem &item : m_db->items(m_currentPlaylistId)) {
        if (!item.trackPath.isEmpty()) {
            paths << item.trackPath;
        }
    }
    if (!paths.isEmpty()) {
        emit playPathsRequested(paths, 0);
    }
}

void PlaylistView::addCurrentPlaylistToQueue()
{
    if (m_db == nullptr || m_currentPlaylistId <= 0) {
        return;
    }
    QStringList paths;
    for (const PlaylistItem &item : m_db->items(m_currentPlaylistId)) {
        if (!item.trackPath.isEmpty()) {
            paths << item.trackPath;
        }
    }
    if (!paths.isEmpty()) {
        emit addPathsToQueueRequested(paths);
    }
}

void PlaylistView::playCurrentItem()
{
    int startIndex = 0;
    const QStringList paths = selectedItemPaths(&startIndex);
    if (!paths.isEmpty()) {
        emit playPathsRequested(paths, startIndex);
    }
}

void PlaylistView::playNextSelectedItems()
{
    const QStringList paths = selectedOnlyItemPaths();
    if (!paths.isEmpty()) {
        emit playNextPathsRequested(paths);
    }
}

void PlaylistView::addSelectedItemsToQueue()
{
    const QStringList paths = selectedOnlyItemPaths();
    if (!paths.isEmpty()) {
        emit addPathsToQueueRequested(paths);
    }
}

void PlaylistView::addSelectedItemsToPlaylist()
{
    const QStringList paths = selectedOnlyItemPaths();
    if (!paths.isEmpty()) {
        emit addToPlaylistRequested(paths);
    }
}

void PlaylistView::editCurrentItem()
{
    const PlaylistItem *item = itemForDisplayRow(currentItemRow());
    if (item != nullptr && m_currentPlaylistId > 0) {
        emit editItemRequested(m_currentPlaylistId, item->id, item->query);
    }
}

void PlaylistView::showPlaylistMenu(const QPoint &pos)
{
    const int row = m_playlistList->row(m_playlistList->itemAt(pos));
    if (row >= 0) {
        m_playlistList->setCurrentRow(row);
    }

    QMenu menu(this);
    QAction *play = menu.addAction(QStringLiteral("Play"));
    play->setEnabled(m_currentPlaylistId > 0);
    connect(play, &QAction::triggered, this, &PlaylistView::playCurrentPlaylist);
    QAction *addToQueue = menu.addAction(QStringLiteral("Add to queue"));
    addToQueue->setEnabled(m_currentPlaylistId > 0);
    connect(addToQueue, &QAction::triggered, this, &PlaylistView::addCurrentPlaylistToQueue);
    QAction *playNext = menu.addAction(QStringLiteral("Play next"));
    playNext->setEnabled(m_currentPlaylistId > 0);
    connect(playNext, &QAction::triggered, this, [this]() {
        if (m_db == nullptr || m_currentPlaylistId <= 0) {
            return;
        }
        QStringList paths;
        for (const PlaylistItem &item : m_db->items(m_currentPlaylistId)) {
            if (!item.trackPath.isEmpty()) {
                paths << item.trackPath;
            }
        }
        if (!paths.isEmpty()) {
            emit playNextPathsRequested(paths);
        }
    });

    menu.addSeparator();
    QAction *addSong = menu.addAction(QStringLiteral("Add song..."));
    addSong->setEnabled(m_currentPlaylistId > 0);
    connect(addSong, &QAction::triggered, this, [this]() {
        if (m_currentPlaylistId > 0) {
            emit addSongRequested(m_currentPlaylistId);
        }
    });
    menu.addAction(QStringLiteral("New playlist..."), this, &PlaylistView::createPlaylist);
    QAction *rename = menu.addAction(QStringLiteral("Rename"));
    rename->setEnabled(m_currentPlaylistId > 0);
    connect(rename, &QAction::triggered, this, &PlaylistView::renameCurrentPlaylist);
    QAction *exportAction = menu.addAction(QStringLiteral("Export..."));
    exportAction->setEnabled(m_currentPlaylistId > 0);
    connect(exportAction, &QAction::triggered, this, &PlaylistView::exportCurrentPlaylist);
    QAction *deleteAction = menu.addAction(QStringLiteral("Delete"));
    deleteAction->setEnabled(m_currentPlaylistId > 0);
    connect(deleteAction, &QAction::triggered, this, &PlaylistView::deleteCurrentPlaylist);

    menu.exec(m_playlistList->viewport()->mapToGlobal(pos));
}

void PlaylistView::showItemMenu(const QPoint &pos)
{
    const QModelIndex index = m_itemTable->indexAt(pos);
    if (!index.isValid()) {
        QMenu menu(this);
        QAction *addSong = menu.addAction(QStringLiteral("Add song..."));
        addSong->setEnabled(m_currentPlaylistId > 0);
        connect(addSong, &QAction::triggered, this, [this]() {
            if (m_currentPlaylistId > 0) {
                emit addSongRequested(m_currentPlaylistId);
            }
        });
        menu.exec(m_itemTable->viewport()->mapToGlobal(pos));
        return;
    }
    if (!m_itemTable->selectionModel()->isRowSelected(index.row(), QModelIndex())) {
        setCurrentItemRow(index.row());
    } else {
        m_itemTable->setCurrentIndex(m_itemModel->index(index.row(), 0));
    }

    const PlaylistItem *item = itemForDisplayRow(index.row());
    const bool hasPlayableSelection = !selectedOnlyItemPaths().isEmpty();
    QMenu menu(this);
    QAction *play = menu.addAction(QStringLiteral("Play"));
    play->setEnabled(item != nullptr && !item->trackPath.isEmpty());
    connect(play, &QAction::triggered, this, &PlaylistView::playCurrentItem);
    QAction *playNext = menu.addAction(QStringLiteral("Play next"));
    playNext->setEnabled(hasPlayableSelection);
    connect(playNext, &QAction::triggered, this, &PlaylistView::playNextSelectedItems);
    QAction *addToQueue = menu.addAction(QStringLiteral("Add to queue"));
    addToQueue->setEnabled(hasPlayableSelection);
    connect(addToQueue, &QAction::triggered, this, &PlaylistView::addSelectedItemsToQueue);
    QAction *addToPlaylist = menu.addAction(QStringLiteral("Add to playlist..."));
    addToPlaylist->setEnabled(hasPlayableSelection);
    connect(addToPlaylist, &QAction::triggered, this, &PlaylistView::addSelectedItemsToPlaylist);

    menu.addSeparator();
    QAction *edit = menu.addAction(QStringLiteral("Edit match"));
    edit->setEnabled(item != nullptr && m_currentPlaylistId > 0);
    connect(edit, &QAction::triggered, this, &PlaylistView::editCurrentItem);
    QAction *properties = menu.addAction(QStringLiteral("Properties"));
    properties->setEnabled(item != nullptr && !item->trackPath.isEmpty());
    connect(properties, &QAction::triggered, this, [this, item]() {
        if (item != nullptr && !item->trackPath.isEmpty()) {
            emit propertiesForPathRequested(item->trackPath);
        }
    });
    QAction *remove = menu.addAction(QStringLiteral("Remove selected"));
    remove->setEnabled(item != nullptr);
    connect(remove, &QAction::triggered, this, &PlaylistView::removeSelectedItems);

    menu.exec(m_itemTable->viewport()->mapToGlobal(pos));
}

void PlaylistView::setCurrentItemRow(int row, int direction)
{
    m_itemTable->setCurrentNavigationRow(row, direction);
}

int PlaylistView::currentItemRow() const
{
    return m_itemTable->currentNavigationRow();
}

void PlaylistView::createPlaylist()
{
    if (m_db == nullptr) {
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("New playlist"),
                                               QStringLiteral("Name:"), QLineEdit::Normal,
                                               QStringLiteral("New playlist"), &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    const qint64 id = m_db->createPlaylist(name);
    if (id <= 0) {
        QMessageBox::warning(this, QStringLiteral("Playlist"), m_db->lastError());
        return;
    }
    m_currentPlaylistId = id;
    reloadPlaylists();
}

void PlaylistView::renameCurrentPlaylist()
{
    if (m_db == nullptr || m_currentPlaylistId <= 0) {
        return;
    }
    const Playlist playlist = m_db->playlist(m_currentPlaylistId);
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Rename playlist"),
                                               QStringLiteral("Name:"), QLineEdit::Normal,
                                               playlist.name, &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    m_db->renamePlaylist(m_currentPlaylistId, name);
    reloadPlaylists();
}

void PlaylistView::deleteCurrentPlaylist()
{
    if (m_db == nullptr || m_currentPlaylistId <= 0) {
        return;
    }
    const Playlist playlist = m_db->playlist(m_currentPlaylistId);
    if (QMessageBox::question(this, QStringLiteral("Delete playlist"),
                              QStringLiteral("Delete \"%1\"?").arg(playlist.name))
        != QMessageBox::Yes) {
        return;
    }
    m_db->deletePlaylist(m_currentPlaylistId);
    m_currentPlaylistId = 0;
    reloadPlaylists();
}

void PlaylistView::exportCurrentPlaylist()
{
    if (m_db == nullptr || m_currentPlaylistId <= 0) {
        return;
    }
    const Playlist playlist = m_db->playlist(m_currentPlaylistId);
    const QVector<PlaylistItem> items = m_db->items(m_currentPlaylistId);

    QString selectedFilter;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export playlist"),
        QStringLiteral("%1.m3u8").arg(playlist.name),
        QStringLiteral("M3U8 playlist (*.m3u8);;CSV (*.csv)"), &selectedFilter);
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return;
    }
    QTextStream out(&file);
    const bool csv = path.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive)
        || selectedFilter.contains(QStringLiteral("csv"), Qt::CaseInsensitive);

    if (csv) {
        const auto field = [](const QString &value) {
            QString escaped = value;
            escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
            return QStringLiteral("\"%1\"").arg(escaped);
        };
        out << "ordinal,title,artist,album,duration_ms,path,status,query,comment\n";
        for (const PlaylistItem &item : items) {
            out << item.ordinal + 1 << ','
                << field(item.titleSnapshot) << ',' << field(item.artistSnapshot) << ','
                << field(item.albumSnapshot) << ',' << item.durationMs << ','
                << field(item.trackPath) << ',' << field(statusLabel(item.status)) << ','
                << field(item.query) << ',' << field(item.comment) << '\n';
        }
    } else {
        out << "#EXTM3U\n";
        for (const PlaylistItem &item : items) {
            if (item.trackPath.isEmpty()) {
                continue;  // unresolved (pending/missing) rows have no playable path
            }
            const int seconds = static_cast<int>(item.durationMs / 1000);
            out << "#EXTINF:" << seconds << ',' << item.artistSnapshot << " - "
                << item.titleSnapshot << '\n' << item.trackPath << '\n';
        }
    }
}

void PlaylistView::removeSelectedItems()
{
    if (m_db == nullptr || m_currentPlaylistId <= 0 || m_itemTable->selectionModel() == nullptr) {
        return;
    }
    QVector<int> rows;
    for (const QModelIndex &index : m_itemTable->selectionModel()->selectedRows(0)) {
        rows.push_back(index.row());
    }
    if (rows.isEmpty() && currentItemRow() >= 0) {
        rows.push_back(currentItemRow());
    }
    // Resolve to item ids up front (display rows may be sorted), then delete by
    // descending ordinal so the per-removal ordinal compaction stays valid.
    QVector<PlaylistItem> targets;
    for (int row : rows) {
        if (const PlaylistItem *item = itemForDisplayRow(row)) {
            targets.push_back(*item);
        }
    }
    std::sort(targets.begin(), targets.end(),
              [](const PlaylistItem &a, const PlaylistItem &b) { return a.ordinal > b.ordinal; });
    for (const PlaylistItem &item : targets) {
        m_db->removeItem(item.id);
    }
    reloadItems();
    reloadPlaylists();
}

bool PlaylistView::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::KeyPress) {
        return QWidget::eventFilter(watched, event);
    }
    auto *ke = static_cast<QKeyEvent *>(event);
    const int key = ke->key();
    const Qt::KeyboardModifiers mods = ke->modifiers();

    if (watched == m_playlistList) {
        switch (key) {
        case Qt::Key_J:
        case Qt::Key_N:
            m_playlistList->setCurrentRow(std::min(m_playlistList->count() - 1, m_playlistList->currentRow() + 1));
            return true;
        case Qt::Key_K:
        case Qt::Key_P:
            m_playlistList->setCurrentRow(std::max(0, m_playlistList->currentRow() - 1));
            return true;
        case Qt::Key_L:
        case Qt::Key_Right:
            if (m_currentPlaylistId > 0) {
                m_itemTable->setFocus(Qt::OtherFocusReason);
                if (currentItemRow() < 0 && m_itemModel->rowCount() > 0) {
                    setCurrentItemRow(0);
                }
            }
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            playCurrentPlaylist();
            return true;
        case Qt::Key_Space:
            addCurrentPlaylistToQueue();
            return true;
        case Qt::Key_Equal:
        case Qt::Key_Plus:
            if (m_currentPlaylistId > 0) {
                emit addSongRequested(m_currentPlaylistId);
            }
            return true;
        case Qt::Key_Insert:
        case Qt::Key_A:
            createPlaylist();
            return true;
        case Qt::Key_T:
            m_showCreatedDate = !m_showCreatedDate;
            reloadPlaylists();
            return true;
        case Qt::Key_R:
        case Qt::Key_F2:
            renameCurrentPlaylist();
            return true;
        case Qt::Key_D:
        case Qt::Key_Delete:
            deleteCurrentPlaylist();
            return true;
        case Qt::Key_X:
            exportCurrentPlaylist();
            return true;
        default:
            break;
        }
        return QWidget::eventFilter(watched, event);
    }

    if (watched == m_itemTable) {
        switch (key) {
        case Qt::Key_J:
        case Qt::Key_N:
            setCurrentItemRow(std::min(m_itemModel->rowCount() - 1, currentItemRow() + 1), +1);
            return true;
        case Qt::Key_K:
        case Qt::Key_P:
            setCurrentItemRow(std::max(0, currentItemRow() - 1), -1);
            return true;
        case Qt::Key_H:
        case Qt::Key_Left:
            focusPlaylistList();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter: {
            playCurrentItem();
            return true;
        }
        case Qt::Key_Space: {
            addSelectedItemsToQueue();
            return true;
        }
        case Qt::Key_Equal:  // = and + (shift+=) both open the add-song modal
        case Qt::Key_Plus:
            if (m_currentPlaylistId > 0) {
                emit addSongRequested(m_currentPlaylistId);
            }
            return true;
        case Qt::Key_A: {
            addSelectedItemsToPlaylist();
            return true;
        }
        case Qt::Key_E: {
            editCurrentItem();
            return true;
        }
        case Qt::Key_D:
        case Qt::Key_Delete:
            removeSelectedItems();
            return true;
        case Qt::Key_S:
            if (mods == Qt::NoModifier) {
                cycleAddedSort();
                return true;
            }
            break;
        case Qt::Key_I:
            if (mods == Qt::NoModifier) {
                const PlaylistItem *item = itemForDisplayRow(currentItemRow());
                if (item != nullptr && !item->trackPath.isEmpty()) {
                    emit propertiesForPathRequested(item->trackPath);
                }
                return true;
            }
            break;
        default:
            break;
        }
        return QWidget::eventFilter(watched, event);
    }

    return QWidget::eventFilter(watched, event);
}
