#include "ui/AlbumGrid.h"

#include "core/Album.h"
#include "scanner/ArtworkResolver.h"
#include "ui/AlbumArtFallback.h"
#include "ui/AlbumGridDelegate.h"
#include "ui/StarRating.h"

#include <QAction>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QTimer>
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

    auto *model = new QStandardItemModel(this);
    auto *item = new QStandardItem(QStringLiteral("No library loaded"));
    item->setEditable(false);
    model->appendRow(item);
    setModel(model);

    connect(this, &QListView::customContextMenuRequested, this, &AlbumGrid::showContextMenu);
}

void AlbumGrid::setArtworkCacheRoot(const QString &cacheRoot)
{
    m_artworkCacheRoot = cacheRoot;
}

void AlbumGrid::setAlbums(const QVector<Album> &albums)
{
    m_populateTimer->stop();
    m_artworkTimer->stop();
    ++m_artworkGeneration;
    m_pendingAlbums = albums;
    m_nextAlbumRow = 0;
    m_nextArtworkRow = 0;

    auto *itemModel = qobject_cast<QStandardItemModel *>(model());
    const int target = static_cast<int>(m_pendingAlbums.size());

    while (itemModel->rowCount() > target) {
        itemModel->removeRow(itemModel->rowCount() - 1);
    }

    for (int i = 0; i < itemModel->rowCount(); ++i) {
        populateItemFromAlbum(itemModel->item(i), m_pendingAlbums[i]);
    }
    m_nextAlbumRow = itemModel->rowCount();

    if (m_nextAlbumRow < target) {
        appendNextAlbumBatch();
    } else {
        m_pendingAlbums.clear();
        if (itemModel->rowCount() > 0 && !m_artworkCacheRoot.isEmpty() && !m_artworkTimer->isActive()) {
            m_artworkTimer->start();
        }
    }
}

void AlbumGrid::populateItemFromAlbum(QStandardItem *item, const Album &album)
{
    QString label = album.title;
    if (!album.date.isEmpty()) {
        label += QStringLiteral("\n%1").arg(album.date.left(4));
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
        item->setData(m_artSize, ArtSizeRole);
        item->setData(m_padding, CellPaddingRole);
        item->setData(m_starSize, StarSizeRole);
        item->setData(QSize(m_cellWidth, m_cellHeight), Qt::SizeHintRole);
        item->setIcon(fallbackIcon);

        itemModel->appendRow(item);
    }

    if (m_nextAlbumRow < m_pendingAlbums.size()) {
        m_populateTimer->start();
    } else {
        m_populateTimer->stop();
        m_pendingAlbums.clear();
    }

    if (itemModel->rowCount() > 0 && !m_artworkCacheRoot.isEmpty() && !m_artworkTimer->isActive()) {
        m_artworkTimer->start();
    }
}

void AlbumGrid::setSelectedAlbumTitle(const QString &albumTitle)
{
    m_selectedAlbumTitle = albumTitle;
    applySettingsToItems();
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
    }
    applySettingsToView();
    applySettingsToItems();
}

void AlbumGrid::mouseMoveEvent(QMouseEvent *event)
{
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
    const QModelIndex index = indexAt(event->pos());
    if (!index.isValid() || event->button() != Qt::LeftButton) {
        QListView::mousePressEvent(event);
        return;
    }

    const QString title = index.data(AlbumTitleRole).toString();
    const int rating = StarRating::ratingFromPosition(ratingRectForIndex(index), event->pos());
    if (rating >= 0) {
        emit albumRatingChanged(index.data(AlbumArtistRole).toString(), title, rating);
        return;
    }

    emit albumSelectionToggled(title);
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
    const QRect artRect(cell.left() + ((cell.width() - m_artSize) / 2), cell.top(), m_artSize, m_artSize);
    const QRect textRect(artRect.left(), artRect.bottom() + 6, artRect.width(), 58);
    const QRect ratingCell = alignedRatingCell(artRect, textRect, m_starSize, m_textAlignment);
    return StarRating::ratingRect(ratingCell, m_starSize);
}

void AlbumGrid::showContextMenu(const QPoint &pos)
{
    QMenu menu(this);
    menu.setMinimumWidth(180);
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
        QAction *playNext = menu.addAction(QStringLiteral("Play next"));
        connect(playNext, &QAction::triggered, this, [this, albumTitle]() {
            emit albumPlayNextRequested(albumTitle);
        });
        QAction *addToQueue = menu.addAction(QStringLiteral("Add to queue"));
        connect(addToQueue, &QAction::triggered, this, [this, albumTitle]() {
            emit albumAddToQueueRequested(albumTitle);
        });
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

void AlbumGrid::applySettingsToView()
{
    setSpacing(m_spacing);
    setIconSize(QSize(m_artSize, m_artSize));
    setGridSize(QSize(m_cellWidth, m_cellHeight));
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
        item->setData(m_artSize, ArtSizeRole);
        item->setData(m_padding, CellPaddingRole);
        item->setData(m_starSize, StarSizeRole);
        item->setData(QSize(m_cellWidth, m_cellHeight), Qt::SizeHintRole);
    }
    viewport()->update();
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
        ++m_nextArtworkRow;
        if (item == nullptr || item->data(ArtworkGenerationRole).toInt() != m_artworkGeneration) {
            continue;
        }

        const QString representativeDir = item->data(RepresentativeDirRole).toString();
        if (representativeDir.isEmpty()) {
            return;
        }

        const ArtworkResolver resolver(m_artworkCacheRoot);
        const ArtworkResult artwork = resolver.resolveForDirectory(representativeDir);
        if (!artwork.cachePath.isEmpty()) {
            item->setIcon(QIcon(artwork.cachePath));
        }
        return;
    }

    if (!m_populateTimer->isActive()) {
        m_artworkTimer->stop();
    }
}
