#include "ui/PlaylistView.h"

#include "db/PlaylistDatabase.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QSplitter>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

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

} // namespace

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
    m_playlistList->installEventFilter(this);

    m_itemTable = new QTableWidget(splitter);
    m_itemTable->setColumnCount(5);
    m_itemTable->setHorizontalHeaderLabels(
        {QStringLiteral("#"), QStringLiteral("Title"), QStringLiteral("Artist"),
         QStringLiteral("Album"), QStringLiteral("Length")});
    m_itemTable->verticalHeader()->setVisible(false);
    m_itemTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_itemTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_itemTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_itemTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_itemTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_itemTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_itemTable->installEventFilter(this);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 3);

    connect(m_playlistList, &QListWidget::currentRowChanged, this, [this](int) {
        m_currentPlaylistId = currentPlaylistId();
        reloadItems();
    });

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
        auto *item = new QListWidgetItem(
            QStringLiteral("%1  (%2)").arg(playlist.name).arg(playlist.itemCount), m_playlistList);
        item->setData(Qt::UserRole, playlist.id);
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
    m_itemTable->setRowCount(0);
    m_items.clear();
    if (m_db == nullptr || m_currentPlaylistId <= 0) {
        updateHeader();
        return;
    }
    m_items = m_db->items(m_currentPlaylistId);
    m_itemTable->setRowCount(static_cast<int>(m_items.size()));
    for (int row = 0; row < m_items.size(); ++row) {
        const PlaylistItem &item = m_items.at(row);
        const QString status = statusLabel(item.status);
        const QString titleText = status.isEmpty()
            ? item.titleSnapshot
            : QStringLiteral("%1  [%2]").arg(item.titleSnapshot, status);
        const QStringList cells = {QString::number(item.ordinal + 1), titleText,
                                   item.artistSnapshot, item.albumSnapshot,
                                   durationText(item.durationMs)};
        for (int col = 0; col < cells.size(); ++col) {
            auto *cell = new QTableWidgetItem(cells.at(col));
            if (item.status != PlaylistItemStatus::Matched) {
                QFont font = cell->font();
                font.setItalic(true);
                cell->setFont(font);
            }
            m_itemTable->setItem(row, col, cell);
        }
    }
    if (m_itemTable->rowCount() > 0 && m_itemTable->currentRow() < 0) {
        m_itemTable->setCurrentCell(0, 0);
    }
    updateHeader();
}

void PlaylistView::updateHeader()
{
    if (m_currentPlaylistId <= 0) {
        m_header->setText(QStringLiteral(
            "Playlists — a: new   R: rename   D: delete   l/Enter: open   "
            "(inside) =/+: add song   e: edit   d: remove   Enter: play"));
        return;
    }
    const QListWidgetItem *current = m_playlistList->currentItem();
    const QString name = current != nullptr ? current->text() : QString();
    m_header->setText(QStringLiteral("%1 — %2 items").arg(name).arg(m_items.size()));
}

QStringList PlaylistView::selectedItemPaths(int *startIndex) const
{
    QStringList paths;
    int first = -1;
    const QModelIndexList selected = m_itemTable->selectionModel() != nullptr
        ? m_itemTable->selectionModel()->selectedRows()
        : QModelIndexList();
    if (selected.size() > 1) {
        QVector<int> rows;
        for (const QModelIndex &index : selected) {
            rows.push_back(index.row());
        }
        std::sort(rows.begin(), rows.end());
        for (int row : rows) {
            if (row >= 0 && row < m_items.size() && !m_items.at(row).trackPath.isEmpty()) {
                paths << m_items.at(row).trackPath;
            }
        }
        if (startIndex != nullptr) {
            *startIndex = 0;
        }
        return paths;
    }
    // Single selection: hand the whole playlist over so playback continues past
    // the chosen row, with startIndex pointing at it.
    const int current = m_itemTable->currentRow();
    for (int row = 0; row < m_items.size(); ++row) {
        if (m_items.at(row).trackPath.isEmpty()) {
            continue;
        }
        if (row == current) {
            first = static_cast<int>(paths.size());
        }
        paths << m_items.at(row).trackPath;
    }
    if (startIndex != nullptr) {
        *startIndex = std::max(0, first);
    }
    return paths;
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

void PlaylistView::removeSelectedItems()
{
    if (m_db == nullptr || m_currentPlaylistId <= 0 || m_itemTable->selectionModel() == nullptr) {
        return;
    }
    QVector<int> rows;
    for (const QModelIndex &index : m_itemTable->selectionModel()->selectedRows()) {
        rows.push_back(index.row());
    }
    if (rows.isEmpty() && m_itemTable->currentRow() >= 0) {
        rows.push_back(m_itemTable->currentRow());
    }
    // Delete highest ordinal first so the compaction in removeItem stays valid.
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int row : rows) {
        if (row >= 0 && row < m_items.size()) {
            m_db->removeItem(m_items.at(row).id);
        }
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
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Right:
            if (m_itemTable->rowCount() > 0) {
                m_itemTable->setFocus(Qt::OtherFocusReason);
                if (m_itemTable->currentRow() < 0) {
                    m_itemTable->setCurrentCell(0, 0);
                }
            }
            return true;
        case Qt::Key_A:
        case Qt::Key_Insert:
            createPlaylist();
            return true;
        case Qt::Key_R:
        case Qt::Key_F2:
            renameCurrentPlaylist();
            return true;
        case Qt::Key_D:
        case Qt::Key_Delete:
            deleteCurrentPlaylist();
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
            m_itemTable->setCurrentCell(std::min(m_itemTable->rowCount() - 1, m_itemTable->currentRow() + 1),
                                        std::max(0, m_itemTable->currentColumn()));
            return true;
        case Qt::Key_K:
        case Qt::Key_P:
            m_itemTable->setCurrentCell(std::max(0, m_itemTable->currentRow() - 1),
                                        std::max(0, m_itemTable->currentColumn()));
            return true;
        case Qt::Key_H:
        case Qt::Key_Left:
            focusPlaylistList();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter: {
            int startIndex = 0;
            const QStringList paths = selectedItemPaths(&startIndex);
            if (!paths.isEmpty()) {
                emit playPathsRequested(paths, startIndex);
            }
            return true;
        }
        case Qt::Key_Space: {
            const QStringList paths = selectedItemPaths();
            if (!paths.isEmpty()) {
                emit addPathsToQueueRequested(paths);
            }
            return true;
        }
        case Qt::Key_Equal:  // = and + (shift+=) both open the add-song modal
        case Qt::Key_Plus:
            if (m_currentPlaylistId > 0) {
                emit addSongRequested(m_currentPlaylistId);
            }
            return true;
        case Qt::Key_E: {
            const int row = m_itemTable->currentRow();
            if (row >= 0 && row < m_items.size() && m_currentPlaylistId > 0) {
                emit editItemRequested(m_currentPlaylistId, m_items.at(row).id, m_items.at(row).query);
            }
            return true;
        }
        case Qt::Key_D:
        case Qt::Key_Delete:
            removeSelectedItems();
            return true;
        case Qt::Key_I:
            if (mods == Qt::NoModifier) {
                const int row = m_itemTable->currentRow();
                if (row >= 0 && row < m_items.size() && !m_items.at(row).trackPath.isEmpty()) {
                    emit propertiesForPathRequested(m_items.at(row).trackPath);
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
