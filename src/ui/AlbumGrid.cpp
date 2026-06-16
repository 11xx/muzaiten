#include "ui/AlbumGrid.h"

#include "core/Album.h"
#include "core/MusicSort.h"
#include "scanner/ArtworkCache.h"
#include "ui/AlbumArtFallback.h"
#include "ui/AlbumGridDelegate.h"
#include "ui/OverlayScrollBar.h"
#include "ui/StarRating.h"

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QPixmap>
#include <QRubberBand>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QStyleHints>
#include <QTimer>
#include <QResizeEvent>
#include <QWheelEvent>
#include <algorithm>

namespace {
constexpr int albumBatchSize = 24;

enum Roles {
    AlbumTitleRole = Qt::UserRole,
    AlbumArtistRole = Qt::UserRole + 1,
    RatingRole = Qt::UserRole + 2,
    HoverRatingRole = Qt::UserRole + 3,
    SelectedRole = Qt::UserRole + 4,
    TextAlignmentRole = Qt::UserRole + 5,
    ArtSizeRole = Qt::UserRole + 6,
    CellPaddingRole = Qt::UserRole + 7,
    StarSizeRole = Qt::UserRole + 8,
    HasUserRatingRole = Qt::UserRole + 9,
    RepresentativeDirRole = Qt::UserRole + 10,
    ArtworkGenerationRole = Qt::UserRole + 11,
    LoadingRole = Qt::UserRole + 12,
    AlbumYearRole = Qt::UserRole + 13,
    RememberedOutlineRole = Qt::UserRole + 14,
};

QString alignmentToString(Qt::Alignment alignment)
{
    if (alignment & Qt::AlignLeft) {
        return QStringLiteral("left");
    }
    if (alignment & Qt::AlignRight) {
        return QStringLiteral("right");
    }
    return QStringLiteral("center");
}

Qt::Alignment alignmentFromString(const QString &value)
{
    if (value == QStringLiteral("left")) {
        return Qt::AlignLeft;
    }
    if (value == QStringLiteral("right")) {
        return Qt::AlignRight;
    }
    return Qt::AlignHCenter;
}

QRect alignedRatingCell(const QRect &anchorRect, const QRect &textRect, int starSize, Qt::Alignment alignment)
{
    const int starsWidth = starSize * 5;
    const int hitPadding = 6;
    int left = anchorRect.left() - hitPadding;
    if (alignment & Qt::AlignRight) {
        left = anchorRect.right() - starsWidth + 1 - hitPadding;
    } else if (alignment & Qt::AlignHCenter) {
        left = anchorRect.left() + ((anchorRect.width() - starsWidth) / 2) - hitPadding;
    }
    return {left, textRect.bottom() + 4, starsWidth + (hitPadding * 2), starSize};
}

int yearFromAlbum(const Album &album)
{
    for (const QString &candidate : {album.originalDate, album.date}) {
        bool ok = false;
        const int year = candidate.trimmed().left(4).toInt(&ok);
        if (ok) {
            return year;
        }
    }
    return 0;
}

} // namespace

AlbumGrid::AlbumGrid(QWidget *parent)
    : QListView(parent)
{
    setViewMode(QListView::IconMode);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setUniformItemSizes(true);
    setWordWrap(true);
    setTextElideMode(Qt::ElideRight);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    setContextMenuPolicy(Qt::CustomContextMenu);
    verticalScrollBar()->setSingleStep(36);
    horizontalScrollBar()->setSingleStep(36);
    applySettingsToView();
    setItemDelegate(new AlbumGridDelegate(this));
    m_populateTimer = new QTimer(this);
    m_populateTimer->setInterval(0);
    connect(m_populateTimer, &QTimer::timeout, this, &AlbumGrid::appendNextAlbumBatch);
    m_artworkTimer = new QTimer(this);
    m_artworkTimer->setInterval(0);
    connect(m_artworkTimer, &QTimer::timeout, this, &AlbumGrid::loadNextAlbumArtwork);
    m_spinnerTimer = new QTimer(this);
    m_spinnerTimer->setInterval(90);
    connect(m_spinnerTimer, &QTimer::timeout, this, [this]() {
        m_loadingAngle = (m_loadingAngle + 30) % 360;
        viewport()->update();
    });
    m_dragScrollTimer = new QTimer(this);
    m_dragScrollTimer->setInterval(40);
    connect(m_dragScrollTimer, &QTimer::timeout, this, &AlbumGrid::updateDragAutoscroll);

    auto *model = new QStandardItemModel(this);
    auto *item = new QStandardItem(QStringLiteral("No library loaded"));
    item->setEditable(false);
    model->appendRow(item);
    setModel(model);

    connect(this, &QListView::customContextMenuRequested, this, &AlbumGrid::showContextMenu);
    OverlayScrollBar::install(this);
}

void AlbumGrid::setArtworkCache(ArtworkCache *cache)
{
    if (m_artworkCache == cache) {
        return;
    }
    if (m_artworkCache != nullptr) {
        disconnect(m_artworkCache, nullptr, this, nullptr);
    }
    m_artworkCache = cache;
    if (m_artworkCache != nullptr) {
        connect(m_artworkCache, &ArtworkCache::artworkReady, this, &AlbumGrid::onArtworkReady);
        connect(m_artworkCache, &ArtworkCache::artworkMissing, this, &AlbumGrid::onArtworkMissing);
    }
}

void AlbumGrid::setAlbums(const QVector<Album> &albums, bool freshLoad)
{
    const QString previousCurrentTitle = currentAlbumTitle();

    m_populateTimer->stop();
    m_artworkTimer->stop();
    m_spinnerTimer->stop();
    ++m_artworkGeneration;

    // Store the unsorted source so applySort() can re-sort without re-querying.
    // Only update m_sourceAlbums when the caller provides a real album list (not
    // an internal re-sort call from applySort, which passes m_sourceAlbums itself).
    if (&albums != &m_sourceAlbums) {
        m_sourceAlbums = albums;
    }

    // Build the display order using the shared sort/grouping chains.
    QVector<Album> sorted = m_sourceAlbums;
    const auto dir = m_sortDescending ? MusicSort::SortDirection::Descending : MusicSort::SortDirection::Ascending;
    std::stable_sort(sorted.begin(), sorted.end(),
                     MusicSort::makeComparator<Album>(m_sortField, dir, m_sortReverseGroups));

    m_pendingAlbums = sorted;
    m_nextAlbumRow = 0;
    m_nextArtworkRow = 0;
    m_showLoading = freshLoad && m_artworkCache != nullptr;
    m_loadingCount = m_showLoading ? static_cast<int>(albums.size()) : 0;

    auto *itemModel = qobject_cast<QStandardItemModel *>(model());
    const int target = static_cast<int>(m_pendingAlbums.size());

    while (itemModel->rowCount() > target) {
        itemModel->removeRow(itemModel->rowCount() - 1);
    }

    // Recompute cell sizes for the new album count before populating so every
    // item (including batch-loaded ones) gets the correct SizeHintRole from the start.
    m_displayItemCount = target;
    applySettingsToView();

    const QIcon fallbackIcon(AlbumArtFallback::resourcePath(palette()));
    for (int i = 0; i < itemModel->rowCount(); ++i) {
        QStandardItem *item = itemModel->item(i);
        // Reset stale art when a reused row now shows a different album, so a
        // previous artist's cover never lingers as another album's art.
        const bool albumChanged = item->data(AlbumTitleRole) != m_pendingAlbums[i].title
            || item->data(AlbumArtistRole) != m_pendingAlbums[i].albumArtistName;
        populateItemFromAlbum(item, m_pendingAlbums[i]);
        if (albumChanged) {
            item->setIcon(fallbackIcon);
        }
    }
    m_nextAlbumRow = itemModel->rowCount();

    if (m_nextAlbumRow < target) {
        appendNextAlbumBatch();
    } else {
        m_pendingAlbums.clear();
        if (itemModel->rowCount() > 0 && m_artworkCache != nullptr && !m_artworkTimer->isActive()) {
            m_artworkTimer->start();
        }
    }

    if (m_showLoading && m_loadingCount > 0) {
        m_spinnerTimer->start();
    }

    if (itemModel->rowCount() > 0) {
        int targetRow = 0;
        const QString targetTitle = !m_selectedAlbumTitle.isEmpty() ? m_selectedAlbumTitle : previousCurrentTitle;
        if (!targetTitle.isEmpty()) {
            for (int row = 0; row < itemModel->rowCount(); ++row) {
                if (itemModel->index(row, 0).data(AlbumTitleRole).toString() == targetTitle) {
                    targetRow = row;
                    break;
                }
            }
        }
        setCurrentRowInternal(targetRow, false);
    }
    QSet<QString> availableTitles;
    for (int row = 0; row < itemModel->rowCount(); ++row) {
        const QString title = itemModel->index(row, 0).data(AlbumTitleRole).toString();
        if (!title.isEmpty()) {
            availableTitles.insert(title);
        }
    }
    for (auto it = m_markedAlbumTitles.begin(); it != m_markedAlbumTitles.end();) {
        if (!availableTitles.contains(*it)) {
            it = m_markedAlbumTitles.erase(it);
        } else {
            ++it;
        }
    }
    reselectMarkedAlbums();
}

void AlbumGrid::applySort()
{
    // Re-sort the stored source albums and repopulate. freshLoad=false: no spinner.
    setAlbums(m_sourceAlbums, false);
}

void AlbumGrid::populateItemFromAlbum(QStandardItem *item, const Album &album)
{
    // Show original year if available, otherwise fall back to release date.
    const QString displayYear = [&album]() -> QString {
        for (const QString &candidate : {album.originalDate, album.date}) {
            const QString y = candidate.trimmed().left(4);
            if (!y.isEmpty()) {
                return y;
            }
        }
        return {};
    }();
    QString label = album.title;
    if (!displayYear.isEmpty()) {
        label += QStringLiteral("\n%1").arg(displayYear);
    }
    item->setText(label);
    item->setData(album.title, AlbumTitleRole);
    item->setData(album.albumArtistName, AlbumArtistRole);
    item->setData(album.effectiveRating0To100, RatingRole);
    item->setData(StarRating::unset, HoverRatingRole);
    item->setData(!album.title.isEmpty() && album.title == m_selectedAlbumTitle, SelectedRole);
    item->setData(album.hasUserRating, HasUserRatingRole);
    item->setData(album.representativeDir, RepresentativeDirRole);
    item->setData(m_artworkGeneration, ArtworkGenerationRole);
    item->setData(m_showLoading, LoadingRole);
    item->setData(yearFromAlbum(album), AlbumYearRole);
    item->setData(false, RememberedOutlineRole);
    item->setData(m_effectiveArtSize, ArtSizeRole);
    item->setData(QSize(m_effectiveCellWidth, m_effectiveCellHeight), Qt::SizeHintRole);
    item->setToolTip(QStringLiteral("%1 tracks").arg(album.trackCount));
}

void AlbumGrid::appendNextAlbumBatch()
{
    auto *itemModel = qobject_cast<QStandardItemModel *>(model());
    if (itemModel == nullptr) {
        m_populateTimer->stop();
        return;
    }

    const QIcon fallbackIcon(AlbumArtFallback::resourcePath(palette()));
    const qsizetype end = std::min(m_nextAlbumRow + albumBatchSize, m_pendingAlbums.size());

    for (; m_nextAlbumRow < end; ++m_nextAlbumRow) {
        const Album &album = m_pendingAlbums.at(m_nextAlbumRow);

        auto *item = new QStandardItem();
        item->setEditable(false);
        populateItemFromAlbum(item, album);
        item->setData(static_cast<int>(m_textAlignment), TextAlignmentRole);
        item->setData(m_effectiveArtSize, ArtSizeRole);
        item->setData(m_padding, CellPaddingRole);
        item->setData(m_starSize, StarSizeRole);
        item->setData(QSize(m_effectiveCellWidth, m_effectiveCellHeight), Qt::SizeHintRole);
        item->setIcon(fallbackIcon);

        itemModel->appendRow(item);
    }

    if (m_nextAlbumRow < m_pendingAlbums.size()) {
        m_populateTimer->start();
    } else {
        m_populateTimer->stop();
        m_pendingAlbums.clear();
    }

    if (itemModel->rowCount() > 0 && m_artworkCache != nullptr && !m_artworkTimer->isActive()) {
        m_artworkTimer->start();
    }
}

void AlbumGrid::setSelectedAlbumTitle(const QString &albumTitle)
{
    m_selectedAlbumTitle = albumTitle;
    if (!m_selectedAlbumTitle.isEmpty()) {
        m_rememberedOutlineVisible = false;
    }
    applySettingsToItems();
}

void AlbumGrid::setRememberedOutlineVisible(bool visible)
{
    if (m_rememberedOutlineVisible == visible) {
        return;
    }
    m_rememberedOutlineVisible = visible;
    refreshRememberedOutline();
}

int AlbumGrid::rowCount() const
{
    return model() != nullptr ? model()->rowCount() : 0;
}

int AlbumGrid::currentRow() const
{
    const QModelIndex index = currentIndex();
    return index.isValid() ? index.row() : -1;
}

void AlbumGrid::setCurrentRow(int row)
{
    setCurrentRowInternal(row, true);
}

void AlbumGrid::setCurrentRowInternal(int row, bool clearRememberedOutline)
{
    if (model() == nullptr || model()->rowCount() == 0) {
        return;
    }
    if (clearRememberedOutline) {
        m_rememberedOutlineVisible = false;
    }
    const int safeRow = std::clamp(row, 0, model()->rowCount() - 1);
    if (clearRememberedOutline) {
        m_shiftAnchorRow = safeRow;
    }
    const QModelIndex index = model()->index(safeRow, 0);
    selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    setCurrentIndex(index);
    scrollTo(index, QAbstractItemView::EnsureVisible);
    reselectMarkedAlbums();
    refreshRememberedOutline();
}

void AlbumGrid::moveCurrentByGrid(int horizontal, int vertical)
{
    if (model() == nullptr || model()->rowCount() == 0) {
        return;
    }

    const int current = currentRow() >= 0 ? currentRow() : 0;
    const int columns = gridColumnCount();
    const int target = current + horizontal + (vertical * columns);
    if (m_marksFromMouse) {
        m_markedAlbumTitles.clear();
        m_marksFromMouse = false;
        emit albumNarrowFollowRequested({});
    }
    setCurrentRow(std::clamp(target, 0, model()->rowCount() - 1));
}

void AlbumGrid::activateCurrentAlbum()
{
    const QString title = currentAlbumTitle();
    if (!title.isEmpty()) {
        m_rememberedOutlineVisible = false;
        refreshRememberedOutline();
        emit albumSelectionToggled(title);
    }
}

void AlbumGrid::addCurrentAlbumToQueue()
{
    for (const QString &title : albumTitlesForAction()) {
        emit albumAddToQueueRequested(title);
    }
}

void AlbumGrid::addCurrentAlbumToPlaylist()
{
    emit albumAddToPlaylistRequested(albumTitlesForAction());
}

void AlbumGrid::playNextCurrentAlbum()
{
    for (const QString &title : albumTitlesForAction()) {
        emit albumPlayNextRequested(title);
    }
}

void AlbumGrid::markCurrentAlbum()
{
    setCurrentAlbumMarked(true);
}

void AlbumGrid::markAllAlbums()
{
    if (model() == nullptr) {
        return;
    }
    for (int row = 0; row < model()->rowCount(); ++row) {
        const QString title = model()->index(row, 0).data(AlbumTitleRole).toString();
        if (!title.isEmpty()) {
            m_markedAlbumTitles.insert(title);
        }
    }
    m_marksFromMouse = false;
    reselectMarkedAlbums();
    followNarrowToSelection();
}

void AlbumGrid::unmarkCurrentAlbum()
{
    setCurrentAlbumMarked(false);
}

void AlbumGrid::unmarkAllAlbums()
{
    m_markedAlbumTitles.clear();
    m_marksFromMouse = false;
    if (selectionModel() != nullptr) {
        selectionModel()->clearSelection();
    }
    if (currentIndex().isValid()) {
        setCurrentRow(currentIndex().row());
    }
    emit albumNarrowFollowRequested({});
}

QString AlbumGrid::currentAlbumTitle() const
{
    const QModelIndex index = currentIndex();
    return index.isValid() ? index.data(AlbumTitleRole).toString() : QString();
}

QStringList AlbumGrid::albumTitlesForAction() const
{
    if (model() == nullptr) {
        return {};
    }
    if (m_markedAlbumTitles.isEmpty()) {
        const QString title = currentAlbumTitle();
        return title.isEmpty() ? QStringList() : QStringList{title};
    }

    QStringList titles;
    for (int row = 0; row < model()->rowCount(); ++row) {
        const QString title = model()->index(row, 0).data(AlbumTitleRole).toString();
        if (!title.isEmpty() && m_markedAlbumTitles.contains(title)) {
            titles.push_back(title);
        }
    }
    return titles;
}

bool AlbumGrid::narrowingPersistsOnReturn() const
{
    return m_markedAlbumTitles.size() > 1 && !m_marksFromMouse;
}

QVector<Search::MatchDocument> AlbumGrid::searchDocuments() const
{
    QVector<Search::MatchDocument> docs;
    if (model() == nullptr) {
        return docs;
    }
    docs.reserve(model()->rowCount());
    for (int row = 0; row < model()->rowCount(); ++row) {
        const QModelIndex index = model()->index(row, 0);
        const QString title = index.data(AlbumTitleRole).toString();
        const QString artist = index.data(AlbumArtistRole).toString();
        const QString path = index.data(RepresentativeDirRole).toString();
        const QString free = QStringLiteral("%1 %2 %3").arg(title, artist, path);
        QVector<Search::MatchNumeric> numeric;
        const int year = index.data(AlbumYearRole).toInt();
        if (year > 0) {
            numeric.push_back({Search::TermKind::Year, year});
        }
        const int rating = index.data(RatingRole).toInt();
        if (rating >= 0) {
            numeric.push_back({Search::TermKind::Rating, rating});
        }
        docs.push_back({
            row,
            {
                Search::makeField(Search::MatchFieldRole::Title, title, 400),
                Search::makeField(Search::MatchFieldRole::Album, title, 200),
                Search::makeField(Search::MatchFieldRole::AlbumArtist, artist, 300),
                Search::makeField(Search::MatchFieldRole::Path, path, 60),
                Search::makeField(Search::MatchFieldRole::Free, free, 100),
            },
            numeric,
        });
    }
    return docs;
}

QString AlbumGrid::viewSettingsJson() const
{
    QJsonObject root;
    root.insert(QStringLiteral("cellWidth"), m_cellWidth);
    root.insert(QStringLiteral("cellHeight"), m_cellHeight);
    root.insert(QStringLiteral("artSize"), m_artSize);
    root.insert(QStringLiteral("spacing"), m_spacing);
    root.insert(QStringLiteral("textAlignment"), alignmentToString(m_textAlignment));
    root.insert(QStringLiteral("starSize"), m_starSize);
    root.insert(QStringLiteral("sortField"), MusicSort::sortFieldToString(m_sortField));
    root.insert(QStringLiteral("sortDescending"), m_sortDescending);
    root.insert(QStringLiteral("sortReverseGroups"), m_sortReverseGroups);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void AlbumGrid::applyViewSettingsJson(const QString &json)
{
    if (!json.isEmpty()) {
        const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
        m_cellWidth = std::clamp(root.value(QStringLiteral("cellWidth")).toInt(204), 160, 320);
        m_cellHeight = std::clamp(root.value(QStringLiteral("cellHeight")).toInt(292), 240, 400);
        m_artSize = std::clamp(root.value(QStringLiteral("artSize")).toInt(176), 96, 260);
        m_spacing = std::clamp(root.value(QStringLiteral("spacing")).toInt(6), 0, 24);
        m_starSize = std::clamp(root.value(QStringLiteral("starSize")).toInt(18), 18, 28);
        m_textAlignment = alignmentFromString(root.value(QStringLiteral("textAlignment")).toString(QStringLiteral("center")));
        m_sortField = MusicSort::sortFieldFromString(root.value(QStringLiteral("sortField")).toString(), MusicSort::SortField::Year);
        m_sortDescending = root.value(QStringLiteral("sortDescending")).toBool(true);
        m_sortReverseGroups = root.value(QStringLiteral("sortReverseGroups")).toBool(false);
    }
    applySettingsToView();
    applySettingsToItems();
}

void AlbumGrid::resetViewSettings()
{
    m_cellWidth = 204;
    m_cellHeight = 292;
    m_artSize = 176;
    m_spacing = 6;
    m_textAlignment = Qt::AlignHCenter;
    m_starSize = 18;
    m_sortField = MusicSort::SortField::Year;
    m_sortDescending = true;
    m_sortReverseGroups = false;
    applySettingsToView();
    applySettingsToItems();
    applySort();
    emit viewSettingsChanged();
}

void AlbumGrid::mouseMoveEvent(QMouseEvent *event)
{
    if (m_leftButtonPressed && !m_pressStartedOnRating) {
        m_dragCurrentPos = event->pos();
        if (!m_dragSelecting
            && (m_dragCurrentPos - m_dragStartPos).manhattanLength() >= QApplication::styleHints()->startDragDistance()) {
            m_dragSelecting = true;
            if (m_rubberBand == nullptr) {
                m_rubberBand = new QRubberBand(QRubberBand::Rectangle, viewport());
            }
            setRubberBandGeometry(currentDragRect());
            m_rubberBand->show();
        }
        if (m_dragSelecting) {
            setRubberBandGeometry(currentDragRect());
            updateRubberBandSelection();
            updateDragAutoscroll();
            event->accept();
            return;
        }
        event->accept();
        return;
    }
    if (m_leftButtonPressed) {
        event->accept();
        return;
    }

    const QModelIndex index = indexAt(event->pos());
    auto *itemModel = qobject_cast<QStandardItemModel *>(model());
    if (itemModel != nullptr) {
        for (int row = 0; row < itemModel->rowCount(); ++row) {
            itemModel->item(row)->setData(StarRating::unset, HoverRatingRole);
        }
    }

    if (index.isValid()) {
        const int hover = StarRating::ratingFromPosition(ratingRectForIndex(index), event->pos());
        if (hover >= 0) {
            model()->setData(index, hover, HoverRatingRole);
        }
    }
    QListView::mouseMoveEvent(event);
}

void AlbumGrid::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QListView::mousePressEvent(event);
        return;
    }

    // Record press state before knowing whether the pointer is over an item so
    // that a drag starting from empty space can still grow into a rubber band.
    m_leftButtonPressed = true;
    m_dragSelecting = false;
    m_dragStartPos = event->pos();
    m_dragCurrentPos = event->pos();
    m_dragStartScroll = verticalScrollBar()->value();
    m_dragModifiers = event->modifiers();
    m_dragBaseMarkedAlbumTitles = m_markedAlbumTitles;

    const QModelIndex index = indexAt(event->pos());
    m_pressStartedOnRating = index.isValid()
        && StarRating::ratingFromPosition(ratingRectForIndex(index), event->pos()) >= 0;

    if (!index.isValid()) {
        event->accept();
        return;
    }
    if (m_shiftAnchorRow < 0) {
        m_shiftAnchorRow = currentRow() >= 0 ? currentRow() : index.row();
    }
    event->accept();
}

void AlbumGrid::mouseReleaseEvent(QMouseEvent *event)
{
    const QModelIndex index = indexAt(event->pos());
    if (event->button() != Qt::LeftButton || !m_leftButtonPressed) {
        QListView::mouseReleaseEvent(event);
        return;
    }

    m_leftButtonPressed = false;
    m_dragScrollTimer->stop();
    if (m_dragSelecting) {
        finishRubberBandSelection();
        event->accept();
        return;
    }

    stopDragSelection();
    if (!index.isValid()) {
        if (event->modifiers() == Qt::NoModifier && !m_markedAlbumTitles.isEmpty()) {
            m_markedAlbumTitles.clear();
            m_marksFromMouse = false;
            applyMarkedAlbumSelection();
            emit albumNarrowFollowRequested({});
        }
        event->accept();
        return;
    }

    const int rating = StarRating::ratingFromPosition(ratingRectForIndex(index), event->pos());
    const QString title = index.data(AlbumTitleRole).toString();
    if (rating >= 0) {
        emit albumRatingChanged(index.data(AlbumArtistRole).toString(), title, rating);
        event->accept();
        return;
    }

    if (event->modifiers() & Qt::ShiftModifier) {
        const int anchor = std::clamp(m_shiftAnchorRow >= 0 ? m_shiftAnchorRow : index.row(), 0, model()->rowCount() - 1);
        const int first = std::min(anchor, index.row());
        const int last = std::max(anchor, index.row());
        for (int row = first; row <= last; ++row) {
            const QString rowTitle = titleForRow(row);
            if (!rowTitle.isEmpty()) {
                m_markedAlbumTitles.insert(rowTitle);
            }
        }
        m_marksFromMouse = true;
        m_rememberedOutlineVisible = false;
        setCurrentRowInternal(index.row(), false);
        applyMarkedAlbumSelection();
        followNarrowToSelection();
        event->accept();
        return;
    }

    if (event->modifiers() & Qt::ControlModifier) {
        if (!title.isEmpty()) {
            if (m_markedAlbumTitles.contains(title)) {
                m_markedAlbumTitles.remove(title);
            } else {
                m_markedAlbumTitles.insert(title);
            }
            m_marksFromMouse = true;
        }
        m_shiftAnchorRow = index.row();
        m_rememberedOutlineVisible = false;
        setCurrentRowInternal(index.row(), false);
        applyMarkedAlbumSelection();
        followNarrowToSelection();
        event->accept();
        return;
    }

    m_markedAlbumTitles.clear();
    m_marksFromMouse = true;
    m_shiftAnchorRow = index.row();
    selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    setCurrentIndex(index);


    if (!m_selectedAlbumTitle.isEmpty()) {
        m_rememberedOutlineVisible = true;
        refreshRememberedOutline();
        emit albumSelectionCleared();
        return;
    }

    m_rememberedOutlineVisible = false;
    refreshRememberedOutline();
    emit albumSelectionToggled(title);
    event->accept();
}

void AlbumGrid::leaveEvent(QEvent *event)
{
    auto *itemModel = qobject_cast<QStandardItemModel *>(model());
    if (itemModel != nullptr) {
        for (int row = 0; row < itemModel->rowCount(); ++row) {
            itemModel->item(row)->setData(StarRating::unset, HoverRatingRole);
        }
    }
    QListView::leaveEvent(event);
}

void AlbumGrid::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        const int step = event->angleDelta().y() > 0 ? 12 : -12;
        m_artSize = std::clamp(m_artSize + step, 96, 260);
        m_cellWidth = std::clamp(m_artSize + 28, 160, 320);
        m_cellHeight = std::clamp(m_artSize + 116, 240, 400);
        applySettingsToView();
        applySettingsToItems();
        emit viewSettingsChanged();
        event->accept();
        return;
    }

    QScrollBar *scrollBar = verticalScrollBar();
    QPoint pixelDelta = event->pixelDelta();
    QPoint angleDelta = event->angleDelta();
    int delta = 0;

    if (std::abs(pixelDelta.x()) > std::abs(pixelDelta.y()) && horizontalScrollBar()->isVisible()) {
        scrollBar = horizontalScrollBar();
        delta = -pixelDelta.x();
        if (delta == 0 && angleDelta.x() != 0) {
            m_wheelAngleRemainder.rx() += angleDelta.x();
            const int steps = m_wheelAngleRemainder.x() / 120;
            m_wheelAngleRemainder.rx() -= steps * 120;
            delta = -(steps * scrollBar->singleStep());
        }
    } else {
        delta = -pixelDelta.y();
        if (delta == 0 && angleDelta.y() != 0) {
            m_wheelAngleRemainder.ry() += angleDelta.y();
            const int steps = m_wheelAngleRemainder.y() / 120;
            m_wheelAngleRemainder.ry() -= steps * 120;
            delta = -(steps * scrollBar->singleStep());
        }
    }

    if (delta == 0) {
        event->ignore();
        return;
    }

    scrollBar->setValue(scrollBar->value() + delta);
    event->accept();
}

QRect AlbumGrid::ratingRectForIndex(const QModelIndex &index) const
{
    const QRect cell = visualRect(index).adjusted(m_padding, m_padding, -m_padding, -m_padding);
    const QRect artRect(cell.left() + ((cell.width() - m_effectiveArtSize) / 2), cell.top(), m_effectiveArtSize, m_effectiveArtSize);
    const QRect textRect(artRect.left(), artRect.bottom() + 6, artRect.width(), 58);
    const QRect ratingCell = alignedRatingCell(artRect, textRect, m_starSize, m_textAlignment);
    return StarRating::ratingRect(ratingCell, m_starSize);
}

int AlbumGrid::gridColumnCount() const
{
    // IconMode's horizontal stride is exactly the grid cell width (setSpacing is
    // ignored once a grid size is set), so the column count is floor(vpW / stride).
    const int stride = gridSize().width() > 0 ? gridSize().width() : m_effectiveCellWidth;
    return std::max(1, viewport()->width() / std::max(1, stride));
}

void AlbumGrid::showContextMenu(const QPoint &pos)
{
    QMenu menu(this);
    menu.setMinimumWidth(180);

    QMenu *sortMenu = menu.addMenu(QStringLiteral("Sort by"));
    const struct { const char *label; MusicSort::SortField field; } sortOptions[] = {
        {"Original year", MusicSort::SortField::Year},
        {"Title (A–Z)",   MusicSort::SortField::Title},
        {"Rating",        MusicSort::SortField::Rating},
        {"Track count",   MusicSort::SortField::TrackCount},
        {"Recently added", MusicSort::SortField::DateAdded},
    };
    for (const auto &opt : sortOptions) {
        QAction *action = sortMenu->addAction(QString::fromUtf8(opt.label));
        action->setCheckable(true);
        action->setChecked(m_sortField == opt.field);
        connect(action, &QAction::triggered, this, [this, field = opt.field]() {
            m_sortField = field;
            applySort();
            emit viewSettingsChanged();
        });
    }
    sortMenu->addSeparator();
    QAction *descAction = sortMenu->addAction(QStringLiteral("Descending"));
    descAction->setCheckable(true);
    descAction->setChecked(m_sortDescending);
    connect(descAction, &QAction::triggered, this, [this](bool checked) {
        m_sortDescending = checked;
        if (!checked) {
            m_sortReverseGroups = false;
        }
        applySort();
        emit viewSettingsChanged();
    });
    QAction *reverseGroupsAction = sortMenu->addAction(QStringLiteral("Reverse groups too"));
    reverseGroupsAction->setCheckable(true);
    reverseGroupsAction->setChecked(m_sortReverseGroups);
    reverseGroupsAction->setEnabled(m_sortDescending);
    connect(reverseGroupsAction, &QAction::triggered, this, [this](bool checked) {
        m_sortReverseGroups = checked;
        applySort();
        emit viewSettingsChanged();
    });

    QMenu *alignment = menu.addMenu(QStringLiteral("Text alignment"));
    for (const auto &[label, value] : {std::pair{QStringLiteral("Left"), Qt::AlignLeft}, std::pair{QStringLiteral("Center"), Qt::AlignHCenter}, std::pair{QStringLiteral("Right"), Qt::AlignRight}}) {
        QAction *action = alignment->addAction(label);
        action->setCheckable(true);
        action->setChecked(m_textAlignment == value);
        connect(action, &QAction::triggered, this, [this, value]() {
            m_textAlignment = value;
            applySettingsToItems();
            emit viewSettingsChanged();
        });
    }

    const QModelIndex index = indexAt(pos);
    if (index.isValid()) {
        const QString albumTitle = index.data(AlbumTitleRole).toString();
        const bool useMarkedSelection = !m_markedAlbumTitles.isEmpty();
        QStringList selectionTitles = useMarkedSelection
            ? albumTitlesForAction()
            : QStringList{albumTitle};
        selectionTitles.removeAll(QString());
        QAction *narrow = menu.addAction(useMarkedSelection
            ? QStringLiteral("Narrow to selection")
            : QStringLiteral("Narrow to album"));
        connect(narrow, &QAction::triggered, this, [this, selectionTitles]() {
            emit albumSelectionNarrowRequested(selectionTitles);
        });
        QAction *playReplace = menu.addAction(useMarkedSelection
            ? QStringLiteral("Play (replace queue)")
            : QStringLiteral("Play album (replace queue)"));
        connect(playReplace, &QAction::triggered, this, [this, selectionTitles]() {
            emit albumPlayReplaceRequested(selectionTitles);
        });
        QAction *playNext = menu.addAction(QStringLiteral("Play next"));
        connect(playNext, &QAction::triggered, this, [this, albumTitle]() {
            emit albumPlayNextRequested(albumTitle);
        });
        QAction *addToQueue = menu.addAction(QStringLiteral("Add to queue"));
        connect(addToQueue, &QAction::triggered, this, [this, albumTitle]() {
            emit albumAddToQueueRequested(albumTitle);
        });
        if (m_queueIsPlaylistSourced) {
            QAction *playNextTemp = menu.addAction(QStringLiteral("Play next (don't save to playlist)"));
            connect(playNextTemp, &QAction::triggered, this, [this, albumTitle]() {
                emit albumPlayNextTemporaryRequested(albumTitle);
            });
            QAction *addToQueueTemp = menu.addAction(QStringLiteral("Add to queue (don't save to playlist)"));
            connect(addToQueueTemp, &QAction::triggered, this, [this, albumTitle]() {
                emit albumAddToQueueTemporaryRequested(albumTitle);
            });
        }
    }

    if (index.isValid() && index.data(HasUserRatingRole).toBool()) {
        menu.addSeparator();
        QAction *clear = menu.addAction(QStringLiteral("Clear album rating override"));
        connect(clear, &QAction::triggered, this, [this, index]() {
            emit albumRatingChanged(index.data(AlbumArtistRole).toString(), index.data(AlbumTitleRole).toString(), StarRating::unset);
        });
    }

    menu.exec(viewport()->mapToGlobal(pos));
}

void AlbumGrid::recomputeEffectiveSizes()
{
    const int vpW = viewport()->width();
    if (vpW <= 0) {
        m_effectiveCellWidth = m_cellWidth;
        m_effectiveCellHeight = m_cellHeight;
        m_effectiveArtSize = m_artSize;
        return;
    }

    // QListView's IconMode lays items on a fixed grid whose horizontal stride is
    // exactly gridSize().width() — setSpacing() is ignored between cells once a
    // grid size is set, and overshooting the viewport re-wraps to one fewer column
    // (it never scrolls). So the stride is the bare cell width, and the column
    // count Qt will honour is floor(vpW / cellWidth).
    const int cols = std::max(1, vpW / m_cellWidth);
    // Only stretch cells to fill the viewport when items actually wrap; if they all
    // fit on one row, keep them at the configured base size. When stretching, divide
    // the full width across the columns (floor, never ceil — ceil would overshoot
    // and drop a column, opening a full-cell gap). The only slack left is the forced
    // integer remainder vpW % cols (< cols px), which is absorbed by the cards' own
    // internal padding, so the row reads as flush.
    const int effCellW = m_displayItemCount > cols
        ? std::max(m_cellWidth, vpW / cols)
        : m_cellWidth;

    const int artHPad = m_cellWidth - m_artSize;   // horizontal padding around art
    const int artVExtra = m_cellHeight - m_artSize; // text/rating area below art
    m_effectiveCellWidth = effCellW;
    m_effectiveArtSize = std::max(m_artSize, effCellW - artHPad);
    m_effectiveCellHeight = m_effectiveArtSize + artVExtra;
}

void AlbumGrid::resizeEvent(QResizeEvent *event)
{
    QListView::resizeEvent(event);
    const int previousArt = m_effectiveArtSize;
    const int previousW = m_effectiveCellWidth;
    recomputeEffectiveSizes();
    if (m_effectiveArtSize != previousArt || m_effectiveCellWidth != previousW) {
        applySettingsToView();
        applySettingsToItems();
    }
}

void AlbumGrid::applySettingsToView()
{
    recomputeEffectiveSizes();
    setSpacing(m_spacing);
    setIconSize(QSize(m_effectiveArtSize, m_effectiveArtSize));
    setGridSize(QSize(m_effectiveCellWidth, m_effectiveCellHeight));
}

void AlbumGrid::applySettingsToItems()
{
    auto *itemModel = qobject_cast<QStandardItemModel *>(model());
    if (itemModel == nullptr) {
        return;
    }
    for (int row = 0; row < itemModel->rowCount(); ++row) {
        QStandardItem *item = itemModel->item(row);
        const QString title = item->data(AlbumTitleRole).toString();
        item->setData(!title.isEmpty() && title == m_selectedAlbumTitle, SelectedRole);
        item->setData(static_cast<int>(m_textAlignment), TextAlignmentRole);
        item->setData(m_effectiveArtSize, ArtSizeRole);
        item->setData(m_padding, CellPaddingRole);
        item->setData(m_starSize, StarSizeRole);
        item->setData(QSize(m_effectiveCellWidth, m_effectiveCellHeight), Qt::SizeHintRole);
    }
    refreshRememberedOutline();
    viewport()->update();
}

void AlbumGrid::followNarrowToSelection()
{
    emit albumNarrowFollowRequested(albumTitlesForAction());
}

void AlbumGrid::reselectMarkedAlbums()
{
    if (model() == nullptr || selectionModel() == nullptr || m_markedAlbumTitles.isEmpty()) {
        return;
    }
    for (int row = 0; row < model()->rowCount(); ++row) {
        const QModelIndex index = model()->index(row, 0);
        const QString title = index.data(AlbumTitleRole).toString();
        if (!title.isEmpty() && m_markedAlbumTitles.contains(title)) {
            selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
    }
}

void AlbumGrid::applyMarkedAlbumSelection()
{
    if (model() == nullptr || selectionModel() == nullptr) {
        return;
    }
    selectionModel()->clearSelection();
    if (m_markedAlbumTitles.isEmpty()) {
        if (currentIndex().isValid()) {
            selectionModel()->select(currentIndex(), QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
        refreshRememberedOutline();
        return;
    }
    for (int row = 0; row < model()->rowCount(); ++row) {
        const QModelIndex index = model()->index(row, 0);
        const QString title = index.data(AlbumTitleRole).toString();
        if (!title.isEmpty() && m_markedAlbumTitles.contains(title)) {
            selectionModel()->select(index, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
    }
    refreshRememberedOutline();
}

void AlbumGrid::setCurrentAlbumMarked(bool marked)
{
    const QString title = currentAlbumTitle();
    if (title.isEmpty()) {
        return;
    }
    if (marked) {
        m_markedAlbumTitles.insert(title);
    } else {
        m_markedAlbumTitles.remove(title);
    }
    m_marksFromMouse = false;
    reselectMarkedAlbums();
    if (!marked && currentIndex().isValid() && selectionModel() != nullptr) {
        selectionModel()->select(currentIndex(), QItemSelectionModel::Deselect | QItemSelectionModel::Rows);
        selectionModel()->select(currentIndex(), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
    if (m_markedAlbumTitles.isEmpty()) {
        emit albumNarrowFollowRequested({});
    } else {
        followNarrowToSelection();
    }
}

void AlbumGrid::refreshRememberedOutline()
{
    auto *itemModel = qobject_cast<QStandardItemModel *>(model());
    if (itemModel == nullptr) {
        return;
    }
    const int current = currentRow();
    for (int row = 0; row < itemModel->rowCount(); ++row) {
        itemModel->item(row)->setData(m_rememberedOutlineVisible && row == current, RememberedOutlineRole);
    }
    viewport()->update();
}

void AlbumGrid::updateRubberBandSelection()
{
    if (model() == nullptr || selectionModel() == nullptr || m_rubberBand == nullptr) {
        return;
    }

    const QRect rect = m_rubberBand->geometry().normalized();
    QSet<QString> touchedTitles;
    int lastTouchedRow = -1;
    for (int row = 0; row < model()->rowCount(); ++row) {
        const QModelIndex index = model()->index(row, 0);
        if (visualRect(index).intersects(rect)) {
            const QString title = index.data(AlbumTitleRole).toString();
            if (!title.isEmpty()) {
                touchedTitles.insert(title);
                lastTouchedRow = row;
            }
        }
    }

    if (m_dragModifiers & Qt::ControlModifier) {
        m_markedAlbumTitles = m_dragBaseMarkedAlbumTitles;
        for (const QString &title : touchedTitles) {
            if (m_markedAlbumTitles.contains(title)) {
                m_markedAlbumTitles.remove(title);
            } else {
                m_markedAlbumTitles.insert(title);
            }
        }
    } else if (m_dragModifiers & Qt::ShiftModifier) {
        m_markedAlbumTitles = m_dragBaseMarkedAlbumTitles;
        m_markedAlbumTitles.unite(touchedTitles);
    } else {
        m_markedAlbumTitles = touchedTitles;
    }
    m_marksFromMouse = true;

    if (lastTouchedRow >= 0) {
        m_rememberedOutlineVisible = false;
        setCurrentRowInternal(lastTouchedRow, false);
    }
    applyMarkedAlbumSelection();
}

void AlbumGrid::finishRubberBandSelection()
{
    updateRubberBandSelection();
    if (currentRow() >= 0) {
        m_shiftAnchorRow = currentRow();
    }
    stopDragSelection();
    // Narrow once on drag release rather than on every intermediate move.
    followNarrowToSelection();
}

void AlbumGrid::updateDragAutoscroll()
{
    if (!m_dragSelecting || m_rubberBand == nullptr) {
        m_dragScrollTimer->stop();
        return;
    }

    constexpr int margin = 36;
    const int y = m_dragCurrentPos.y();
    int delta = 0;
    if (y < margin) {
        delta = -std::max(4, margin - y);
    } else if (y > viewport()->height() - margin) {
        delta = std::max(4, y - (viewport()->height() - margin));
    }

    if (delta == 0) {
        m_dragScrollTimer->stop();
        return;
    }

    verticalScrollBar()->setValue(verticalScrollBar()->value() + delta);
    m_dragCurrentPos.setY(std::clamp(m_dragCurrentPos.y(), 0, viewport()->height()));
    setRubberBandGeometry(currentDragRect());
    updateRubberBandSelection();
    if (!m_dragScrollTimer->isActive()) {
        m_dragScrollTimer->start();
    }
}

void AlbumGrid::stopDragSelection()
{
    m_dragSelecting = false;
    m_leftButtonPressed = false;
    m_pressStartedOnRating = false;
    m_dragScrollTimer->stop();
    if (m_rubberBand != nullptr) {
        m_rubberBand->hide();
    }
}

QRect AlbumGrid::currentDragRect() const
{
    // Anchor the start corner in content space: as autoscroll changes the
    // scroll position, the start point tracks the items it was placed over
    // instead of staying pinned to a now-scrolled-away viewport position.
    const int scrollDelta = m_dragStartScroll - verticalScrollBar()->value();
    const QPoint start(m_dragStartPos.x(), m_dragStartPos.y() + scrollDelta);
    return QRect(start, m_dragCurrentPos).normalized();
}

void AlbumGrid::setRubberBandGeometry(const QRect &rect)
{
    if (m_rubberBand == nullptr) {
        return;
    }
    const QRect previous = m_rubberBand->geometry();
    m_rubberBand->setGeometry(rect);
    // The viewport is custom-painted, so QRubberBand moving over empty (item-less)
    // background does not force the content beneath the old border to redraw,
    // leaving a stale frame trail. Repaint the swept region (old ∪ new, with a
    // little slack for the border width) so no trail remains.
    viewport()->update(previous.united(rect).adjusted(-2, -2, 2, 2));
}

QString AlbumGrid::titleForRow(int row) const
{
    if (model() == nullptr || row < 0 || row >= model()->rowCount()) {
        return {};
    }
    return model()->index(row, 0).data(AlbumTitleRole).toString();
}

void AlbumGrid::loadNextAlbumArtwork()
{
    auto *itemModel = qobject_cast<QStandardItemModel *>(model());
    if (itemModel == nullptr) {
        m_artworkTimer->stop();
        return;
    }

    while (m_nextArtworkRow < itemModel->rowCount()) {
        QStandardItem *item = itemModel->item(m_nextArtworkRow);
        const int row = m_nextArtworkRow;
        ++m_nextArtworkRow;
        if (item == nullptr || item->data(ArtworkGenerationRole).toInt() != m_artworkGeneration) {
            continue;
        }

        const QString representativeDir = item->data(RepresentativeDirRole).toString();
        if (representativeDir.isEmpty()) {
            return;
        }

        // Request art asynchronously (folder art only for the grid). The SVG
        // fallback is already set at item creation; onArtworkReady fills in the
        // real cover when it arrives, guarded by the artwork generation.
        if (m_artworkCache != nullptr) {
            m_artworkCache->requestArtwork(QString::number(row), representativeDir, QString(),
                                           static_cast<quint64>(m_artworkGeneration));
        }
        return;
    }

    if (!m_populateTimer->isActive()) {
        m_artworkTimer->stop();
    }
}

void AlbumGrid::onArtworkReady(const QString &token, const QImage &image, quint64 generation)
{
    if (image.isNull() || generation != static_cast<quint64>(m_artworkGeneration)) {
        return;
    }
    auto *itemModel = qobject_cast<QStandardItemModel *>(model());
    if (itemModel == nullptr) {
        return;
    }
    bool ok = false;
    const int row = token.toInt(&ok);
    if (!ok) {
        return; // not a grid token (e.g. the sidebar's "current")
    }
    QStandardItem *item = itemModel->item(row);
    if (item != nullptr && item->data(ArtworkGenerationRole).toInt() == m_artworkGeneration) {
        item->setIcon(QIcon(QPixmap::fromImage(image)));
    }
    clearItemLoading(row);
}

void AlbumGrid::onArtworkMissing(const QString &token, quint64 generation)
{
    if (generation != static_cast<quint64>(m_artworkGeneration)) {
        return;
    }
    auto *itemModel = qobject_cast<QStandardItemModel *>(model());
    if (itemModel == nullptr) {
        return;
    }
    bool ok = false;
    const int row = token.toInt(&ok);
    if (!ok) {
        return; // not a grid token (e.g. the sidebar's "current")
    }
    QStandardItem *item = itemModel->item(row);
    if (item != nullptr && item->data(ArtworkGenerationRole).toInt() == m_artworkGeneration) {
        // No art for this album: ensure the fallback shows (clears any stale
        // cover carried over from a reused row).
        item->setIcon(QIcon(AlbumArtFallback::resourcePath(palette())));
    }
    clearItemLoading(row);
}

void AlbumGrid::clearItemLoading(int row)
{
    auto *itemModel = qobject_cast<QStandardItemModel *>(model());
    if (itemModel == nullptr) {
        return;
    }
    QStandardItem *item = itemModel->item(row);
    if (item == nullptr || !item->data(LoadingRole).toBool()) {
        return;
    }
    item->setData(false, LoadingRole);
    if (m_loadingCount > 0 && --m_loadingCount == 0) {
        m_spinnerTimer->stop();
        viewport()->update();
    }
}
