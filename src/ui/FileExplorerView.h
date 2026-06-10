#pragma once

#include "core/MusicSort.h"
#include "core/Track.h"
#include "ui/FileExplorerKeybindings.h"

#include <QHash>
#include <QList>
#include <QWidget>
#include <QVector>

#include <functional>

class QFileInfo;
class QLabel;
class QMenu;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

enum class FileExplorerMode {
    Library,
    FreeRoam,
};

class FileExplorerView final : public QWidget {
    Q_OBJECT

public:
    explicit FileExplorerView(QWidget *parent = nullptr);

    void setMode(FileExplorerMode mode);
    FileExplorerMode mode() const;
    void setModeTitle(const QString &title);
    void setRootPath(const QString &path);
    QString currentDirectory() const;
    void setLibraryEntries(const QStringList &directories, const QVector<Track> &tracks);

    void setKeyBindingProfileName(const QString &name);
    QString keyBindingProfileName() const;
    void setKeyHintBarVisible(bool visible);
    bool isKeyHintBarVisible() const;
    QStringList availableKeyBindingProfiles() const;
    void setShowUnsupportedFiles(bool show);
    bool showUnsupportedFiles() const;
    // Resolver used to reuse already-scanned track metadata/ratings (by path)
    // instead of re-reading tags; returns a track with empty path when unknown.
    void setTrackResolver(std::function<Track(const QString &)> resolver);
    void setStartDirectory(const QString &path);
    QString startDirectory() const;
    void setRowHeight(int height);
    int rowHeight() const;
    void setSort(MusicSort::SortField field, bool descending, bool reverseGroups);
    // Navigates to the file's directory and selects it (no-op if the directory
    // can't be reached). Works for both library and free-roam modes.
    void revealFile(const QString &filePath);

signals:
    void directoryRequested(const QString &path);
    void trackActivated(const Track &track);
    void playNextRequested(const QVector<Track> &tracks);
    void addToQueueRequested(const QVector<Track> &tracks);
    void importDirectoryRequested(const QString &path);
    void findFileRequested(const Track &track);
    void propertiesRequested(const Track &track);
    void trackRatingChangeRequested(const Track &track, int rating0To100);
    void startDirectoryChanged(const QString &path);
    void rowHeightChanged(int height);
    void keyBindingProfileChanged(const QString &name);
    void keyHintVisibilityChanged(bool visible);
    void sortChanged(const QString &field, bool descending, bool reverseGroups);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void refreshFreeRoam();
    void activateItem(QTreeWidgetItem *item);
    void showContextMenu(const QPoint &pos);
    void navigateUp();
    void addDirectoryItem(const QString &path);
    void addTrackItem(const Track &track);
    void addPendingTrackItem(const QFileInfo &info);
    void addUnsupportedItem(const QFileInfo &info);
    void applyTrackToItem(QTreeWidgetItem *item, const Track &track);
    void processNextMetadata();
    void sortTopLevelItems();
    void buildSortMenu(QMenu *parent);
    void restoreSelectionForCurrentDirectory();
    void applyRowHeight();
    void applyRowHeightToItem(QTreeWidgetItem *item) const;
    Track trackForFile(const QString &path) const;
    QVector<Track> tracksForDirectory(const QString &path) const;
    QVector<Track> selectedTracks() const;

    void applyKeyAction(const QString &action);
    void updateHintBar();
    // Recompute the hint-bar palette from the live application palette so the
    // bar (background + text) tracks theme changes instead of freezing the
    // colors captured at construction.
    void applyHintBarPalette();

    QLabel *m_pathLabel = nullptr;
    QLabel *m_modeTitle = nullptr;
    QWidget *m_hintBar = nullptr;
    QLabel *m_hintLabel = nullptr;
    QTreeWidget *m_tree = nullptr;
    QTimer *m_ggTimer = nullptr;
    QTimer *m_metadataTimer = nullptr;
    QList<QTreeWidgetItem *> m_pendingMetadata;
    std::function<Track(const QString &)> m_trackResolver;
    FileExplorerMode m_mode = FileExplorerMode::Library;
    QString m_currentDirectory;
    KeyBindingMap m_keyBindings;
    QString m_keyBindingProfileName;
    bool m_pendingG = false;
    bool m_showUnsupported = false;
    QString m_startDirectory;
    QHash<QString, QString> m_lastSelectedByDir;
    bool m_restoringSelection = false;
    int m_rowHeight = 18;
    MusicSort::SortField m_sortField = MusicSort::SortField::FileName;
    bool m_sortDescending = false;
    bool m_sortReverseGroups = false;
};


