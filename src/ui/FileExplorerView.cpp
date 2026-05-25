#include "ui/FileExplorerView.h"

#include "scanner/LibraryScanner.h"
#include "scanner/TagReader.h"

#include <QAction>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVariant>

namespace {

enum ItemType {
    DirectoryItem = 1,
    TrackItem = 2,
};

enum ItemRoles {
    TypeRole = Qt::UserRole,
    PathRole = Qt::UserRole + 1,
    TrackRole = Qt::UserRole + 2,
};

QString cleanPath(const QString &path)
{
    const QString cleaned = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (cleaned.size() > 1 && cleaned.endsWith(QLatin1Char('/'))) {
        return cleaned.left(cleaned.size() - 1);
    }
    return cleaned;
}

QString formatDuration(qint64 durationMs)
{
    if (durationMs <= 0) {
        return QString();
    }
    const qint64 totalSeconds = durationMs / 1000;
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}

} // namespace

FileExplorerView::FileExplorerView(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *bar = new QWidget(this);
    auto *barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(8, 4, 8, 4);
    auto *up = new QPushButton(QStringLiteral("Up"), bar);
    up->setFixedHeight(24);
    m_pathLabel = new QLabel(bar);
    m_pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    barLayout->addWidget(up);
    barLayout->addWidget(m_pathLabel, 1);
    layout->addWidget(bar);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(4);
    m_tree->setHeaderLabels({
        QStringLiteral("Name"),
        QStringLiteral("Artist"),
        QStringLiteral("Album"),
        QStringLiteral("Duration"),
    });
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->setRootIsDecorated(false);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_tree, 1);

    connect(up, &QPushButton::clicked, this, &FileExplorerView::navigateUp);
    connect(m_tree, &QTreeWidget::itemActivated, this, &FileExplorerView::activateItem);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &FileExplorerView::showContextMenu);
}

void FileExplorerView::setMode(FileExplorerMode mode)
{
    if (m_mode == mode) {
        return;
    }
    m_mode = mode;
    m_tree->clear();
}

FileExplorerMode FileExplorerView::mode() const
{
    return m_mode;
}

void FileExplorerView::setRootPath(const QString &path)
{
    m_currentDirectory = cleanPath(path);
    m_pathLabel->setText(m_currentDirectory);
    if (m_mode == FileExplorerMode::FreeRoam) {
        refreshFreeRoam();
    }
}

QString FileExplorerView::currentDirectory() const
{
    return m_currentDirectory;
}

void FileExplorerView::setLibraryEntries(const QStringList &directories, const QVector<Track> &tracks)
{
    m_tree->clear();
    m_pathLabel->setText(m_currentDirectory.isEmpty() ? QStringLiteral("Library") : m_currentDirectory);
    for (const QString &directory : directories) {
        addDirectoryItem(directory);
    }
    for (const Track &track : tracks) {
        addTrackItem(track);
    }
}

void FileExplorerView::refreshFreeRoam()
{
    m_tree->clear();
    const QDir dir(m_currentDirectory);
    if (!dir.exists()) {
        return;
    }

    const QFileInfoList entries = dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::Readable | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (entry.isSymLink()) {
            continue;
        }
        if (entry.isDir()) {
            addDirectoryItem(entry.absoluteFilePath());
        } else if (LibraryScanner::isSupportedAudioFile(entry.absoluteFilePath())) {
            addTrackItem(trackForFile(entry.absoluteFilePath()));
        }
    }
}

void FileExplorerView::activateItem(QTreeWidgetItem *item)
{
    if (item == nullptr) {
        return;
    }
    if (item->data(0, TypeRole).toInt() == DirectoryItem) {
        emit directoryRequested(item->data(0, PathRole).toString());
        return;
    }
    const Track track = item->data(0, TrackRole).value<Track>();
    if (!track.path.isEmpty()) {
        emit trackActivated(track);
    }
}

void FileExplorerView::showContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_tree->itemAt(pos);
    if (item == nullptr) {
        return;
    }

    QMenu menu(this);
    const int type = item->data(0, TypeRole).toInt();
    if (type == DirectoryItem) {
        const QString path = item->data(0, PathRole).toString();
        QAction *playAll = menu.addAction(QStringLiteral("Play all tracks"));
        QAction *playNext = menu.addAction(QStringLiteral("Play next"));
        QAction *addQueue = menu.addAction(QStringLiteral("Add to queue"));
        QAction *scan = menu.addAction(QStringLiteral("Scan/Add this directory to library"));
        const QAction *selected = menu.exec(m_tree->viewport()->mapToGlobal(pos));
        if (selected == nullptr) {
            return;
        }
        const QVector<Track> tracks = tracksForDirectory(path);
        if (selected == playAll && !tracks.isEmpty()) {
            emit trackActivated(tracks.first());
            if (tracks.size() > 1) {
                QVector<Track> remaining;
                for (qsizetype i = 1; i < tracks.size(); ++i) {
                    remaining.push_back(tracks.at(i));
                }
                emit addToQueueRequested(remaining);
            }
        } else if (selected == playNext) {
            emit playNextRequested(tracks);
        } else if (selected == addQueue) {
            emit addToQueueRequested(tracks);
        } else if (selected == scan) {
            emit importDirectoryRequested(path);
        }
        return;
    }

    const QVector<Track> tracks = selectedTracks();
    if (tracks.isEmpty()) {
        return;
    }
    QAction *play = menu.addAction(QStringLiteral("Play"));
    QAction *playNext = menu.addAction(QStringLiteral("Play next"));
    QAction *addQueue = menu.addAction(QStringLiteral("Add to queue"));
    QAction *findFile = menu.addAction(QStringLiteral("Find file"));
    const QAction *selected = menu.exec(m_tree->viewport()->mapToGlobal(pos));
    if (selected == play) {
        emit trackActivated(tracks.first());
    } else if (selected == playNext) {
        emit playNextRequested(tracks);
    } else if (selected == addQueue) {
        emit addToQueueRequested(tracks);
    } else if (selected == findFile) {
        emit findFileRequested(tracks.first());
    }
}

void FileExplorerView::navigateUp()
{
    if (m_currentDirectory.isEmpty()) {
        return;
    }
    const QString parent = QFileInfo(m_currentDirectory).absoluteDir().absolutePath();
    if (parent == m_currentDirectory) {
        return;
    }
    emit directoryRequested(parent);
}

void FileExplorerView::addDirectoryItem(const QString &path)
{
    auto *item = new QTreeWidgetItem(m_tree);
    item->setText(0, QFileInfo(path).fileName().isEmpty() ? path : QFileInfo(path).fileName());
    item->setData(0, TypeRole, DirectoryItem);
    item->setData(0, PathRole, cleanPath(path));
}

void FileExplorerView::addTrackItem(const Track &track)
{
    auto *item = new QTreeWidgetItem(m_tree);
    item->setText(0, track.title.trimmed().isEmpty() ? track.filename : track.title);
    item->setText(1, track.artistName);
    item->setText(2, track.albumTitle);
    item->setText(3, formatDuration(track.durationMs));
    item->setData(0, TypeRole, TrackItem);
    item->setData(0, PathRole, track.path);
    item->setData(0, TrackRole, QVariant::fromValue(track));
}

Track FileExplorerView::trackForFile(const QString &path) const
{
    Track track = TagReader().read(path);
    if (track.filename.isEmpty()) {
        track.filename = QFileInfo(path).fileName();
    }
    if (track.title.isEmpty()) {
        track.title = QFileInfo(path).completeBaseName();
    }
    return track;
}

QVector<Track> FileExplorerView::tracksForDirectory(const QString &path) const
{
    QVector<Track> tracks;
    if (m_mode == FileExplorerMode::Library) {
        for (int row = 0; row < m_tree->topLevelItemCount(); ++row) {
            QTreeWidgetItem *item = m_tree->topLevelItem(row);
            if (item != nullptr && item->data(0, TypeRole).toInt() == TrackItem) {
                const Track track = item->data(0, TrackRole).value<Track>();
                if (track.parentDir == path) {
                    tracks.push_back(track);
                }
            }
        }
        return tracks;
    }

    QDirIterator iterator(path, QDir::Files | QDir::Readable | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString file = iterator.next();
        const QFileInfo info(file);
        if (!info.isSymLink() && LibraryScanner::isSupportedAudioFile(file)) {
            tracks.push_back(trackForFile(file));
        }
    }
    return tracks;
}

QVector<Track> FileExplorerView::selectedTracks() const
{
    QVector<Track> tracks;
    for (QTreeWidgetItem *item : m_tree->selectedItems()) {
        if (item != nullptr && item->data(0, TypeRole).toInt() == TrackItem) {
            const Track track = item->data(0, TrackRole).value<Track>();
            if (!track.path.isEmpty()) {
                tracks.push_back(track);
            }
        }
    }
    return tracks;
}
