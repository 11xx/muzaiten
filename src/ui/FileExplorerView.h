#pragma once

#include "core/Track.h"
#include "ui/FileExplorerKeybindings.h"

#include <QWidget>
#include <QVector>

class QFileInfo;
class QLabel;
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

signals:
    void directoryRequested(const QString &path);
    void trackActivated(const Track &track);
    void playNextRequested(const QVector<Track> &tracks);
    void addToQueueRequested(const QVector<Track> &tracks);
    void importDirectoryRequested(const QString &path);
    void findFileRequested(const Track &track);
    void keyBindingProfileChanged(const QString &name);
    void keyHintVisibilityChanged(bool visible);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void refreshFreeRoam();
    void activateItem(QTreeWidgetItem *item);
    void showContextMenu(const QPoint &pos);
    void navigateUp();
    void addDirectoryItem(const QString &path);
    void addTrackItem(const Track &track);
    void addUnsupportedItem(const QFileInfo &info);
    Track trackForFile(const QString &path) const;
    QVector<Track> tracksForDirectory(const QString &path) const;
    QVector<Track> selectedTracks() const;

    void applyKeyAction(const QString &action);
    void updateHintBar();

    QLabel *m_pathLabel = nullptr;
    QLabel *m_modeTitle = nullptr;
    QWidget *m_hintBar = nullptr;
    QLabel *m_hintLabel = nullptr;
    QTreeWidget *m_tree = nullptr;
    QTimer *m_ggTimer = nullptr;
    FileExplorerMode m_mode = FileExplorerMode::Library;
    QString m_currentDirectory;
    KeyBindingMap m_keyBindings;
    QString m_keyBindingProfileName;
    bool m_pendingG = false;
    bool m_showUnsupported = false;
};


