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
    // Display-only ordering. The canonical ordinal is always preserved in the
    // DB and shown in the "#" column; sorting only reshuffles how rows are shown.
    enum class SortKey { Ordinal, AddedAt, Title, Artist, Album, Duration };

    // Key/action reference for the Keybinds dialog. Kept next to eventFilter()
    // in the .cpp — update both together.
    static KeyBindingReferenceList keyBindingReference();

public slots:
    void createPlaylist();
    void renameCurrentPlaylist();
    void editCurrentPlaylistComment();
    void editCurrentItemComment();
    void deleteCurrentPlaylist();
    void removeSelectedItems();
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
    void saveQueueAsRequested();
    void restorePreviousQueueRequested();
    void playSavedQueueRequested(const QString &snapshotId, int startIndex);
    void addSavedQueueToQueueRequested(const QString &snapshotId);
    void playNextSavedQueueRequested(const QString &snapshotId);
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
    void addSelectedItemsToPlaylist();
    void editCurrentItem();
    void showPlaylistMenu(const QPoint &pos);
    void showItemMenu(const QPoint &pos);
    void showHeaderMenu(const QPoint &pos);
    void setCurrentItemRow(int row, int direction = 0);
    int currentItemRow() const;
    void moveSelectedItems(int delta);
    void applyPlaylistRowHeights();
    QVector<qint64> displayedItemIds() const;
    void updatePaneFocus();

    QStringList selectedItemPaths(int *startIndex = nullptr) const;
    QStringList selectedOnlyItemPaths() const;
    // Maps a (possibly sorted) display row to its backing item via the id stored
    // in the cell's UserRole. Returns nullptr if the row has no live item.
    const PlaylistItem *itemForDisplayRow(int row) const;

    PlaylistDatabase *m_db = nullptr;
    qint64 m_currentPlaylistId = 0;
    QString m_currentQueueSnapshotId;
    QVector<SavedQueuePlaylistEntry> m_savedQueueEntries;
    QVector<PlaylistItem> m_items;   // canonical, ordinal order
    SortKey m_sortKey = SortKey::Ordinal;
    bool m_sortDescending = false;

    bool m_showCreatedDate = true;
    int m_playlistRowHeight = 18;
    QListWidget *m_playlistList = nullptr;
    NavigableTableView *m_itemTable = nullptr;
    PlaylistItemTableModel *m_itemModel = nullptr;
    ResponsiveColumnLayout *m_columnLayout = nullptr;
    QLabel *m_header = nullptr;
};
