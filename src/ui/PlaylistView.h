#pragma once

#include "core/Playlist.h"

#include <QStringList>
#include <QVector>
#include <QWidget>

class PlaylistDatabase;
class QListWidget;
class QTableWidget;
class QLabel;
class QEvent;

// Key-5 playlist management centre: a playlist list on the left and the selected
// playlist's items on the right. Keyboard-first (emacs + hjkl): h/l switch panes,
// j/k/n/p move, Enter plays. Owns no data — reads/writes through a
// PlaylistDatabase supplied by MainWindow and emits path-based signals that
// MainWindow resolves against the library.
class PlaylistView final : public QWidget {
    Q_OBJECT
public:
    explicit PlaylistView(QWidget *parent = nullptr);

    void setDatabase(PlaylistDatabase *db);
    void reloadPlaylists();
    void reloadItems();
    // Selects (or re-selects) a playlist by id after an external change.
    void selectPlaylist(qint64 playlistId);

    qint64 currentPlaylistId() const;
    void focusPlaylistList();

signals:
    void playPathsRequested(const QStringList &paths, int startIndex);
    void addPathsToQueueRequested(const QStringList &paths);
    void playNextPathsRequested(const QStringList &paths);
    void propertiesForPathRequested(const QString &path);
    // Open the add-song modal for the given playlist (RET-driven, task 9).
    void addSongRequested(qint64 playlistId);
    // Re-open the add modal pre-filled with this item's remembered query to edit
    // which track it resolves to.
    void editItemRequested(qint64 playlistId, qint64 itemId, const QString &query);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // Display-only ordering. The canonical ordinal is always preserved in the
    // DB and shown in the "#" column; sorting only reshuffles how rows are shown.
    enum class SortKey { Ordinal, AddedAt, Title, Artist, Album, Duration };

    void createPlaylist();
    void renameCurrentPlaylist();
    void deleteCurrentPlaylist();
    void removeSelectedItems();
    void exportCurrentPlaylist();
    void cycleAddedSort();
    void sortByColumn(int column);
    void populateItems();
    QVector<PlaylistItem> displayItems() const;
    void updateHeader();

    QStringList selectedItemPaths(int *startIndex = nullptr) const;
    // Maps a (possibly sorted) display row to its backing item via the id stored
    // in the cell's UserRole. Returns nullptr if the row has no live item.
    const PlaylistItem *itemForDisplayRow(int row) const;

    PlaylistDatabase *m_db = nullptr;
    qint64 m_currentPlaylistId = 0;
    QVector<PlaylistItem> m_items;   // canonical, ordinal order
    SortKey m_sortKey = SortKey::Ordinal;
    bool m_sortDescending = false;

    QListWidget *m_playlistList = nullptr;
    QTableWidget *m_itemTable = nullptr;
    QLabel *m_header = nullptr;
};
