#include "ui/FileExplorerView.h"

#include "core/HumanQuantity.h"
#include "core/MusicSort.h"
#include "scanner/LibraryScanner.h"
#include "scanner/TagReader.h"
#include "ui/IdleReleaseController.h"
#include "ui/OverlayScrollBar.h"
#include "ui/PanelSearchBar.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QEvent>
#include <QFileInfo>
#include <QFont>
#include <QHash>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPalette>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QShowEvent>
#include <QStyle>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVariant>
#include <QWheelEvent>

#include <algorithm>

namespace {

enum ItemType {
    DirectoryItem = 1, // start at 1 so an unset TypeRole (toInt() == 0) is never a valid type
    TrackItem,
    UnsupportedItem,
};

enum ItemRoles {
    TypeRole = Qt::UserRole,
    PathRole,
    TrackRole,
};

enum Column {
    NameColumn = 0,
    ArtistColumn,
    AlbumColumn,
    DurationColumn,
    RatingColumn,
    SizeColumn,
};

QString formatSize(qint64 bytes)
{
    if (bytes <= 0) {
        return QString();
    }
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 ? QStringLiteral("%1 B").arg(bytes)
                     : QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(QLatin1String(units[unit]));
}

QString ratingStars(int rating0To100)
{
    if (rating0To100 <= 0) {
        return QString();
    }
    const int full = rating0To100 / 20;
    const bool half = (rating0To100 % 20) >= 10;
    QString stars(full, QChar(0x2605)); // ★
    if (half) {
        stars += QChar(0x00BD); // ½
    }
    return stars;
}

int displayRating(const Track &track)
{
    return track.effectiveRating0To100 >= 0 ? track.effectiveRating0To100 : track.rating0To100;
}

QString cleanPath(const QString &path)
{
    const QString cleaned = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (cleaned.size() > 1 && cleaned.endsWith(QLatin1Char('/'))) {
        return cleaned.left(cleaned.size() - 1);
    }
    return cleaned;
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
    // Bold via the font (not a stylesheet): a stylesheet pins the widget's
    // resolved palette, so the text colour would ignore live theme changes.
    QFont modeTitleFont = m_modeTitle->font();
    modeTitleFont.setBold(true);
    m_modeTitle->setFont(modeTitleFont);
    m_modeTitle->setContentsMargins(0, 0, 12, 0);
    auto *up = new QPushButton(QStringLiteral("Up"), bar);
    up->setFixedHeight(24);
    m_pathLabel = new QLabel(bar);
    m_pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    barLayout->addWidget(m_modeTitle);
    barLayout->addWidget(up);
    barLayout->addWidget(m_pathLabel, 1);
    layout->addWidget(bar);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(6);
    m_tree->setHeaderLabels({
        QStringLiteral("Name"),
        QStringLiteral("Artist"),
        QStringLiteral("Album"),
        QStringLiteral("Duration"),
        QStringLiteral("Rating"),
        QStringLiteral("Size"),
    });
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_tree->setRootIsDecorated(false);
    m_tree->setIndentation(0);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    applyRowHeight();
    layout->addWidget(m_tree, 1);

    m_search = new PanelSearchBar(this);
    PanelSearchBar::Providers searchProviders;
    searchProviders.documents = [this] { return searchDocuments(); };
    searchProviders.rowCount = [this] { return m_tree->topLevelItemCount(); };
    searchProviders.currentRow = [this] { return currentTopLevelRow(); };
    searchProviders.setCurrentRow = [this](int row) { selectTopLevelRow(row); };
    searchProviders.focusList = [this] { m_tree->setFocus(); };
    m_search->setProviders(searchProviders);
    layout->addWidget(m_search);

    m_hintBar = new QWidget(this);
    m_hintBar->setAutoFillBackground(true);
    applyHintBarPalette();
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

    // Drives lazy, non-blocking metadata reads for free-roam files not already
    // known to the library (one file per event-loop tick).
    m_metadataTimer = new QTimer(this);
    m_metadataTimer->setInterval(0);
    connect(m_metadataTimer, &QTimer::timeout, this, &FileExplorerView::processNextMetadata);

    m_tree->installEventFilter(this);
    m_tree->viewport()->installEventFilter(this); // Ctrl+wheel row-height resize
    OverlayScrollBar::install(m_tree);
    // Route focus to the tree so its key-binding eventFilter receives key
    // presses as soon as the explorer is shown, without a prior click.
    setFocusProxy(m_tree);

    setKeyBindingProfileName(QStringLiteral("vim"));

    connect(up, &QPushButton::clicked, this, &FileExplorerView::navigateUp);
    connect(m_tree, &QTreeWidget::itemActivated, this, &FileExplorerView::activateItem);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &FileExplorerView::showContextMenu);
    // Remember the cursor position per directory so returning to a directory
    // restores its previous selection instead of resetting to the top.
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *current, QTreeWidgetItem *) {
        if (m_restoringSelection || current == nullptr) {
            return;
        }
        m_lastSelectedByDir.insert(m_currentDirectory, current->data(0, PathRole).toString());
    });

    // Drop the populated tree once the explorer has been hidden for a while; the
    // owning view repopulates it (setLibraryEntries / setRootPath) on the next
    // navigation, and per-directory selection memory survives the release.
    new IdleReleaseController(this, [this] { releaseIdleResources(); });
}

void FileExplorerView::releaseIdleResources()
{
    m_metadataTimer->stop();
    m_pendingMetadata.clear();
    m_tree->clear();
}

QVector<Search::MatchDocument> FileExplorerView::searchDocuments() const
{
    QVector<Search::MatchDocument> docs;
    const int count = m_tree->topLevelItemCount();
    docs.reserve(count);
    for (int row = 0; row < count; ++row) {
        QTreeWidgetItem *item = m_tree->topLevelItem(row);
        if (item == nullptr) {
            continue;
        }
        const QString name = item->text(0);
        const QString path = item->data(0, PathRole).toString();
        QVector<Search::MatchField> fields;
        if (item->data(0, TypeRole).toInt() == TrackItem) {
            const Track track = item->data(0, TrackRole).value<Track>();
            fields = {
                Search::makeField(Search::MatchFieldRole::Filename, name, 300),
                Search::makeField(Search::MatchFieldRole::Artist, track.artistName, 200),
                Search::makeField(Search::MatchFieldRole::AlbumArtist, track.albumArtistName, 200),
                Search::makeField(Search::MatchFieldRole::Album, track.albumTitle, 150),
                Search::makeField(Search::MatchFieldRole::Path, path, 60),
            };
        } else {
            fields = {
                Search::makeField(Search::MatchFieldRole::Filename, name, 300),
                Search::makeField(Search::MatchFieldRole::Path, path, 60),
            };
        }
        docs.push_back({row, fields, {}});
    }
    return docs;
}

int FileExplorerView::currentTopLevelRow() const
{
    QTreeWidgetItem *current = m_tree->currentItem();
    return current != nullptr ? m_tree->indexOfTopLevelItem(current) : 0;
}

void FileExplorerView::selectTopLevelRow(int row)
{
    if (row < 0 || row >= m_tree->topLevelItemCount()) {
        return;
    }
    QTreeWidgetItem *item = m_tree->topLevelItem(row);
    m_tree->setCurrentItem(item);
    m_tree->scrollToItem(item);
}

void FileExplorerView::setMode(FileExplorerMode mode)
{
    if (m_mode == mode) {
        return;
    }
    m_mode = mode;
    m_metadataTimer->stop();
    m_pendingMetadata.clear();
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

void FileExplorerView::revealFile(const QString &filePath)
{
    const QString cleaned = cleanPath(filePath);
    if (cleaned.isEmpty()) {
        return;
    }
    const QString dir = cleanPath(QFileInfo(cleaned).absolutePath());
    if (dir.isEmpty()) {
        return;
    }

    // Remember the wanted selection so it is applied once the directory's listing
    // is (re)populated; restoreSelectionForCurrentDirectory() consumes it.
    m_lastSelectedByDir.insert(dir, cleaned);

    if (cleanPath(m_currentDirectory) == dir) {
        restoreSelectionForCurrentDirectory();
    } else {
        // MainWindow owns navigation (library entries come from the DB, free-roam
        // from the filesystem); the listing's repopulation restores the selection.
        emit directoryRequested(dir);
    }
}

void FileExplorerView::setRowHeight(int height)
{
    const int clamped = std::clamp(height, 16, 40);
    if (m_rowHeight == clamped) {
        return;
    }
    m_rowHeight = clamped;
    applyRowHeight();
    emit rowHeightChanged(m_rowHeight);
}

int FileExplorerView::rowHeight() const
{
    return m_rowHeight;
}

void FileExplorerView::applyRowHeightToItem(QTreeWidgetItem *item) const
{
    if (item != nullptr) {
        item->setSizeHint(0, QSize(0, m_rowHeight));
    }
}

void FileExplorerView::applyRowHeight()
{
    const int icon = std::clamp(m_rowHeight - 4, 12, 24);
    m_tree->setIconSize(QSize(icon, icon));
    for (int row = 0; row < m_tree->topLevelItemCount(); ++row) {
        applyRowHeightToItem(m_tree->topLevelItem(row));
    }
}

void FileExplorerView::restoreSelectionForCurrentDirectory()
{
    if (m_tree->topLevelItemCount() == 0) {
        return;
    }
    m_restoringSelection = true;
    const QString wanted = m_lastSelectedByDir.value(m_currentDirectory);
    QTreeWidgetItem *target = nullptr;
    if (!wanted.isEmpty()) {
        for (int row = 0; row < m_tree->topLevelItemCount(); ++row) {
            QTreeWidgetItem *item = m_tree->topLevelItem(row);
            if (item != nullptr && item->data(0, PathRole).toString() == wanted) {
                target = item;
                break;
            }
        }
    }
    if (target == nullptr) {
        target = m_tree->topLevelItem(0);
    }
    m_tree->setCurrentItem(target);
    m_tree->scrollToItem(target, QAbstractItemView::PositionAtCenter);
    m_restoringSelection = false;
}

void FileExplorerView::setLibraryEntries(const QStringList &directories, const QVector<Track> &tracks)
{
    m_metadataTimer->stop();
    m_pendingMetadata.clear();
    m_tree->clear();
    m_pathLabel->setText(m_currentDirectory.isEmpty() ? QStringLiteral("Library") : m_currentDirectory);
    for (const QString &directory : directories) {
        addDirectoryItem(directory);
    }
    for (const Track &track : tracks) {
        addTrackItem(track);
    }
    sortTopLevelItems();
    restoreSelectionForCurrentDirectory();
    // Force a full repaint; without this the view can keep showing the prior
    // listing until each row is hovered.
    m_tree->viewport()->update();
}

void FileExplorerView::refreshFreeRoam()
{
    m_metadataTimer->stop();
    m_pendingMetadata.clear();
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
            // Reuse already-scanned metadata/ratings when known; otherwise show
            // the row immediately and read its tags lazily off the UI hot path.
            const Track known = m_trackResolver ? m_trackResolver(cleanPath(entry.absoluteFilePath())) : Track();
            if (!known.path.isEmpty()) {
                addTrackItem(known);
            } else {
                addPendingTrackItem(entry);
            }
        } else if (m_showUnsupported) {
            // Listing only: unsupported (incl. extension-less) files are shown
            // but never read or played.
            addUnsupportedItem(entry);
        }
    }

    if (!m_pendingMetadata.isEmpty()) {
        m_metadataTimer->start();
    }
    sortTopLevelItems();
    restoreSelectionForCurrentDirectory();
    // Force a full repaint; without this the view can keep showing the prior
    // listing until each row is hovered.
    m_tree->viewport()->update();
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
        QAction *setStart = nullptr;
        if (m_mode == FileExplorerMode::FreeRoam && !m_currentDirectory.isEmpty()) {
            setStart = menu.addAction(QStringLiteral("Set current folder as start directory (b to jump)"));
            menu.addSeparator();
        }
        buildSortMenu(&menu);
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
        if (setStart != nullptr && selected == setStart) {
            setStartDirectory(m_currentDirectory);
        }
        return;
    }

    QMenu menu(this);
    const int type = item->data(0, TypeRole).toInt();
    if (type == DirectoryItem) {
        const QString path = item->data(0, PathRole).toString();
        QAction *playAll = menu.addAction(QStringLiteral("Play all tracks"));
        QAction *playNext = menu.addAction(QStringLiteral("Play next"));
        QAction *addQueue = menu.addAction(QStringLiteral("Add to queue"));
        QAction *playNextTemp = m_queueIsPlaylistSourced
            ? menu.addAction(QStringLiteral("Play next (don't save to playlist)")) : nullptr;
        QAction *addQueueTemp = m_queueIsPlaylistSourced
            ? menu.addAction(QStringLiteral("Add to queue (don't save to playlist)")) : nullptr;
        QAction *scan = menu.addAction(QStringLiteral("Scan/Add this directory to library"));
        QAction *setStart = nullptr;
        if (m_mode == FileExplorerMode::FreeRoam) {
            setStart = menu.addAction(QStringLiteral("Set as start directory (b to jump)"));
        }
        menu.addSeparator();
        buildSortMenu(&menu);
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
        } else if (playNextTemp != nullptr && selected == playNextTemp) {
            emit playNextTemporaryRequested(tracks);
        } else if (addQueueTemp != nullptr && selected == addQueueTemp) {
            emit addToQueueTemporaryRequested(tracks);
        } else if (selected == scan) {
            emit importDirectoryRequested(path);
        } else if (setStart != nullptr && selected == setStart) {
            setStartDirectory(path);
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
    QAction *playNextTemp = m_queueIsPlaylistSourced
        ? menu.addAction(QStringLiteral("Play next (don't save to playlist)")) : nullptr;
    QAction *addQueueTemp = m_queueIsPlaylistSourced
        ? menu.addAction(QStringLiteral("Add to queue (don't save to playlist)")) : nullptr;
    QAction *addPlaylist = menu.addAction(QStringLiteral("Add to playlist…"));
    QAction *findFile = menu.addAction(QStringLiteral("Open containing directory"));
    QAction *properties = menu.addAction(QStringLiteral("Properties"));

    QMenu *ratingMenu = menu.addMenu(QStringLiteral("Rating"));
    QHash<QAction *, int> ratingActions;
    QAction *clearRating = ratingMenu->addAction(QStringLiteral("Clear rating"));
    ratingActions.insert(clearRating, -1);
    for (int rating = 10; rating <= 100; rating += 10) {
        QAction *action = ratingMenu->addAction(ratingStars(rating));
        ratingActions.insert(action, rating);
    }

    menu.addSeparator();
    buildSortMenu(&menu);
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
    QAction *selected = menu.exec(m_tree->viewport()->mapToGlobal(pos));
    if (selected == play) {
        emit trackActivated(tracks.first());
    } else if (selected == playNext) {
        emit playNextRequested(tracks);
    } else if (selected == addQueue) {
        emit addToQueueRequested(tracks);
    } else if (playNextTemp != nullptr && selected == playNextTemp) {
        emit playNextTemporaryRequested(tracks);
    } else if (addQueueTemp != nullptr && selected == addQueueTemp) {
        emit addToQueueTemporaryRequested(tracks);
    } else if (selected == addPlaylist) {
        emit addToPlaylistRequested(tracks);
    } else if (selected == findFile) {
        emit findFileRequested(tracks.first());
    } else if (selected == properties) {
        emit propertiesRequested(tracks.first());
    } else if (selected != nullptr && ratingActions.contains(selected)) {
        const int rating = ratingActions.value(selected);
        for (const Track &track : tracks) {
            emit trackRatingChangeRequested(track, rating);
        }
        // Reflect the new rating immediately on the selected rows.
        for (QTreeWidgetItem *selectedItem : m_tree->selectedItems()) {
            if (selectedItem != nullptr && selectedItem->data(0, TypeRole).toInt() == TrackItem) {
                Track updated = selectedItem->data(0, TrackRole).value<Track>();
                updated.effectiveRating0To100 = rating;
                updated.rating0To100 = rating;
                updated.hasUserRating = rating >= 0;
                selectedItem->setData(0, TrackRole, QVariant::fromValue(updated));
                selectedItem->setText(RatingColumn, ratingStars(rating < 0 ? 0 : rating));
            }
        }
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

void FileExplorerView::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::StyleChange) {
        applyHintBarPalette();
    }
}

void FileExplorerView::applyHintBarPalette()
{
    if (m_hintBar == nullptr) {
        return;
    }
    // Derive from the live (inherited) application palette each time so both the
    // darkened background and the inherited text colour stay current.
    QPalette hintPalette = palette();
    hintPalette.setColor(QPalette::Window, hintPalette.color(QPalette::Window).darker(108));
    m_hintBar->setPalette(hintPalette);
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

void FileExplorerView::sortTopLevelItems()
{
    const int count = m_tree->topLevelItemCount();
    if (count <= 1) {
        return;
    }

    QTreeWidgetItem *current = m_tree->currentItem();
    const QString currentPath = current != nullptr ? current->data(0, PathRole).toString() : QString();

    QList<QTreeWidgetItem *> items;
    items.reserve(count);
    while (m_tree->topLevelItemCount() > 0) {
        items.push_back(m_tree->takeTopLevelItem(0));
    }

    const auto dir = m_sortDescending ? MusicSort::SortDirection::Descending : MusicSort::SortDirection::Ascending;
    const auto cmp = MusicSort::makeComparator<Track>(m_sortField, dir, m_sortReverseGroups);
    std::stable_sort(items.begin(), items.end(), [&](QTreeWidgetItem *a, QTreeWidgetItem *b) {
        const bool aDir = a->data(0, TypeRole).toInt() == DirectoryItem;
        const bool bDir = b->data(0, TypeRole).toInt() == DirectoryItem;
        if (aDir != bDir) {
            return aDir; // directories always grouped first
        }
        if (aDir) {
            return QString::localeAwareCompare(a->text(NameColumn), b->text(NameColumn)) < 0;
        }
        return cmp(a->data(0, TrackRole).value<Track>(), b->data(0, TrackRole).value<Track>());
    });

    m_tree->addTopLevelItems(items);

    if (!currentPath.isEmpty()) {
        for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
            QTreeWidgetItem *it = m_tree->topLevelItem(i);
            if (it->data(0, PathRole).toString() == currentPath) {
                m_tree->setCurrentItem(it);
                break;
            }
        }
    }
}

void FileExplorerView::setSort(MusicSort::SortField field, bool descending, bool reverseGroups)
{
    if (m_sortField == field && m_sortDescending == descending && m_sortReverseGroups == reverseGroups) {
        return;
    }
    m_sortField = field;
    m_sortDescending = descending;
    m_sortReverseGroups = reverseGroups;
    sortTopLevelItems();
    emit sortChanged(MusicSort::sortFieldToString(m_sortField), m_sortDescending, m_sortReverseGroups);
}

void FileExplorerView::buildSortMenu(QMenu *parent)
{
    QMenu *sortMenu = parent->addMenu(QStringLiteral("Sort by"));
    const struct { const char *label; MusicSort::SortField field; } options[] = {
        {"Name", MusicSort::SortField::FileName},
        {"Year", MusicSort::SortField::Year},
        {"Album", MusicSort::SortField::AlbumTitle},
        {"Artist", MusicSort::SortField::Artist},
        {"Rating", MusicSort::SortField::Rating},
        {"Duration", MusicSort::SortField::Duration},
        {"Track number", MusicSort::SortField::TrackNumber},
    };
    for (const auto &opt : options) {
        QAction *action = sortMenu->addAction(QString::fromUtf8(opt.label));
        action->setCheckable(true);
        action->setChecked(m_sortField == opt.field);
        connect(action, &QAction::triggered, this, [this, field = opt.field]() {
            setSort(field, m_sortDescending, m_sortReverseGroups);
        });
    }
    sortMenu->addSeparator();
    QAction *desc = sortMenu->addAction(QStringLiteral("Descending"));
    desc->setCheckable(true);
    desc->setChecked(m_sortDescending);
    connect(desc, &QAction::triggered, this, [this](bool checked) {
        setSort(m_sortField, checked, checked ? m_sortReverseGroups : false);
    });
    QAction *reverseGroups = sortMenu->addAction(QStringLiteral("Reverse groups too"));
    reverseGroups->setCheckable(true);
    reverseGroups->setChecked(m_sortReverseGroups);
    reverseGroups->setEnabled(m_sortDescending);
    connect(reverseGroups, &QAction::triggered, this, [this](bool checked) {
        setSort(m_sortField, m_sortDescending, checked);
    });
}

void FileExplorerView::addDirectoryItem(const QString &path)
{
    auto *item = new QTreeWidgetItem(m_tree);
    item->setText(0, QFileInfo(path).fileName().isEmpty() ? path : QFileInfo(path).fileName());
    item->setIcon(0, QIcon::fromTheme(QStringLiteral("folder"), style()->standardIcon(QStyle::SP_DirIcon)));
    item->setData(0, TypeRole, DirectoryItem);
    item->setData(0, PathRole, cleanPath(path));
    applyRowHeightToItem(item);
}

void FileExplorerView::applyTrackToItem(QTreeWidgetItem *item, const Track &track)
{
    item->setText(NameColumn, track.title.trimmed().isEmpty() ? track.filename : track.title);
    item->setText(ArtistColumn, track.artistName);
    item->setText(AlbumColumn, track.albumTitle);
    item->setText(DurationColumn, humanquantity::formatDuration(track.durationMs));
    item->setText(RatingColumn, ratingStars(displayRating(track)));
    item->setText(SizeColumn, formatSize(track.fileSize));
    item->setData(0, TrackRole, QVariant::fromValue(track));
}

void FileExplorerView::addTrackItem(const Track &track)
{
    auto *item = new QTreeWidgetItem(m_tree);
    item->setIcon(0, QIcon::fromTheme(QStringLiteral("audio-x-generic"), style()->standardIcon(QStyle::SP_MediaPlay)));
    item->setData(0, TypeRole, TrackItem);
    item->setData(0, PathRole, track.path);
    applyTrackToItem(item, track);
    applyRowHeightToItem(item);
}

void FileExplorerView::addPendingTrackItem(const QFileInfo &info)
{
    auto *item = new QTreeWidgetItem(m_tree);
    item->setText(NameColumn, info.completeBaseName());
    item->setText(SizeColumn, formatSize(info.size()));
    item->setIcon(0, QIcon::fromTheme(QStringLiteral("audio-x-generic"), style()->standardIcon(QStyle::SP_MediaPlay)));
    item->setData(0, TypeRole, TrackItem);
    item->setData(0, PathRole, cleanPath(info.absoluteFilePath()));
    Track placeholder;
    placeholder.path = cleanPath(info.absoluteFilePath());
    placeholder.filename = info.fileName();
    placeholder.fileSize = info.size();
    item->setData(0, TrackRole, QVariant::fromValue(placeholder));
    applyRowHeightToItem(item);
    m_pendingMetadata.push_back(item);
}

void FileExplorerView::processNextMetadata()
{
    if (m_pendingMetadata.isEmpty()) {
        m_metadataTimer->stop();
        return;
    }
    QTreeWidgetItem *item = m_pendingMetadata.takeFirst();
    if (item == nullptr) {
        return;
    }
    const QString path = item->data(0, PathRole).toString();
    applyTrackToItem(item, trackForFile(path));
    if (m_pendingMetadata.isEmpty()) {
        m_metadataTimer->stop();
        // Tags are now loaded; settle the listing under the active sort.
        sortTopLevelItems();
    }
}

void FileExplorerView::addUnsupportedItem(const QFileInfo &info)
{
    auto *item = new QTreeWidgetItem(m_tree);
    item->setText(NameColumn, info.fileName());
    item->setText(SizeColumn, formatSize(info.size()));
    item->setIcon(0, QIcon::fromTheme(QStringLiteral("text-x-generic"), style()->standardIcon(QStyle::SP_FileIcon)));
    item->setData(0, TypeRole, UnsupportedItem);
    item->setData(0, PathRole, cleanPath(info.absoluteFilePath()));
    Track placeholder;
    placeholder.path = cleanPath(info.absoluteFilePath());
    placeholder.filename = info.fileName();
    placeholder.fileSize = info.size();
    item->setData(0, TrackRole, QVariant::fromValue(placeholder));
    item->setForeground(NameColumn, palette().brush(QPalette::Disabled, QPalette::Text));
    applyRowHeightToItem(item);
}

void FileExplorerView::setTrackResolver(std::function<Track(const QString &)> resolver)
{
    m_trackResolver = std::move(resolver);
}

void FileExplorerView::setStartDirectory(const QString &path)
{
    const QString cleaned = path.isEmpty() ? QString() : cleanPath(path);
    if (m_startDirectory == cleaned) {
        return;
    }
    m_startDirectory = cleaned;
    emit startDirectoryChanged(m_startDirectory);
}

QString FileExplorerView::startDirectory() const
{
    return m_startDirectory;
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
    if (m_search != nullptr) {
        m_search->setLabel(title);
    }
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
    if (watched == m_tree->viewport() && event->type() == QEvent::Wheel) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        if (wheel->modifiers() & Qt::ControlModifier) {
            setRowHeight(m_rowHeight + (wheel->angleDelta().y() > 0 ? 2 : -2));
            wheel->accept();
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

    if (watched != m_tree || event->type() != QEvent::KeyPress) {
        return QWidget::eventFilter(watched, event);
    }

    auto *keyEvent = static_cast<QKeyEvent *>(event);
    const int key = keyEvent->key();
    const auto modifiers = keyEvent->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier | Qt::ShiftModifier);

    // "/" search, mirroring the main view: open, M-n/M-p cycle confirmed matches,
    // Esc clears the constraint. The bar handles its own edit keys internally.
    if (m_search != nullptr) {
        if (!modifiers && key == Qt::Key_Slash) {
            m_search->open();
            return true;
        }
        if (modifiers == Qt::AltModifier && key == Qt::Key_N) {
            m_search->cycle(+1);
            return true;
        }
        if (modifiers == Qt::AltModifier && key == Qt::Key_P) {
            m_search->cycle(-1);
            return true;
        }
        if (key == Qt::Key_Escape && (m_search->isSearchVisible() || m_search->hasActiveQuery())) {
            m_search->escape();
            return true;
        }
    }

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
            const int rowHeight = std::max(1, m_tree->sizeHintForRow(0));
            const int visible = std::max(1, m_tree->viewport()->height() / rowHeight);
            const int target = std::min(m_tree->topLevelItemCount() - 1,
                                        m_tree->indexOfTopLevelItem(item) + visible);
            m_tree->setCurrentItem(m_tree->topLevelItem(target));
            m_tree->scrollToItem(m_tree->topLevelItem(target), center);
        }

    } else if (action == PageUp) {
        if (item != nullptr) {
            const int rowHeight = std::max(1, m_tree->sizeHintForRow(0));
            const int visible = std::max(1, m_tree->viewport()->height() / rowHeight);
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

    } else if (action == AddToPlaylist) {
        const QVector<Track> tracks = selectedTracks();
        if (!tracks.isEmpty()) {
            emit addToPlaylistRequested(tracks);
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

    } else if (action == GoToStart) {
        if (m_mode == FileExplorerMode::FreeRoam && !m_startDirectory.isEmpty()) {
            emit directoryRequested(m_startDirectory);
        }

    } else if (action == Escape) {
        if (!m_currentDirectory.isEmpty()) {
            navigateUp();
        }
    }
}
