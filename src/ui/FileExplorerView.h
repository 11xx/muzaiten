#pragma once

#include "core/Track.h"

#include <QWidget>
#include <QVector>

class QLabel;
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
    void setRootPath(const QString &path);
    QString currentDirectory() const;
    void setLibraryEntries(const QStringList &directories, const QVector<Track> &tracks);

signals:
    void directoryRequested(const QString &path);
    void trackActivated(const Track &track);
    void playNextRequested(const QVector<Track> &tracks);
    void addToQueueRequested(const QVector<Track> &tracks);
    void importDirectoryRequested(const QString &path);
    void findFileRequested(const Track &track);

private:
    void refreshFreeRoam();
    void activateItem(QTreeWidgetItem *item);
    void showContextMenu(const QPoint &pos);
    void navigateUp();
    void addDirectoryItem(const QString &path);
    void addTrackItem(const Track &track);
    Track trackForFile(const QString &path) const;
    QVector<Track> tracksForDirectory(const QString &path) const;
    QVector<Track> selectedTracks() const;

    QLabel *m_pathLabel = nullptr;
    QTreeWidget *m_tree = nullptr;
    FileExplorerMode m_mode = FileExplorerMode::Library;
    QString m_currentDirectory;
};
