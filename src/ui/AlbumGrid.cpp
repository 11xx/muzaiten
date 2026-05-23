#include "ui/AlbumGrid.h"

#include "core/Album.h"
#include "scanner/ArtworkResolver.h"
#include "ui/AlbumGridDelegate.h"
#include "ui/StarRating.h"

#include <QAction>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QStandardItemModel>
#include <algorithm>

namespace {
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
    const int width = starSize * 5 + 12;
    int left = anchorRect.left();
    if (alignment & Qt::AlignRight) {
        left = anchorRect.right() - width + 1;
    } else if (alignment & Qt::AlignHCenter) {
        left = anchorRect.left() + ((anchorRect.width() - width) / 2);
    }
    return {left, textRect.bottom() + 4, width, starSize};
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
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    setContextMenuPolicy(Qt::CustomContextMenu);
    applySettingsToView();
    setItemDelegate(new AlbumGridDelegate(this));

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
    auto *itemModel = qobject_cast<QStandardItemModel *>(model());
    itemModel->clear();
    const ArtworkResolver resolver(m_artworkCacheRoot);
    const QIcon fallbackIcon(QStringLiteral(":/artwork/album-fallback-dark.svg"));

    for (const Album &album : albums) {
        QString label = album.title;
        if (!album.date.isEmpty()) {
            label += QStringLiteral("\n%1").arg(album.date.left(4));
        }

        auto *item = new QStandardItem(label);
        item->setEditable(false);
        item->setData(album.title, AlbumTitleRole);
        item->setData(album.albumArtistName, AlbumArtistRole);
        item->setData(album.effectiveRating0To100, RatingRole);
        item->setData(StarRating::unset, HoverRatingRole);
        item->setData(!album.title.isEmpty() && album.title == m_selectedAlbumTitle, SelectedRole);
        item->setData(static_cast<int>(m_textAlignment), TextAlignmentRole);
        item->setData(m_artSize, ArtSizeRole);
        item->setData(m_padding, CellPaddingRole);
        item->setData(m_starSize, StarSizeRole);
        item->setData(album.hasUserRating, HasUserRatingRole);
        item->setData(QSize(m_cellWidth, m_cellHeight), Qt::SizeHintRole);
        item->setToolTip(QStringLiteral("%1 tracks").arg(album.trackCount));

        QIcon albumIcon = fallbackIcon;
        if (!album.representativeDir.isEmpty() && !m_artworkCacheRoot.isEmpty()) {
            const ArtworkResult artwork = resolver.resolveForDirectory(album.representativeDir);
            if (!artwork.cachePath.isEmpty()) {
                albumIcon = QIcon(artwork.cachePath);
            }
        }
        item->setIcon(albumIcon);

        itemModel->appendRow(item);
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
        m_cellHeight = std::clamp(root.value(QStringLiteral("cellHeight")).toInt(278), 220, 380);
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

QRect AlbumGrid::ratingRectForIndex(const QModelIndex &index) const
{
    const QRect cell = visualRect(index).adjusted(m_padding, m_padding, -m_padding, -m_padding);
    const QRect artRect(cell.left() + ((cell.width() - m_artSize) / 2), cell.top(), m_artSize, m_artSize);
    const QRect textRect(artRect.left(), artRect.bottom() + 6, artRect.width(), 44);
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
