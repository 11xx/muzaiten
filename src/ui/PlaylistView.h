#pragma once

#include "core/Playlist.h"
#include "ui/KeyBindingTypes.h"

#include <QStringList>
#include <QVector>
#include <QWidget>

class PlaylistDatabase;
class NavigableTableView;
class ResponsiveColumnLayout;
class QListWidget;
class QLabel;
class QEvent;
class QPoint;
class QSplitter;
class PlaylistItemTableModel;

struct SavedQueuePlaylistEntry {
    QString id;
    QString name;
    QString meta;
    qint64 savedAt = 0;
    QVector<PlaylistItem> items;
};

// Key-5 playlist management centre: a playlist list on the left and the selected
// playlist's items on the right. Keyboard-first (emacs + hjkl): h/l switch panes,
// j/k/n/p move, Enter plays the focused playlist/item. Owns no data — reads/writes through a
// PlaylistDatabase supplied by MainWindow and emits path-based signals that
// MainWindow resolves against the library.
class PlaylistView final : public QWidget {
    Q_OBJECT
public:
    explicit PlaylistView(QWidget *parent = nullptr);

    enum class SelectorMetadata { None, CreatedAt, UpdatedAt, Comment };

    void setDatabase(PlaylistDatabase *db);
    void setSavedQueueEntries(const QVector<SavedQueuePlaylistEntry> &entries);
    void reloadPlaylists();
    void reloadItems();
    // Selects (or re-selects) a playlist by id after an external change.
    void selectPlaylist(qint64 playlistId);

    qint64 currentPlaylistId() const;
    QString currentQueueSnapshotId() const;
    void focusPlaylistList();
    QString viewSettingsJson() const;
    void applyViewSettingsJson(const QString &json);
    void resetViewSettings();
    void setHeaderHeight(int height);
    void configureSelectorMetadata(QWidget *parent = nullptr);
    // Display-only ordering. The canonical ordinal is always preserved in the
    // DB and shown in the "#" column; sorting only reshuffles how rows are shown.
    enum class SortKey { Ordinal, AddedAt, Title, Artist, Album, Duration };

    // Key/action reference for the Keybinds dialog. Kept next to eventFilter()
    // in the .cpp — update both together.
    static KeyBindingReferenceList keyBindingReference();

    // When true, the context menus also offer "(don't save to playlist)" queue
    // adds (the queue is mirroring a playlist).
    void setQueueIsPlaylistSourced(bool sourced) { m_queueIsPlaylistSourced = sourced; }

public slots:
    void createPlaylist();
    void renameCurrentPlaylist();
    void editCurrentPlaylistComment();
    void editCurrentItemComment();
    void deleteCurrentPlaylist();
    void removeSelectedItems();
    // Remove only the missing rows within the current selection — lets the user
    // rough-select a range and drop just the broken entries.
    void removeSelectedMissingItems();
    void exportCurrentPlaylist();
    void addSongToCurrentPlaylist();
    void importIntoCurrentPlaylist();
    void playCurrentPlaylist();
    void playNextCurrentPlaylist();
    void addCurrentPlaylistToQueue();
    void moveCurrentItemUp();
    void moveCurrentItemDown();

signals:
    void playPathsRequested(const QStringList &paths, int startIndex);
    void addPathsToQueueRequested(const QStringList &paths);
    void playNextPathsRequested(const QStringList &paths);
    // "(don't save to playlist)" variants: add to the queue only, never mirroring
    // into the playlist that backs the queue.
    void addPathsToQueueTemporaryRequested(const QStringList &paths);
    void playNextPathsTemporaryRequested(const QStringList &paths);
    void propertiesForPathRequested(const QString &path);
    // Open the add-song modal for the given playlist (RET-driven, task 9).
    void addSongRequested(qint64 playlistId);
    // Open the bulk import dialog (paste/m3u/csv → matcher) for this playlist.
    void importRequested(qint64 playlistId);
    // Re-open the add modal pre-filled with this item's remembered query to edit
    // which track it resolves to.
    void editItemRequested(qint64 playlistId, qint64 itemId, const QString &query);
    // Open the "choose playlist" dialog for these item paths (e.g. copy to another list).
    void addToPlaylistRequested(const QStringList &paths);
    void removeAllMissingTracksRequested();
    void playSavedQueueRequested(const QString &snapshotId, int startIndex);
    void addSavedQueueToQueueRequested(const QString &snapshotId);
    void playNextSavedQueueRequested(const QString &snapshotId);
    void deleteSavedQueueRequested(const QString &snapshotId);
    void viewSettingsChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void cycleAddedSort();
    void sortByColumn(int column);
    void populateItems();
    QVector<PlaylistItem> displayItems() const;
    void updateHeader();
    bool currentSelectionIsSavedQueue() const;
    QStringList pathsForSavedQueue(const QString &snapshotId, int *startIndex = nullptr) const;
    void playCurrentItem();
    void playNextSelectedItems();
    void addSelectedItemsToQueue();
    // Shared bodies for the queue-add slots/menu items. temporary=true emits the
    // "(don't save to playlist)" signals; playNext picks insertion vs append.
    void enqueueCurrentPlaylist(bool playNext, bool temporary);
    void enqueueSelectedItems(bool playNext, bool temporary);
    void addSelectedItemsToPlaylist();
    void editCurrentItem();
    void showPlaylistMenu(const QPoint &pos);
    void showItemMenu(const QPoint &pos);
    void showHeaderMenu(const QPoint &pos);
    void setCurrentItemRow(int row, int direction = 0);
    int currentItemRow() const;
    void moveSelectedItems(int delta);
    void applyPlaylistRowHeights();
    void updateSavedQueueSpacerHeight();
    QString selectorMetadataForPlaylist(const Playlist &playlist) const;
    QVector<qint64> displayedItemIds() const;
    void updatePaneFocus();

    QStringList selectedItemPaths(int *startIndex = nullptr) const;
    QStringList selectedOnlyItemPaths() const;
    // Maps a (possibly sorted) display row to its backing item via the id stored
    // in the cell's UserRole. Returns nullptr if the row has no live item.
    const PlaylistItem *itemForDisplayRow(int row) const;
    // Shared removal: all selected rows, or only the missing ones when filtered.
    void removeSelectedItemsImpl(bool missingOnly);

    PlaylistDatabase *m_db = nullptr;
    qint64 m_currentPlaylistId = 0;
    QString m_currentQueueSnapshotId;
    QVector<SavedQueuePlaylistEntry> m_savedQueueEntries;
    QVector<PlaylistItem> m_items;   // canonical, ordinal order
    SortKey m_sortKey = SortKey::Ordinal;
    bool m_sortDescending = false;

    SelectorMetadata m_selectorMetadata = SelectorMetadata::CreatedAt;
    int m_playlistRowHeight = 18;
    QListWidget *m_playlistList = nullptr;
    NavigableTableView *m_itemTable = nullptr;
    QSplitter *m_splitter = nullptr;
    PlaylistItemTableModel *m_itemModel = nullptr;
    ResponsiveColumnLayout *m_columnLayout = nullptr;
    QLabel *m_header = nullptr;
    bool m_queueIsPlaylistSourced = false;
};
