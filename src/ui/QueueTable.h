#pragma once

#include "core/Track.h"
#include "search/SearchMatcher.h"
#include "ui/KeyBindingTypes.h"
#include "ui/PanelBorderStyle.h"

#include <QWidget>

#include <functional>

class QueueStore;
class QAbstractTableModel;
class QLabel;
class QLineEdit;
class QStyledItemDelegate;
class QTableView;
class PanelSearchBar;
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
    void setPickReasonResolver(std::function<QString(const QString &)> resolver);
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
    void setTableBorders(PanelBorderEdges edges);
    void setTableTopBorderVisible(bool visible);
    // Gates the "Unlink queue from playlist" context-menu entry, shown only while
    // the live queue is mirroring a playlist.
    void setQueueIsPlaylistSourced(bool sourced);

signals:
    void trackActivated(int index);
    void startRadioRequested(const Track &track);
    void trackRatingChanged(const Track &track, int rating0To100);
    void rowsMoveRequested(const QVector<int> &rows, int destinationRow);
    void rowsRemoveRequested(const QVector<int> &rows);
    void removeAllMissingTracksRequested();
    void clearRequested();
    void clearPlayNextPriorityRequested();
    void saveQueueAsRequested();
    void restorePreviousQueueRequested();
    void unlinkFromPlaylistRequested();
    void findFileRequested(const Track &track);
    void trackLibraryRequested(const Track &track);
    void addToPlaylistRequested(const QVector<Track> &tracks);
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
    void applyTableFrameStyle();
    void setHeaderHeight(int height);
    bool handleKeyPress(QKeyEvent *event);
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
    PanelBorderEdges m_tableBorders;
    bool m_restoreScrollPending = false;
    bool m_restoreScrollScheduled = false;
    bool m_queueIsPlaylistSourced = false;

    // Incremental "/" find (FullScreen preset only); null for the sidebar preset.
    PanelSearchBar *m_search = nullptr;
};
