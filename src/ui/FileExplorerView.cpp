#include "ui/FileExplorerView.h"

#include "scanner/LibraryScanner.h"
#include "scanner/TagReader.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QEvent>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QShowEvent>
#include <QStyle>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVariant>

namespace {

enum ItemType {
    DirectoryItem = 1,
    TrackItem = 2,
    UnsupportedItem = 3,
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
    m_modeTitle = new QLabel(bar);
    m_modeTitle->setStyleSheet(QStringLiteral("font-weight: bold; padding-right: 12px;"));
    auto *up = new QPushButton(QStringLiteral("Up"), bar);
    up->setFixedHeight(24);
    m_pathLabel = new QLabel(bar);
    m_pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    barLayout->addWidget(m_modeTitle);
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
    m_tree->setIndentation(0);
    m_tree->setIconSize(QSize(22, 22));
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_tree, 1);

    m_hintBar = new QWidget(this);
    m_hintBar->setAutoFillBackground(true);
    QPalette hintPalette = m_hintBar->palette();
    hintPalette.setColor(QPalette::Window, hintPalette.color(QPalette::Window).darker(108));
    m_hintBar->setPalette(hintPalette);
    auto *hintLayout = new QHBoxLayout(m_hintBar);
    hintLayout->setContentsMargins(8, 1, 8, 1);
    m_hintLabel = new QLabel(m_hintBar);
    hintLayout->addWidget(m_hintLabel, 1);
    auto *hintDismiss = new QPushButton(QStringLiteral("\u2715"), m_hintBar);
    hintDismiss->setFixedSize(20, 20);
    hintDismiss->setFlat(true);
    hintDismiss->setToolTip(QStringLiteral("Hide key hints"));
    connect(hintDismiss, &QPushButton::clicked, this, [this]() {
        setKeyHintBarVisible(false);
    });
    hintLayout->addWidget(hintDismiss);
    layout->addWidget(m_hintBar);
    m_hintBar->setVisible(false);

    m_ggTimer = new QTimer(this);
    m_ggTimer->setInterval(500);
    m_ggTimer->setSingleShot(true);
    connect(m_ggTimer, &QTimer::timeout, this, [this]() {
        m_pendingG = false;
    });

    m_tree->installEventFilter(this);
    // Route focus to the tree so its key-binding eventFilter receives key
    // presses as soon as the explorer is shown, without a prior click.
    setFocusProxy(m_tree);

    setKeyBindingProfileName(QStringLiteral("vim"));

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
        } else if (m_showUnsupported) {
            // Listing only: unsupported (incl. extension-less) files are shown
            // but never read or played.
            addUnsupportedItem(entry);
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
        QMenu menu(this);
        QMenu *keyMenu = menu.addMenu(QStringLiteral("Key bindings"));
        for (const KeyBindingProfile &profile : defaultKeyBindingProfiles()) {
            QAction *action = keyMenu->addAction(profile.label);
            action->setCheckable(true);
            action->setChecked(m_keyBindingProfileName == profile.name);
            connect(action, &QAction::triggered, this, [this, name = profile.name]() {
                setKeyBindingProfileName(name);
            });
        }
        keyMenu->addSeparator();
        QAction *hintToggle = keyMenu->addAction(QStringLiteral("Show key hints"));
        hintToggle->setCheckable(true);
        hintToggle->setChecked(m_hintBar->isVisible());
        connect(hintToggle, &QAction::toggled, this, &FileExplorerView::setKeyHintBarVisible);
        menu.exec(m_tree->viewport()->mapToGlobal(pos));
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
        menu.addSeparator();
        QMenu *keyMenu = menu.addMenu(QStringLiteral("Key bindings"));
        for (const KeyBindingProfile &profile : defaultKeyBindingProfiles()) {
            QAction *action = keyMenu->addAction(profile.label);
            action->setCheckable(true);
            action->setChecked(m_keyBindingProfileName == profile.name);
            connect(action, &QAction::triggered, this, [this, name = profile.name]() {
                setKeyBindingProfileName(name);
            });
        }
        keyMenu->addSeparator();
        QAction *hintToggle = keyMenu->addAction(QStringLiteral("Show key hints"));
        hintToggle->setCheckable(true);
        hintToggle->setChecked(m_hintBar->isVisible());
        connect(hintToggle, &QAction::toggled, this, &FileExplorerView::setKeyHintBarVisible);
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
    menu.addSeparator();
    QMenu *keyMenu = menu.addMenu(QStringLiteral("Key bindings"));
    for (const KeyBindingProfile &profile : defaultKeyBindingProfiles()) {
        QAction *action = keyMenu->addAction(profile.label);
        action->setCheckable(true);
        action->setChecked(m_keyBindingProfileName == profile.name);
        connect(action, &QAction::triggered, this, [this, name = profile.name]() {
            setKeyBindingProfileName(name);
        });
    }
    keyMenu->addSeparator();
    QAction *hintToggle = keyMenu->addAction(QStringLiteral("Show key hints"));
    hintToggle->setCheckable(true);
    hintToggle->setChecked(m_hintBar->isVisible());
    connect(hintToggle, &QAction::toggled, this, &FileExplorerView::setKeyHintBarVisible);
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

void FileExplorerView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // Give the tree keyboard focus whenever this explorer becomes visible, so
    // the configured key bindings work immediately (previously they only
    // activated after a click moved focus into the tree).
    m_tree->setFocus(Qt::OtherFocusReason);
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
    item->setIcon(0, QIcon::fromTheme(QStringLiteral("folder"), style()->standardIcon(QStyle::SP_DirIcon)));
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
    item->setIcon(0, QIcon::fromTheme(QStringLiteral("audio-x-generic"), style()->standardIcon(QStyle::SP_MediaPlay)));
    item->setData(0, TypeRole, TrackItem);
    item->setData(0, PathRole, track.path);
    item->setData(0, TrackRole, QVariant::fromValue(track));
}

void FileExplorerView::addUnsupportedItem(const QFileInfo &info)
{
    auto *item = new QTreeWidgetItem(m_tree);
    item->setText(0, info.fileName());
    item->setIcon(0, QIcon::fromTheme(QStringLiteral("text-x-generic"), style()->standardIcon(QStyle::SP_FileIcon)));
    item->setData(0, TypeRole, UnsupportedItem);
    item->setData(0, PathRole, cleanPath(info.absoluteFilePath()));
    item->setForeground(0, palette().brush(QPalette::Disabled, QPalette::Text));
}

void FileExplorerView::setShowUnsupportedFiles(bool show)
{
    if (m_showUnsupported == show) {
        return;
    }
    m_showUnsupported = show;
    if (m_mode == FileExplorerMode::FreeRoam) {
        refreshFreeRoam();
    }
}

bool FileExplorerView::showUnsupportedFiles() const
{
    return m_showUnsupported;
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

void FileExplorerView::setModeTitle(const QString &title)
{
    m_modeTitle->setText(title);
}

void FileExplorerView::setKeyBindingProfileName(const QString &name)
{
    if (m_keyBindingProfileName == name) {
        return;
    }
    m_keyBindingProfileName = name;
    m_keyBindings = bindingMapForProfile(name);
    updateHintBar();
    emit keyBindingProfileChanged(name);
}

QString FileExplorerView::keyBindingProfileName() const
{
    return m_keyBindingProfileName;
}

QStringList FileExplorerView::availableKeyBindingProfiles() const
{
    return ::availableProfileNames();
}

void FileExplorerView::setKeyHintBarVisible(bool visible)
{
    if (m_hintBar->isVisible() == visible) {
        return;
    }
    m_hintBar->setVisible(visible);
    emit keyHintVisibilityChanged(visible);
}

bool FileExplorerView::isKeyHintBarVisible() const
{
    return m_hintBar->isVisible();
}

void FileExplorerView::updateHintBar()
{
    for (const KeyBindingProfile &profile : defaultKeyBindingProfiles()) {
        if (profile.name == m_keyBindingProfileName) {
            m_hintLabel->setText(profile.hintText);
            return;
        }
    }
    m_hintLabel->clear();
}

bool FileExplorerView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_tree || event->type() != QEvent::KeyPress) {
        return QWidget::eventFilter(watched, event);
    }

    auto *keyEvent = static_cast<QKeyEvent *>(event);
    const int key = keyEvent->key();
    const auto modifiers = keyEvent->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier | Qt::ShiftModifier);

    if (!modifiers && key == Qt::Key_Backspace) {
        navigateUp();
        return true;
    }

    if (key == Qt::Key_Up || key == Qt::Key_Down || key == Qt::Key_Left || key == Qt::Key_Right ||
        key == Qt::Key_PageUp || key == Qt::Key_PageDown ||
        key == Qt::Key_Home || key == Qt::Key_End ||
        key == Qt::Key_Return || key == Qt::Key_Enter) {
        return false;
    }

    if (!modifiers && key == Qt::Key_G && m_keyBindings.contains(Qt::Key_G)) {
        if (m_pendingG) {
            m_pendingG = false;
            m_ggTimer->stop();
            applyKeyAction(ExplorerAction::ScrollToTop);
        } else {
            m_pendingG = true;
            m_ggTimer->start();
        }
        return true;
    }
    m_pendingG = false;

    QKeySequence ks(modifiers | key);

    if (m_keyBindings.contains(ks)) {
        applyKeyAction(m_keyBindings.value(ks));
        return true;
    }

    if (!modifiers && key >= Qt::Key_Space && key <= Qt::Key_AsciiTilde) {
        for (auto it = m_keyBindings.constBegin(); it != m_keyBindings.constEnd(); ++it) {
            if (it.key().count() == 1 && (it.key()[0].key() == static_cast<Qt::Key>(key))) {
                applyKeyAction(it.value());
                return true;
            }
        }
    }

    return false;
}

void FileExplorerView::applyKeyAction(const QString &action)
{
    using namespace ExplorerAction;

    auto *item = m_tree->currentItem();
    const QAbstractItemView::ScrollHint center = QAbstractItemView::PositionAtCenter;

    if (action == MoveDown) {
        if (item != nullptr) {
            const int row = m_tree->indexOfTopLevelItem(item);
            if (row + 1 < m_tree->topLevelItemCount()) {
                m_tree->setCurrentItem(m_tree->topLevelItem(row + 1));
                m_tree->scrollToItem(m_tree->topLevelItem(row + 1), center);
            }
        } else if (m_tree->topLevelItemCount() > 0) {
            m_tree->setCurrentItem(m_tree->topLevelItem(0));
        }

    } else if (action == MoveUp) {
        if (item != nullptr) {
            const int row = m_tree->indexOfTopLevelItem(item);
            if (row > 0) {
                m_tree->setCurrentItem(m_tree->topLevelItem(row - 1));
                m_tree->scrollToItem(m_tree->topLevelItem(row - 1), center);
            }
        }

    } else if (action == NavigateIn) {
        if (item != nullptr && item->data(0, TypeRole).toInt() == DirectoryItem) {
            emit directoryRequested(item->data(0, PathRole).toString());
        } else if (item != nullptr) {
            activateItem(item);
        }

    } else if (action == NavigateUp) {
        navigateUp();

    } else if (action == ScrollToTop) {
        if (m_tree->topLevelItemCount() > 0) {
            m_tree->setCurrentItem(m_tree->topLevelItem(0));
        }

    } else if (action == ScrollToBottom) {
        if (m_tree->topLevelItemCount() > 0) {
            const int last = m_tree->topLevelItemCount() - 1;
            m_tree->setCurrentItem(m_tree->topLevelItem(last));
            m_tree->scrollToItem(m_tree->topLevelItem(last), center);
        }

    } else if (action == PageDown) {
        if (item != nullptr) {
            const int visible = std::max(1, m_tree->viewport()->height() / m_tree->sizeHintForRow(0));
            const int target = std::min(m_tree->topLevelItemCount() - 1,
                                        m_tree->indexOfTopLevelItem(item) + visible);
            m_tree->setCurrentItem(m_tree->topLevelItem(target));
            m_tree->scrollToItem(m_tree->topLevelItem(target), center);
        }

    } else if (action == PageUp) {
        if (item != nullptr) {
            const int visible = std::max(1, m_tree->viewport()->height() / m_tree->sizeHintForRow(0));
            const int target = std::max(0, m_tree->indexOfTopLevelItem(item) - visible);
            m_tree->setCurrentItem(m_tree->topLevelItem(target));
            m_tree->scrollToItem(m_tree->topLevelItem(target), center);
        }

    } else if (action == PlaySelected) {
        if (item != nullptr && item->data(0, TypeRole).toInt() == TrackItem) {
            const Track track = item->data(0, TrackRole).value<Track>();
            if (!track.path.isEmpty()) {
                emit trackActivated(track);
            }
        }

    } else if (action == AddToQueue) {
        const QVector<Track> tracks = selectedTracks();
        if (!tracks.isEmpty()) {
            emit addToQueueRequested(tracks);
        }

    } else if (action == PlayNext) {
        const QVector<Track> tracks = selectedTracks();
        if (!tracks.isEmpty()) {
            emit playNextRequested(tracks);
        }

    } else if (action == ImportDirectory) {
        if (item != nullptr && item->data(0, TypeRole).toInt() == DirectoryItem) {
            emit importDirectoryRequested(item->data(0, PathRole).toString());
        }

    } else if (action == FindFile) {
        if (item != nullptr && item->data(0, TypeRole).toInt() == TrackItem) {
            const Track track = item->data(0, TrackRole).value<Track>();
            if (!track.path.isEmpty()) {
                emit findFileRequested(track);
            }
        }

    } else if (action == GoHome) {
        emit directoryRequested(QDir::homePath());

    } else if (action == Escape) {
        if (!m_currentDirectory.isEmpty()) {
            navigateUp();
        }
    }
}
