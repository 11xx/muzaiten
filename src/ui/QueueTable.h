#pragma once

#include "core/Track.h"
#include "search/SearchMatcher.h"
#include "ui/KeyBindingTypes.h"

#include <QWidget>

class QueueStore;
class QAbstractTableModel;
class QLabel;
class QLineEdit;
class QStyledItemDelegate;
class QTableView;
class ResponsiveColumnLayout;
class StarRatingDelegate;

enum class QueueTablePreset {
    Sidebar,
    FullScreen,
};

class QueueTable final : public QWidget {
    Q_OBJECT

public:
    explicit QueueTable(QueueTablePreset preset, QWidget *parent = nullptr);

    void setQueueStore(QueueStore *store);
    QString viewSettingsJson() const;
    void applyViewSettingsJson(const QString &json);
    void resetViewSettings();
    QWidget *navigationWidget() const;
    int rowCount() const;
    int currentRow() const;
    void setCurrentRow(int row);
    void setCurrentRow(int row, int scrollDirection);
    void moveCurrentRow(int delta);
    void activateCurrentRow();
    void revealCurrentPlaying();
    void setNavigationScrollPadding(int rows);
    void setKeyBindingProfileName(const QString &name);
    QString keyBindingProfileName() const { return m_keyBindingProfileName; }

signals:
    void trackActivated(int index);
    void trackRatingChanged(const Track &track, int rating0To100);
    void rowsMoveRequested(const QVector<int> &rows, int destinationRow);
    void rowsRemoveRequested(const QVector<int> &rows);
    void clearRequested();
    void clearPlayNextPriorityRequested();
    void findFileRequested(const Track &track);
    void trackLibraryRequested(const Track &track);
    void propertiesRequested(const Track &track);
    void viewSettingsChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void showHeaderMenu(const QPoint &pos);
    void showQueueMenu(const QPoint &pos);
    void setHoveredRow(int row);
    void updateHoverFromCursor();
    void applyPresetDefaults();
    void setHeaderHeight(int height);
    bool handleKeyPress(QKeyEvent *event);
    // Incremental find (FullScreen preset only): jumps the cursor to matching
    // rows as you type, mirroring the main-panel PanelSearchController.
    void openSearch();
    void escapeSearch();
    void setSearchQuery(const QString &query);
    void rebuildSearchMatches(bool jumpToFirst);
    void updateSearchUi();
    void cycleSearchMatch(int direction);
    bool handleSearchEditKey(QKeyEvent *event);
    int pageStepRows() const;
    QVector<int> selectedRowsForAction() const;
    Track currentTrackForAction() const;
    void scheduleRestoreScrollToCurrentRow();
    void restoreScrollToCurrentRowOnce();

    QueueTablePreset m_preset = QueueTablePreset::Sidebar;
    QueueStore *m_store = nullptr;
    KeyBindingMap m_keyBindings;
    QString m_keyBindingProfileName;
    QTableView *m_view = nullptr;
    QAbstractTableModel *m_model = nullptr;
    QStyledItemDelegate *m_itemDelegate = nullptr;
    StarRatingDelegate *m_ratingDelegate = nullptr;
    ResponsiveColumnLayout *m_columnLayout = nullptr;
    int m_hoveredRow = -1;
    int m_pendingRestoreRow = -1;
    bool m_showPlayNextBadge = true;
    bool m_showPlayNextTitleAccent = false;
    bool m_restoreScrollPending = false;
    bool m_restoreScrollScheduled = false;

    // Incremental-find state and widgets (created only for the FullScreen preset).
    QWidget *m_searchBar = nullptr;
    QLabel *m_searchPrompt = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QLabel *m_searchStatus = nullptr;
    QString m_searchQuery;
    QVector<Search::PanelMatch> m_searchMatches;
    int m_searchCurrentMatch = -1;
    bool m_searchFuzzy = false;
};
