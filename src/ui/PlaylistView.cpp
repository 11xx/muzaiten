#include "ui/PlaylistView.h"

#include "db/PlaylistDatabase.h"
#include "ui/DenseTableDelegate.h"
#include "ui/HeaderLabelStyle.h"
#include "ui/NavigableTableView.h"
#include "ui/NeighborColumnResizer.h"
#include "ui/OverlayScrollBar.h"
#include "ui/PanelBorderStyle.h"
#include "ui/ResponsiveColumnLayout.h"
#include "ui/ResponsiveColumnOptionsDialog.h"
#include "ui/RowHeightWheel.h"
#include "ui/RowReorderSupport.h"
#include "ui/SelectionColors.h"
#include "ui/StarRating.h"
#include "ui/StarRatingDelegate.h"
#include "ui/SplitterPersistence.h"

#include <QAbstractTableModel>
#include <QActionGroup>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QItemSelection>
#include <QTimer>
#include <QItemSelectionModel>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMimeData>
#include <QUrl>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyleOptionViewItem>
#include <QTextStream>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <utility>

namespace {

constexpr HeaderViewStyle kTableHeaderStyle{
    HeaderLabelStyle{QFont::Normal, true, HeaderLabelTone::Muted, 0.20},
    false,
};

QString statusLabel(PlaylistItemStatus status)
{
    switch (status) {
    case PlaylistItemStatus::Matched:     return QString();
    case PlaylistItemStatus::Missing:     return QStringLiteral("missing");
    case PlaylistItemStatus::Pending:     return QStringLiteral("pending");
    case PlaylistItemStatus::MultiMatch:  return QStringLiteral("multi");
    case PlaylistItemStatus::Approximate: return QStringLiteral("approx");
    }
    return QString();
}

QString durationText(qint64 ms)
{
    if (ms <= 0) {
        return QString();
    }
    const qint64 totalSeconds = ms / 1000;
    return QStringLiteral("%1:%2")
        .arg(totalSeconds / 60)
        .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'));
}

enum PlaylistItemColumn {
    OrdinalColumn,
    TitleColumn,
    ArtistColumn,
    AlbumColumn,
    LengthColumn,
    RatingColumn,
    PlaylistItemColumnCount,
};

constexpr int PlaylistIdRole = Qt::UserRole;
constexpr int PlaylistNameRole = Qt::UserRole + 1;
constexpr int PlaylistItemCountRole = Qt::UserRole + 2;
constexpr int PlaylistCreatedAtRole = Qt::UserRole + 3;
constexpr int PlaylistShowCreatedRole = Qt::UserRole + 4;
constexpr int PlaylistListActiveRole = Qt::UserRole + 5;
constexpr int PlaylistKindRole = Qt::UserRole + 6;
constexpr int QueueSnapshotIdRole = Qt::UserRole + 7;
constexpr int PlaylistMetaRole = Qt::UserRole + 8;
constexpr int PlaylistSeparatorRole = Qt::UserRole + 9;
constexpr int PlaylistUpdatedAtRole = Qt::UserRole + 10;
constexpr int PlaylistSpacerRole = Qt::UserRole + 11;
constexpr int PlaylistImportingRole = Qt::UserRole + 12;  // background drop-import in progress

enum PlaylistItemRoles {
    ItemIdRole = Qt::UserRole,
    HoverRatingRole = Qt::UserRole + 2,
};

struct PlaylistColumnSpec {
    int index;
    const char *key;
    const char *label;
    int preferredWidth;
    int minWidth;
    ResponsiveColumnPriority priority;
    bool absorber = false;
};

constexpr PlaylistColumnSpec playlistColumns[] = {
    {OrdinalColumn, "#",      "#",      48, 38, ResponsiveColumnPriority::Keep},
    {TitleColumn,   "title",  "Title", 1115, 120, ResponsiveColumnPriority::Keep, true},
    {ArtistColumn,  "artist", "Artist", 190, 100, ResponsiveColumnPriority::Normal},
    {AlbumColumn,   "album",  "Album", 220, 100, ResponsiveColumnPriority::Normal},
    {LengthColumn,  "length", "Length",  76,  64, ResponsiveColumnPriority::HideEarly},
    {RatingColumn,  "rating", "Rating",  96,  56, ResponsiveColumnPriority::Normal},
};

int minWidthForPlaylistItemColumn(int column)
{
    for (const PlaylistColumnSpec &spec : playlistColumns) {
        if (spec.index == column) {
            return spec.minWidth;
        }
    }
    return 40;
}

QVector<ResponsiveColumnSpec> playlistResponsiveSpecs()
{
    QVector<ResponsiveColumnSpec> specs;
    for (const PlaylistColumnSpec &spec : playlistColumns) {
        specs.push_back({spec.index,
                         QString::fromLatin1(spec.key),
                         spec.preferredWidth,
                         spec.minWidth,
                         spec.priority,
                         spec.absorber});
    }
    return specs;
}

QSet<QString> defaultPlaylistVisibleColumns()
{
    return {QStringLiteral("#"),
            QStringLiteral("title"),
            QStringLiteral("artist"),
            QStringLiteral("album"),
            QStringLiteral("length")};
}

QVector<ResponsiveColumnOption> playlistResponsiveOptions()
{
    QVector<ResponsiveColumnOption> options;
    for (const PlaylistColumnSpec &spec : playlistColumns) {
        options.push_back({QString::fromLatin1(spec.key), QString::fromLatin1(spec.label)});
    }
    return options;
}

QString priorityLabel(ResponsiveColumnPriority priority)
{
    switch (priority) {
    case ResponsiveColumnPriority::Keep:
        return QStringLiteral("Keep");
    case ResponsiveColumnPriority::Normal:
        return QStringLiteral("Hide later");
    case ResponsiveColumnPriority::HideEarly:
        return QStringLiteral("Hide early");
    }
    return QStringLiteral("Hide later");
}

QString sortKeyToString(PlaylistView::SortKey key)
{
    switch (key) {
    case PlaylistView::SortKey::Ordinal: return QStringLiteral("ordinal");
    case PlaylistView::SortKey::AddedAt: return QStringLiteral("addedAt");
    case PlaylistView::SortKey::Title: return QStringLiteral("title");
    case PlaylistView::SortKey::Artist: return QStringLiteral("artist");
    case PlaylistView::SortKey::Album: return QStringLiteral("album");
    case PlaylistView::SortKey::Duration: return QStringLiteral("duration");
    case PlaylistView::SortKey::Rating: return QStringLiteral("rating");
    }
    return QStringLiteral("ordinal");
}

PlaylistView::SortKey sortKeyFromString(const QString &value)
{
    if (value == QStringLiteral("addedAt")) return PlaylistView::SortKey::AddedAt;
    if (value == QStringLiteral("title")) return PlaylistView::SortKey::Title;
    if (value == QStringLiteral("artist")) return PlaylistView::SortKey::Artist;
    if (value == QStringLiteral("album")) return PlaylistView::SortKey::Album;
    if (value == QStringLiteral("duration")) return PlaylistView::SortKey::Duration;
    if (value == QStringLiteral("rating")) return PlaylistView::SortKey::Rating;
    return PlaylistView::SortKey::Ordinal;
}

QString selectorMetadataToString(PlaylistView::SelectorMetadata metadata)
{
    switch (metadata) {
    case PlaylistView::SelectorMetadata::CreatedAt:
        return QStringLiteral("createdAt");
    case PlaylistView::SelectorMetadata::UpdatedAt:
        return QStringLiteral("updatedAt");
    case PlaylistView::SelectorMetadata::Comment:
        return QStringLiteral("comment");
    case PlaylistView::SelectorMetadata::None:
        break;
    }
    return QStringLiteral("none");
}

PlaylistView::SelectorMetadata selectorMetadataFromString(const QString &value)
{
    if (value == QStringLiteral("updatedAt")) {
        return PlaylistView::SelectorMetadata::UpdatedAt;
    }
    if (value == QStringLiteral("comment")) {
        return PlaylistView::SelectorMetadata::Comment;
    }
    if (value == QStringLiteral("none")) {
        return PlaylistView::SelectorMetadata::None;
    }
    return PlaylistView::SelectorMetadata::CreatedAt;
}

QString defaultTimestampFormat()
{
    return QStringLiteral("yyyy-MM-dd'T'HH:mm:ss");
}

QString timestampMetadata(qint64 timestamp, const QString &format)
{
    if (timestamp <= 0) {
        return {};
    }
    const QString effectiveFormat = format.trimmed().isEmpty() ? defaultTimestampFormat() : format.trimmed();
    return QDateTime::fromSecsSinceEpoch(timestamp).toString(effectiveFormat);
}

int nextSelectablePlaylistRow(QListWidget *list, int row, int direction)
{
    if (list == nullptr || list->count() <= 0) {
        return -1;
    }
    row = std::clamp(row, 0, list->count() - 1);
    while (row >= 0 && row < list->count()) {
        const QListWidgetItem *item = list->item(row);
        if (item != nullptr && item->flags().testFlag(Qt::ItemIsSelectable)) {
            return row;
        }
        row += direction < 0 ? -1 : 1;
    }
    return std::clamp(row, 0, list->count() - 1);
}

// Splitter layout policy for the playlist list vs. item table. The default
// left-pane width (269px) is tuned to fit a typical playlist name plus its
// item-count badge without wasting half the view, which is what Qt hands out
// by default to a stretch-1 vs stretch-3 splitter. Stability minimums mirror
// the main-view splitters: only sane distributions are persisted or restored,
// so a tiny window or a pre-layout setSizes() can never corrupt the saved
// layout. The right pane just needs to keep the table columns legible.
constexpr int kPlaylistListDefaultWidth = 269;
constexpr int kPlaylistListMinimumWidth = 180;
constexpr int kPlaylistItemTableMinimumWidth = 360;
constexpr int kPlaylistSplitterMinimumTotal = 600;

} // namespace

class PlaylistListDelegate final : public QStyledItemDelegate {
public:
    explicit PlaylistListDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    // Rotation phase for the import spinner, advanced by PlaylistView's timer.
    void setSpinnerAngle(int angle) { m_spinnerAngle = angle; }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override
    {
        if (index.data(PlaylistSpacerRole).toBool()) {
            return index.data(Qt::SizeHintRole).toSize().isValid()
                ? index.data(Qt::SizeHintRole).toSize()
                : QSize(160, 0);
        }
        if (index.data(PlaylistSeparatorRole).toBool()) {
            return QSize(160, 20);
        }
        return index.data(Qt::SizeHintRole).toSize().isValid()
            ? index.data(Qt::SizeHintRole).toSize()
            : QSize(160, 22);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        opt.text.clear();

        if (index.data(PlaylistSpacerRole).toBool()) {
            return;
        }

        if (index.data(PlaylistSeparatorRole).toBool()) {
            const QRect lineRect = option.rect.adjusted(8, 0, -8, 0);
            const int centerY = lineRect.center().y();
            const QString label = index.data(PlaylistNameRole).toString();
            const int labelWidth = option.fontMetrics.horizontalAdvance(label) + 12;
            QRect labelRect = lineRect;
            labelRect.setWidth(std::min(labelWidth, lineRect.width()));
            QRect leftLine = lineRect;
            leftLine.setLeft(labelRect.right() + 6);

            painter->save();
            painter->setPen(option.palette.color(QPalette::Mid));
            painter->drawLine(leftLine.left(), centerY, lineRect.right(), centerY);
            painter->setPen(option.palette.color(QPalette::Disabled, QPalette::Text));
            painter->drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter,
                              option.fontMetrics.elidedText(label, Qt::ElideRight, labelRect.width()));
            painter->restore();
            return;
        }

        const bool selected = opt.state & QStyle::State_Selected;
        const bool hovered = opt.state & QStyle::State_MouseOver;
        const bool activePane = index.data(PlaylistListActiveRole).toBool();
        if (selected) {
            if (activePane) {
                painter->fillRect(opt.rect, SelectionColors::selectedFill(opt));
            } else {
                const QColor dim = SelectionColors::dimmedHighlight(opt.palette.color(QPalette::Base),
                                                                    opt.palette.color(QPalette::Highlight));
                painter->fillRect(opt.rect, dim);
            }
        } else if (hovered) {
            QColor hover = opt.palette.color(QPalette::Highlight);
            hover.setAlpha(34);
            painter->fillRect(opt.rect, hover);
        } else if (index.row() % 2 == 1) {
            painter->fillRect(opt.rect, opt.palette.color(QPalette::AlternateBase));
        }

        const QRect textRect = option.rect.adjusted(6, 0, -6, 0);
        const QString name = index.data(PlaylistNameRole).toString();
        const QString count = QString::number(index.data(PlaylistItemCountRole).toInt());
        const QString meta = index.data(PlaylistMetaRole).toString();
        const bool showDate = index.data(PlaylistShowCreatedRole).toBool();
        const qint64 createdAt = index.data(PlaylistCreatedAtRole).toLongLong();
        Q_UNUSED(showDate);
        Q_UNUSED(createdAt);
        const QString detail = meta;

        const QColor primary = selected ? SelectionColors::selectedText(option) : option.palette.color(QPalette::Text);
        const QColor secondary = selected ? primary : option.palette.color(QPalette::Disabled, QPalette::Text);
        const bool importing = index.data(PlaylistImportingRole).toBool();
        const int countWidth = option.fontMetrics.horizontalAdvance(count) + 8;
        // A spinner sits just left of the count while this playlist is still filling.
        const int spinnerSize = importing ? std::min(12, textRect.height() - 6) : 0;
        const int spinnerGap = importing ? 5 : 0;
        QRect nameRect = textRect;
        nameRect.setRight(textRect.right() - countWidth - spinnerSize - spinnerGap);

        painter->save();
        painter->setPen(secondary);
        painter->drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, count);
        if (importing && spinnerSize > 0) {
            const int sx = textRect.right() - countWidth - spinnerSize;
            const int sy = textRect.center().y() - spinnerSize / 2;
            const QRect arcRect(sx, sy, spinnerSize, spinnerSize);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            QColor arc = selected ? primary : option.palette.color(QPalette::Highlight);
            QPen pen(arc, 2);
            pen.setCapStyle(Qt::RoundCap);
            painter->setPen(pen);
            // A 270° arc whose start sweeps with m_spinnerAngle reads as a spinner.
            painter->drawArc(arcRect, -m_spinnerAngle * 16, 270 * 16);
            painter->restore();
        }
        painter->setPen(primary);
        if (detail.isEmpty()) {
            painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                              option.fontMetrics.elidedText(name, Qt::ElideRight, nameRect.width()));
        } else {
            QRect top = nameRect;
            top.setBottom(nameRect.center().y());
            QRect bottom = nameRect;
            bottom.setTop(nameRect.center().y());
            painter->drawText(top, Qt::AlignLeft | Qt::AlignVCenter,
                              option.fontMetrics.elidedText(name, Qt::ElideRight, top.width()));
            painter->setPen(secondary);
            painter->drawText(bottom, Qt::AlignLeft | Qt::AlignVCenter,
                              option.fontMetrics.elidedText(detail, Qt::ElideRight, bottom.width()));
        }
        painter->restore();
    }

private:
    int m_spinnerAngle = 0;
};

class PlaylistItemTableModel final : public QAbstractTableModel {
public:
    explicit PlaylistItemTableModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent)
    {
    }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
    }

    int columnCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : PlaylistItemColumnCount;
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
            return {};
        }

        const PlaylistItem &item = m_rows.at(index.row());
        if (role == ItemIdRole) {
            if (index.column() == RatingColumn) {
                return item.effectiveRating0To100;
            }
            return item.id;
        }
        if (role == HoverRatingRole && index.column() == RatingColumn) {
            return m_hoverRatings.value(index.row(), StarRating::unset);
        }
        if (role == Qt::EditRole && index.column() == RatingColumn) {
            return item.effectiveRating0To100;
        }
        if (role == Qt::FontRole && item.status != PlaylistItemStatus::Matched) {
            QFont font;
            font.setItalic(true);
            return font;
        }
        if (role == Qt::ForegroundRole) {
            // Tint rows that need manual attention; keep Matched at the default
            // colour. The bracketed status text and italic (FontRole) are unchanged.
            switch (item.status) {
            case PlaylistItemStatus::Missing:     return QColor(180, 48, 48);   // strong red
            case PlaylistItemStatus::Pending:     return QColor(200, 110, 110); // pink-ish red, weaker
            case PlaylistItemStatus::MultiMatch:  return QColor(190, 150, 30);  // yellow/gold
            case PlaylistItemStatus::Approximate: return QColor(210, 120, 40);  // amber
            case PlaylistItemStatus::Matched:     break;
            }
        }
        if (role == Qt::TextAlignmentRole) {
            return index.column() == OrdinalColumn || index.column() == LengthColumn
                ? QVariant(Qt::AlignRight | Qt::AlignVCenter)
                : QVariant(Qt::AlignLeft | Qt::AlignVCenter);
        }
        if (role == Qt::ToolTipRole) {
            // Surface the original imported string (kept even after a match/replace)
            // alongside any free-form comment.
            QStringList parts;
            if (!item.sourceText.isEmpty()) {
                parts << QStringLiteral("Imported as: %1").arg(item.sourceText);
            }
            if (!item.comment.isEmpty()) {
                parts << item.comment;
            }
            return parts.isEmpty() ? QVariant() : parts.join(QLatin1Char('\n'));
        }
        if (role != Qt::DisplayRole) {
            return {};
        }

        switch (index.column()) {
        case OrdinalColumn:
            return item.status == PlaylistItemStatus::Missing ? QStringLiteral("×") : QString::number(item.ordinal + 1);
        case TitleColumn: {
            const QString status = statusLabel(item.status);
            const QString title = item.status == PlaylistItemStatus::Missing
                ? QStringLiteral("× %1").arg(item.titleSnapshot)
                : item.titleSnapshot;
            return status.isEmpty()
                ? title
                : QStringLiteral("%1  [%2]").arg(title, status);
        }
        case ArtistColumn:
            return item.artistSnapshot;
        case AlbumColumn:
            return item.albumSnapshot;
        case LengthColumn:
            return durationText(item.durationMs);
        case RatingColumn:
            return item.effectiveRating0To100;
        default:
            return {};
        }
    }

    bool setData(const QModelIndex &index, const QVariant &value, int role) override
    {
        if (!index.isValid() || index.column() != RatingColumn || index.row() < 0 || index.row() >= m_rows.size()) {
            return false;
        }
        if (role == HoverRatingRole) {
            m_hoverRatings[index.row()] = value.toInt();
            emit dataChanged(index, index, {HoverRatingRole});
            return true;
        }
        if (role == Qt::EditRole) {
            m_rows[index.row()].effectiveRating0To100 = value.toInt();
            emit dataChanged(index, index, {Qt::DisplayRole, ItemIdRole, Qt::EditRole});
            return true;
        }
        return false;
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        if (index.isValid()) {
            return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;
        }
        return Qt::ItemIsEnabled | Qt::ItemIsDropEnabled;
    }

    Qt::DropActions supportedDropActions() const override
    {
        return Qt::MoveAction;
    }

    QStringList mimeTypes() const override
    {
        return {QString::fromLatin1(RowReorder::playlistMimeType)};
    }

    // Drag source only — NavigableTableView's reorder support performs the drop
    // and emits rowsReorderRequested with these rows (see PlaylistView wiring).
    QMimeData *mimeData(const QModelIndexList &indexes) const override
    {
        return RowReorder::encode(QString::fromLatin1(RowReorder::playlistMimeType),
                                  RowReorder::rowsFromIndexes(indexes));
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal) {
            return {};
        }
        if (const QVariant styleData = headerLabelStyleData(role, kTableHeaderStyle.labels); styleData.isValid()) {
            return styleData;
        }
        if (role != Qt::DisplayRole) {
            return {};
        }
        switch (section) {
        case OrdinalColumn: return QStringLiteral("#");
        case TitleColumn: return QStringLiteral("Title");
        case ArtistColumn: return QStringLiteral("Artist");
        case AlbumColumn: return QStringLiteral("Album");
        case LengthColumn: return QStringLiteral("Length");
        case RatingColumn: return QStringLiteral("Rating");
        default: return {};
        }
    }

    void setItems(const QVector<PlaylistItem> &rows)
    {
        beginResetModel();
        m_rows = rows;
        m_hoverRatings.fill(StarRating::unset, m_rows.size());
        endResetModel();
    }

    void updateRatingForPath(const QString &path, int effectiveRating0To100)
    {
        if (path.isEmpty()) {
            return;
        }
        for (int row = 0; row < m_rows.size(); ++row) {
            if (m_rows.at(row).trackPath != path) {
                continue;
            }
            m_rows[row].effectiveRating0To100 = effectiveRating0To100;
            const QModelIndex rating = index(row, RatingColumn);
            emit dataChanged(rating, rating, {Qt::DisplayRole, ItemIdRole, Qt::EditRole});
        }
    }

    const PlaylistItem *itemAt(int row) const
    {
        if (row < 0 || row >= m_rows.size()) {
            return nullptr;
        }
        return &m_rows.at(row);
    }

private:
    QVector<PlaylistItem> m_rows;
    QVector<int> m_hoverRatings;
};

PlaylistView::PlaylistView(QWidget *parent)
    : QWidget(parent)
{
    setAcceptDrops(true);  // drop importable files → one new playlist per file
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_header = new QLabel(this);
    m_header->setContentsMargins(8, 4, 8, 4);
    layout->addWidget(m_header);

    m_splitter = new QSplitter(Qt::Horizontal, this);
    m_splitter->setObjectName(QStringLiteral("PlaylistFrame"));
    layout->addWidget(m_splitter, 1);

    m_playlistList = new QListWidget(m_splitter);
    m_playlistList->setObjectName(QStringLiteral("PlaylistList"));
    m_playlistList->setFrameShape(QFrame::NoFrame);
    m_playlistList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_playlistList->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_playlistList->setItemDelegate(new PlaylistListDelegate(this));
    // Drives the selector spinner while a background drop-import is filling a
    // placeholder; only runs while m_importingPlaylists is non-empty.
    m_spinnerTimer = new QTimer(this);
    m_spinnerTimer->setInterval(80);
    connect(m_spinnerTimer, &QTimer::timeout, this, [this]() {
        m_spinnerAngle = (m_spinnerAngle + 30) % 360;
        // The selector delegate is always the PlaylistListDelegate set in the ctor.
        static_cast<PlaylistListDelegate *>(m_playlistList->itemDelegate())->setSpinnerAngle(m_spinnerAngle);
        m_playlistList->viewport()->update();
    });
    m_playlistList->setWordWrap(true);
    m_playlistList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_playlistList->installEventFilter(this);
    m_playlistList->viewport()->installEventFilter(this);

    m_itemModel = new PlaylistItemTableModel(this);
    m_itemTable = new NavigableTableView(m_splitter);
    m_itemTable->setObjectName(QStringLiteral("PlaylistItemTable"));
    m_itemTable->setModel(m_itemModel);
    m_itemTable->enableRowReorder(QString::fromLatin1(RowReorder::playlistMimeType));
    connect(m_itemTable, &NavigableTableView::rowsReorderRequested, this,
            &PlaylistView::moveItemsToIndex);
    m_itemTable->setItemDelegate(new DenseTableDelegate(this));
    m_ratingDelegate = new StarRatingDelegate(this);
    m_itemTable->setItemDelegateForColumn(RatingColumn, m_ratingDelegate);
    m_itemTable->verticalHeader()->setVisible(false);
    m_itemTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_itemTable->setAlternatingRowColors(true);
    m_itemTable->setShowGrid(false);
    m_itemTable->setWordWrap(false);
    m_itemTable->horizontalHeader()->setStretchLastSection(false);
    m_itemTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_itemTable->horizontalHeader()->setSectionsMovable(true);
    m_itemTable->horizontalHeader()->setFixedHeight(20);
    m_itemTable->horizontalHeader()->setMinimumSectionSize(8);
    m_itemTable->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    applyHeaderViewStyle(m_itemTable->horizontalHeader(), kTableHeaderStyle);
    m_itemTable->verticalHeader()->setDefaultSectionSize(20);
    m_itemTable->verticalHeader()->setMinimumSectionSize(20);
    m_itemTable->setContextMenuPolicy(Qt::CustomContextMenu);
    m_itemTable->setMouseTracking(true);
    m_itemTable->installEventFilter(this);
    m_itemTable->viewport()->setMouseTracking(true);
    m_itemTable->viewport()->installEventFilter(this);
    OverlayScrollBar::install(m_itemTable);

    // Stretch policy mirrors the main-view root splitter: the playlist list
    // (pane 0) has stretch 0 so it keeps a fixed width and never absorbs a
    // window resize or a post-restoreGeometry redistribution, while the item
    // table (pane 1) has stretch 1 and takes all extra space — its Title column
    // is the responsive absorber, so resizes land there, not on the sidebar.
    // A proportional stretch (e.g. 1:3) is what made the sidebar creep wider on
    // every tray hide/show and grow percentage-style past a viewport width.
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 1);
    m_splitter->setChildrenCollapsible(false);
    m_playlistList->setMinimumWidth(kPlaylistListMinimumWidth);
    m_itemTable->setMinimumWidth(kPlaylistItemTableMinimumWidth);
    m_splitter->setSizes({kPlaylistListDefaultWidth, kPlaylistSplitterMinimumTotal - kPlaylistListDefaultWidth});
    m_userSplitterSizes = {kPlaylistListDefaultWidth, kPlaylistSplitterMinimumTotal - kPlaylistListDefaultWidth};
    restylePanelBorders();

    connect(m_playlistList, &QListWidget::currentRowChanged, this, [this](int) {
        m_currentPlaylistId = currentPlaylistId();
        m_currentQueueSnapshotId = currentQueueSnapshotId();
        reloadItems();
    });
    connect(m_itemTable->horizontalHeader(), &QHeaderView::sectionClicked,
            this, &PlaylistView::sortByColumn);
    m_columnLayout = new ResponsiveColumnLayout(m_itemTable, playlistResponsiveSpecs(), this);
    m_columnLayout->resetToDefaults();
    m_columnLayout->setUserVisibleColumns(defaultPlaylistVisibleColumns());
    auto *columnResizer = NeighborColumnResizer::install(
        m_itemTable->horizontalHeader(), [](int column) { return minWidthForPlaylistItemColumn(column); });
    connect(columnResizer, qOverload<int, int>(&NeighborColumnResizer::columnResized), this, [this](int leftLogical, int rightLogical) {
        m_columnLayout->updateBaselineWidthsForResize(leftLogical, rightLogical);
    });
    connect(m_columnLayout, &ResponsiveColumnLayout::layoutSettingsChanged, this, [this]() {
        emit viewSettingsChanged();
    });
    connect(m_itemTable->horizontalHeader(), &QHeaderView::customContextMenuRequested,
            this, &PlaylistView::showHeaderMenu);
    connect(m_itemTable->horizontalHeader(), &QHeaderView::sectionMoved, this, [this]() {
        emit viewSettingsChanged();
    });
    connect(m_splitter, &QSplitter::splitterMoved, this, [this]() {
        // Only a real user drag updates the persisted sizes. Programmatic
        // setSizes() (defaults, resetViewSettings, redistributions on resize)
        // emits no splitterMoved, so it can never clobber a user-tuned layout.
        const QList<int> live = m_splitter->sizes();
        if (SplitterPersistence::splitterSizesAreStable(live,
                                   {kPlaylistListMinimumWidth, kPlaylistItemTableMinimumWidth},
                                   kPlaylistSplitterMinimumTotal)) {
            m_userSplitterSizes = live;
        }
        emit viewSettingsChanged();
    });
    connect(m_ratingDelegate, &StarRatingDelegate::ratingEdited, this, [this](const QModelIndex &index, int rating) {
        const PlaylistItem *item = itemForDisplayRow(index.row());
        if (item != nullptr && !item->trackPath.isEmpty()) {
            m_itemModel->setData(m_itemModel->index(index.row(), RatingColumn), rating, Qt::EditRole);
            emit trackRatingChanged(item->trackPath, rating);
        }
    });
    connect(m_playlistList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) {
        playCurrentPlaylist();
    });
    connect(m_itemTable, &QTableView::doubleClicked, this, [this](const QModelIndex &) {
        playCurrentItem();
    });
    connect(m_playlistList, &QWidget::customContextMenuRequested, this, &PlaylistView::showPlaylistMenu);
    connect(m_itemTable, &QWidget::customContextMenuRequested, this, &PlaylistView::showItemMenu);

    updatePaneFocus();
    updateHeader();
}

void PlaylistView::setDatabase(PlaylistDatabase *db)
{
    m_db = db;
    reloadPlaylists();
}

void PlaylistView::setTrackResolver(std::function<Track(const QString &)> resolver)
{
    m_trackResolver = std::move(resolver);
    refreshItemRatings();
    populateItems();
}

void PlaylistView::setSavedQueueEntries(const QVector<SavedQueuePlaylistEntry> &entries)
{
    m_savedQueueEntries = entries;
    reloadPlaylists();
}

qint64 PlaylistView::currentPlaylistId() const
{
    const QListWidgetItem *item = m_playlistList->currentItem();
    return item != nullptr ? item->data(PlaylistIdRole).toLongLong() : 0;
}

QString PlaylistView::currentQueueSnapshotId() const
{
    const QListWidgetItem *item = m_playlistList->currentItem();
    return item != nullptr ? item->data(QueueSnapshotIdRole).toString() : QString();
}

void PlaylistView::focusPlaylistList()
{
    m_playlistList->setFocus(Qt::OtherFocusReason);
    updatePaneFocus();
}

QString PlaylistView::viewSettingsJson() const
{
    QJsonArray visibleColumns;
    const QSet<QString> userVisible = m_columnLayout->userVisibleColumns();
    for (const PlaylistColumnSpec &spec : playlistColumns) {
        const QString key = QString::fromLatin1(spec.key);
        if (userVisible.contains(key)) {
            visibleColumns.append(key);
        }
    }

    QJsonObject root;
    root.insert(QStringLiteral("visibleColumns"), visibleColumns);
    root.insert(QStringLiteral("sortKey"), sortKeyToString(m_sortKey));
    root.insert(QStringLiteral("sortDescending"), m_sortDescending);
    root.insert(QStringLiteral("selectorMetadata"), selectorMetadataToString(m_selectorMetadata));
    root.insert(QStringLiteral("selectorDateFormat"), m_selectorDateFormat);
    root.insert(QStringLiteral("playlistRowHeight"), m_playlistRowHeight);
    root.insert(QStringLiteral("rowHeight"), m_itemTable->verticalHeader()->defaultSectionSize());
    root.insert(QStringLiteral("headerHeight"), m_itemTable->horizontalHeader()->height());
    root.insert(QStringLiteral("headerState"), QString::fromLatin1(m_itemTable->horizontalHeader()->saveState().toBase64()));
    // Persist the user-tuned sizes, not the live splitter sizes: programmatic
    // setSizes() must never leak into settings (see splitterMoved handler).
    root.insert(QStringLiteral("splitter"), SplitterPersistence::splitterSizesToJson(m_userSplitterSizes));
    m_columnLayout->writeSavedWidthsJson(&root);
    m_columnLayout->writePrioritiesJson(&root);
    m_columnLayout->writeMinimumWidthsJson(&root);
    m_columnLayout->writeDropOrderJson(&root);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void PlaylistView::applyViewSettingsJson(const QString &json)
{
    if (json.isEmpty()) {
        return;
    }

    const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
    const QJsonArray visible = root.value(QStringLiteral("visibleColumns")).toArray();
    QSet<QString> visibleKeys = m_columnLayout->userVisibleColumns();
    if (!visible.isEmpty()) {
        visibleKeys.clear();
        for (const QJsonValue &value : visible) {
            const QString key = value.toString();
            if (!key.isEmpty()) {
                visibleKeys.insert(key);
            }
        }
    }

    m_sortKey = sortKeyFromString(root.value(QStringLiteral("sortKey")).toString());
    m_sortDescending = root.value(QStringLiteral("sortDescending")).toBool(false);
    if (root.contains(QStringLiteral("selectorMetadata"))) {
        m_selectorMetadata = selectorMetadataFromString(root.value(QStringLiteral("selectorMetadata")).toString());
    } else if (root.contains(QStringLiteral("showCreatedDate"))) {
        m_selectorMetadata = root.value(QStringLiteral("showCreatedDate")).toBool(true)
            ? SelectorMetadata::CreatedAt
            : SelectorMetadata::None;
    } else {
        m_selectorMetadata = SelectorMetadata::UpdatedAt;
    }
    m_selectorDateFormat = root.value(QStringLiteral("selectorDateFormat")).toString(defaultTimestampFormat()).trimmed();
    if (m_selectorDateFormat.isEmpty()) {
        m_selectorDateFormat = defaultTimestampFormat();
    }
    m_playlistRowHeight = std::clamp(root.value(QStringLiteral("playlistRowHeight")).toInt(18), 18, 48);
    m_itemTable->verticalHeader()->setDefaultSectionSize(std::clamp(root.value(QStringLiteral("rowHeight")).toInt(20), 20, 48));
    setHeaderHeight(root.value(QStringLiteral("headerHeight")).toInt(20));
    const QByteArray headerState = QByteArray::fromBase64(root.value(QStringLiteral("headerState")).toString().toLatin1());
    if (!headerState.isEmpty()) {
        m_itemTable->horizontalHeader()->restoreState(headerState);
    }
    // Restore splitter sizes only if they pass the stability check, so a
    // degenerate distribution (legacy setting, hand-edit, or a snapshot taken
    // in a tiny window) can never shrink the playlist list to a sliver or
    // stretch it across the whole view. Stable sizes also become the new
    // user-tuned baseline; unstable ones leave the constructor defaults in
    // place.
    if (SplitterPersistence::restoreSplitterIfStable(m_splitter,
            root.value(QStringLiteral("splitter")).toArray(),
            {kPlaylistListMinimumWidth, kPlaylistItemTableMinimumWidth},
            kPlaylistSplitterMinimumTotal)) {
        m_userSplitterSizes = m_splitter->sizes();
    }
    m_itemTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_columnLayout->applyPrioritiesJson(root);
    m_columnLayout->applyMinimumWidthsJson(root);
    m_columnLayout->applyDropOrderJson(root);
    m_columnLayout->applySavedWidthsJson(root);
    m_columnLayout->setUserVisibleColumns(visibleKeys);
}

void PlaylistView::resetViewSettings()
{
    const QSignalBlocker headerBlocker(m_itemTable->horizontalHeader());
    for (const PlaylistColumnSpec &spec : playlistColumns) {
        m_itemTable->horizontalHeader()->setSectionResizeMode(spec.index, QHeaderView::Interactive);
    }
    for (int visual = 0; visual < PlaylistItemColumnCount; ++visual) {
        m_itemTable->horizontalHeader()->moveSection(m_itemTable->horizontalHeader()->visualIndex(visual), visual);
    }
    m_sortKey = SortKey::Ordinal;
    m_sortDescending = false;
    m_selectorMetadata = SelectorMetadata::UpdatedAt;
    m_selectorDateFormat = defaultTimestampFormat();
    m_playlistRowHeight = 18;
    setHeaderHeight(20);
    m_itemTable->verticalHeader()->setDefaultSectionSize(20);
    m_splitter->setSizes({kPlaylistListDefaultWidth, kPlaylistSplitterMinimumTotal - kPlaylistListDefaultWidth});
    m_userSplitterSizes = {kPlaylistListDefaultWidth, kPlaylistSplitterMinimumTotal - kPlaylistListDefaultWidth};
    m_columnLayout->resetToDefaults();
    m_columnLayout->setUserVisibleColumns(defaultPlaylistVisibleColumns());
    populateItems();
    reloadPlaylists();
    emit viewSettingsChanged();
}

void PlaylistView::configureMetadataDisplay(QWidget *parent)
{
    QDialog dialog(parent == nullptr ? this : parent);
    dialog.setWindowTitle(QStringLiteral("Playlist metadata display"));
    auto *layout = new QFormLayout(&dialog);

    auto *metadata = new QComboBox(&dialog);
    const QStringList labels = {
        QStringLiteral("Modified timestamp"),
        QStringLiteral("Created timestamp"),
        QStringLiteral("Comment"),
        QStringLiteral("None"),
    };
    const QVector<SelectorMetadata> values = {
        SelectorMetadata::UpdatedAt,
        SelectorMetadata::CreatedAt,
        SelectorMetadata::Comment,
        SelectorMetadata::None,
    };
    for (const QString &label : labels) {
        metadata->addItem(label);
    }
    metadata->setCurrentIndex(static_cast<int>(std::max<qsizetype>(0, values.indexOf(m_selectorMetadata))));
    layout->addRow(QStringLiteral("Metadata line:"), metadata);

    auto *dateFormat = new QLineEdit(m_selectorDateFormat, &dialog);
    dateFormat->setPlaceholderText(defaultTimestampFormat());
    layout->addRow(QStringLiteral("Date format:"), dateFormat);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const int index = metadata->currentIndex();
    if (index < 0 || index >= values.size()) {
        return;
    }
    m_selectorMetadata = values.at(index);
    m_selectorDateFormat = dateFormat->text().trimmed();
    if (m_selectorDateFormat.isEmpty()) {
        m_selectorDateFormat = defaultTimestampFormat();
    }
    reloadPlaylists();
    emit viewSettingsChanged();
}

void PlaylistView::setHeaderHeight(int height)
{
    m_itemTable->horizontalHeader()->setFixedHeight(std::clamp(height, 18, 40));
}

void PlaylistView::reloadPlaylists()
{
    if (m_db == nullptr) {
        return;
    }
    const qint64 keep = m_currentPlaylistId;
    const QString keepQueueId = m_currentQueueSnapshotId;
    m_playlistList->clear();
    const bool listActive = m_playlistList->hasFocus() || !m_itemTable->hasFocus();
    const QVector<Playlist> playlists = m_db->playlists();
    for (const Playlist &playlist : playlists) {
        QString text = playlist.name;
        auto *item = new QListWidgetItem(text, m_playlistList);
        item->setData(PlaylistIdRole, playlist.id);
        item->setData(QueueSnapshotIdRole, QString());
        item->setData(PlaylistKindRole, QStringLiteral("playlist"));
        item->setData(PlaylistNameRole, playlist.name);
        item->setData(PlaylistItemCountRole, playlist.itemCount);
        item->setData(PlaylistImportingRole, m_importingPlaylists.contains(playlist.id));
        item->setData(PlaylistCreatedAtRole, playlist.createdAt);
        item->setData(PlaylistUpdatedAtRole, playlist.updatedAt);
        item->setData(PlaylistShowCreatedRole, !selectorMetadataForPlaylist(playlist).isEmpty());
        item->setData(PlaylistMetaRole, selectorMetadataForPlaylist(playlist));
        item->setData(PlaylistListActiveRole, listActive);
        if (!playlist.comment.isEmpty()) {
            item->setToolTip(playlist.comment);
        }
    }
    if (!m_savedQueueEntries.isEmpty()) {
        auto *spacer = new QListWidgetItem(QString(), m_playlistList);
        spacer->setFlags(Qt::NoItemFlags);
        spacer->setData(PlaylistKindRole, QStringLiteral("spacer"));
        spacer->setData(PlaylistSpacerRole, true);

        auto *separator = new QListWidgetItem(QStringLiteral("Saved queues"), m_playlistList);
        separator->setFlags(Qt::NoItemFlags);
        separator->setData(PlaylistIdRole, 0);
        separator->setData(QueueSnapshotIdRole, QString());
        separator->setData(PlaylistKindRole, QStringLiteral("separator"));
        separator->setData(PlaylistNameRole, QStringLiteral("Saved queues"));
        separator->setData(PlaylistItemCountRole, 0);
        separator->setData(PlaylistCreatedAtRole, 0);
        separator->setData(PlaylistUpdatedAtRole, 0);
        separator->setData(PlaylistShowCreatedRole, false);
        separator->setData(PlaylistMetaRole, QString());
        separator->setData(PlaylistListActiveRole, listActive);
        separator->setData(PlaylistSeparatorRole, true);
    }
    for (const SavedQueuePlaylistEntry &queue : m_savedQueueEntries) {
        auto *item = new QListWidgetItem(queue.name, m_playlistList);
        item->setData(PlaylistIdRole, 0);
        item->setData(QueueSnapshotIdRole, queue.snapshotKey.isEmpty() ? queue.id : queue.snapshotKey);
        item->setData(PlaylistKindRole, QStringLiteral("queue"));
        item->setData(PlaylistNameRole, queue.name);
        item->setData(PlaylistItemCountRole, queue.items.size());
        item->setData(PlaylistCreatedAtRole, queue.savedAt);
        item->setData(PlaylistUpdatedAtRole, queue.savedAt);
        item->setData(PlaylistShowCreatedRole, !selectorMetadataForSavedQueue(queue).isEmpty());
        item->setData(PlaylistMetaRole, selectorMetadataForSavedQueue(queue));
        item->setData(PlaylistListActiveRole, listActive);
        item->setData(PlaylistSeparatorRole, false);
    }
    applyPlaylistRowHeights();
    if (!keepQueueId.isEmpty()) {
        for (int row = 0; row < m_playlistList->count(); ++row) {
            if (m_playlistList->item(row)->data(QueueSnapshotIdRole).toString() == keepQueueId) {
                m_playlistList->setCurrentRow(row);
                updatePaneFocus();
                updateHeader();
                return;
            }
        }
    }
    if (keep > 0) {
        selectPlaylist(keep);
    } else if (m_playlistList->count() > 0) {
        m_playlistList->setCurrentRow(nextSelectablePlaylistRow(m_playlistList, 0, +1));
    } else {
        m_currentPlaylistId = 0;
        reloadItems();
    }
    updatePaneFocus();
    updateHeader();
}

void PlaylistView::selectPlaylist(qint64 playlistId)
{
    for (int row = 0; row < m_playlistList->count(); ++row) {
        if (m_playlistList->item(row)->data(PlaylistIdRole).toLongLong() == playlistId) {
            m_playlistList->setCurrentRow(row);
            return;
        }
    }
}

void PlaylistView::selectItemById(qint64 itemId)
{
    if (itemId <= 0) {
        return;
    }
    for (int row = 0; row < m_itemModel->rowCount(); ++row) {
        if (const PlaylistItem *item = itemForDisplayRow(row); item != nullptr && item->id == itemId) {
            m_itemTable->setFocus(Qt::OtherFocusReason);
            setCurrentItemRow(row);
            return;
        }
    }
}

void PlaylistView::setPlaylistImporting(qint64 playlistId, bool importing)
{
    if (playlistId <= 0) {
        return;
    }
    const bool had = m_importingPlaylists.contains(playlistId);
    if (importing == had) {
        return;
    }
    if (importing) {
        m_importingPlaylists.insert(playlistId);
    } else {
        m_importingPlaylists.remove(playlistId);
    }
    // Run the spinner animation only while something is importing.
    if (m_spinnerTimer != nullptr) {
        if (!m_importingPlaylists.isEmpty() && !m_spinnerTimer->isActive()) {
            m_spinnerTimer->start();
        } else if (m_importingPlaylists.isEmpty() && m_spinnerTimer->isActive()) {
            m_spinnerTimer->stop();
        }
    }
    // Update just the affected selector row's importing flag in place.
    for (int row = 0; row < m_playlistList->count(); ++row) {
        if (m_playlistList->item(row)->data(PlaylistIdRole).toLongLong() == playlistId) {
            m_playlistList->item(row)->setData(PlaylistImportingRole, importing);
            break;
        }
    }
    m_playlistList->viewport()->update();
}

void PlaylistView::refreshImportingPlaylist(qint64 playlistId)
{
    if (m_db == nullptr || playlistId <= 0) {
        return;
    }
    // Keep the selector count current as items stream in.
    const int count = m_db->playlist(playlistId).itemCount;
    for (int row = 0; row < m_playlistList->count(); ++row) {
        if (m_playlistList->item(row)->data(PlaylistIdRole).toLongLong() == playlistId) {
            m_playlistList->item(row)->setData(PlaylistItemCountRole, count);
            break;
        }
    }
    m_playlistList->viewport()->update();
    // If the user is watching this playlist, show the new rows live.
    if (playlistId == m_currentPlaylistId) {
        const int keepRow = currentItemRow();
        reloadItems();
        if (keepRow >= 0) {
            setCurrentItemRow(keepRow);
        }
    }
}

void PlaylistView::reloadItems()
{
    m_items.clear();
    if (!m_currentQueueSnapshotId.isEmpty()) {
        for (const SavedQueuePlaylistEntry &queue : m_savedQueueEntries) {
            const QString key = queue.snapshotKey.isEmpty() ? queue.id : queue.snapshotKey;
            if (key == m_currentQueueSnapshotId || queue.id == m_currentQueueSnapshotId) {
                m_items = queue.items;
                break;
            }
        }
    } else if (m_db != nullptr && m_currentPlaylistId > 0) {
        m_items = m_db->items(m_currentPlaylistId);
    }
    refreshItemRatings();
    populateItems();
    emit viewSettingsChanged();
}

void PlaylistView::refreshItemRatings()
{
    if (!m_trackResolver) {
        return;
    }
    for (PlaylistItem &item : m_items) {
        if (item.trackPath.isEmpty()) {
            continue;
        }
        const Track track = m_trackResolver(item.trackPath);
        if (!track.path.isEmpty()) {
            item.effectiveRating0To100 = track.effectiveRating0To100;
        }
    }
}

void PlaylistView::updateTrackRating(const QString &path, int effectiveRating0To100)
{
    if (path.isEmpty()) {
        return;
    }
    for (PlaylistItem &item : m_items) {
        if (item.trackPath == path) {
            item.effectiveRating0To100 = effectiveRating0To100;
        }
    }
    if (m_itemModel != nullptr) {
        m_itemModel->updateRatingForPath(path, effectiveRating0To100);
    }
}

QVector<PlaylistItem> PlaylistView::displayItems() const
{
    QVector<PlaylistItem> sorted = m_items;
    if (m_sortKey == SortKey::Ordinal) {
        // Canonical order; m_items already arrives ordered by ordinal.
        if (m_sortDescending) {
            std::reverse(sorted.begin(), sorted.end());
        }
        return sorted;
    }
    const auto cmp = [this](const PlaylistItem &a, const PlaylistItem &b) {
        int c = 0;
        switch (m_sortKey) {
        case SortKey::AddedAt:
            c = a.addedAt < b.addedAt ? -1 : (a.addedAt > b.addedAt ? 1 : 0);
            break;
        case SortKey::Title:
            c = a.titleSnapshot.localeAwareCompare(b.titleSnapshot);
            break;
        case SortKey::Artist:
            c = a.artistSnapshot.localeAwareCompare(b.artistSnapshot);
            break;
        case SortKey::Album:
            c = a.albumSnapshot.localeAwareCompare(b.albumSnapshot);
            break;
        case SortKey::Duration:
            c = a.durationMs < b.durationMs ? -1 : (a.durationMs > b.durationMs ? 1 : 0);
            break;
        case SortKey::Rating:
            c = a.effectiveRating0To100 < b.effectiveRating0To100 ? -1 : (a.effectiveRating0To100 > b.effectiveRating0To100 ? 1 : 0);
            break;
        case SortKey::Ordinal:
            break;
        }
        if (c == 0) {
            c = a.ordinal - b.ordinal;  // stable tiebreak on canonical position
        }
        return m_sortDescending ? c > 0 : c < 0;
    };
    std::stable_sort(sorted.begin(), sorted.end(), cmp);
    return sorted;
}

void PlaylistView::populateItems()
{
    const int keepRow = currentItemRow();
    const QVector<PlaylistItem> rows = displayItems();
    m_itemModel->setItems(rows);
    if (m_itemModel->rowCount() > 0) {
        setCurrentItemRow(std::clamp(keepRow, 0, m_itemModel->rowCount() - 1));
    }
    updateHeader();
}

void PlaylistView::cycleAddedSort()
{
    // Toggle Newest -> Oldest -> back to canonical ordinal.
    if (m_sortKey != SortKey::AddedAt) {
        m_sortKey = SortKey::AddedAt;
        m_sortDescending = true;  // Newest first
    } else if (m_sortDescending) {
        m_sortDescending = false; // Oldest first
    } else {
        m_sortKey = SortKey::Ordinal;
        m_sortDescending = false;
    }
    populateItems();
}

void PlaylistView::sortByColumn(int column)
{
    const SortKey key = [column]() {
        switch (column) {
        case 1: return SortKey::Title;
        case 2: return SortKey::Artist;
        case 3: return SortKey::Album;
        case 4: return SortKey::Duration;
        case 5: return SortKey::Rating;
        default: return SortKey::Ordinal;  // "#" column restores canonical order
        }
    }();
    if (key == m_sortKey && key != SortKey::Ordinal) {
        m_sortDescending = !m_sortDescending;  // re-click flips direction
    } else {
        m_sortKey = key;
        m_sortDescending = false;
    }
    populateItems();
    emit viewSettingsChanged();
}

void PlaylistView::updateHeader()
{
    if (currentSelectionIsSavedQueue()) {
        const QListWidgetItem *current = m_playlistList->currentItem();
        const QString name = current != nullptr ? current->data(PlaylistNameRole).toString() : QString();
        m_header->setText(QStringLiteral("%1 — saved queue, %2 items").arg(name).arg(m_items.size()));
        return;
    }
    if (m_currentPlaylistId <= 0) {
        m_header->setText(QStringLiteral(
            "Playlists — a: new   R: rename   D: delete   x: export   =/+: add song   T: toggle date   Enter: play   l: open   "
            "(inside) =/+: add   a: add-to-playlist   e: edit   d: remove   Alt+j/k: move   s: sort   Enter: play"));
        return;
    }
    const QListWidgetItem *current = m_playlistList->currentItem();
    const QString name = current != nullptr ? current->data(PlaylistNameRole).toString() : QString();
    m_header->setText(QStringLiteral("%1 — %2 items").arg(name).arg(m_items.size()));
}

const PlaylistItem *PlaylistView::itemForDisplayRow(int row) const
{
    return m_itemModel != nullptr ? m_itemModel->itemAt(row) : nullptr;
}

bool PlaylistView::currentSelectionIsSavedQueue() const
{
    return !m_currentQueueSnapshotId.isEmpty();
}

QStringList PlaylistView::pathsForSavedQueue(const QString &snapshotId, int *startIndex) const
{
    QStringList paths;
    if (startIndex != nullptr) {
        *startIndex = 0;
    }
    const int current = currentItemRow();
    for (const SavedQueuePlaylistEntry &queue : m_savedQueueEntries) {
        const QString key = queue.snapshotKey.isEmpty() ? queue.id : queue.snapshotKey;
        if (key != snapshotId && queue.id != snapshotId) {
            continue;
        }
        for (int row = 0; row < queue.items.size(); ++row) {
            const PlaylistItem &item = queue.items.at(row);
            if (item.status != PlaylistItemStatus::Matched || item.trackPath.isEmpty()) {
                continue;
            }
            if (row == current && startIndex != nullptr) {
                *startIndex = static_cast<int>(paths.size());
            }
            paths << item.trackPath;
        }
        break;
    }
    return paths;
}

QStringList PlaylistView::selectedItemPaths(int *startIndex) const
{
    if (currentSelectionIsSavedQueue()) {
        return pathsForSavedQueue(m_currentQueueSnapshotId, startIndex);
    }
    QStringList paths;
    int first = -1;
    const QModelIndexList selected = m_itemTable->selectionModel() != nullptr
        ? m_itemTable->selectionModel()->selectedRows(0)
        : QModelIndexList();
    if (selected.size() > 1) {
        QVector<int> rows;
        for (const QModelIndex &index : selected) {
            rows.push_back(index.row());
        }
        std::sort(rows.begin(), rows.end());  // honor on-screen (display) order
        for (int row : rows) {
            const PlaylistItem *item = itemForDisplayRow(row);
            if (item != nullptr && item->status == PlaylistItemStatus::Matched && !item->trackPath.isEmpty()) {
                paths << item->trackPath;
            }
        }
        if (startIndex != nullptr) {
            *startIndex = 0;
        }
        return paths;
    }
    // Single selection: hand the whole playlist (in display order) over so
    // playback continues past the chosen row, with startIndex pointing at it.
    const int current = currentItemRow();
    for (int row = 0; row < m_itemModel->rowCount(); ++row) {
        const PlaylistItem *item = itemForDisplayRow(row);
        if (item == nullptr || item->status != PlaylistItemStatus::Matched || item->trackPath.isEmpty()) {
            continue;
        }
        if (row == current) {
            first = static_cast<int>(paths.size());
        }
        paths << item->trackPath;
    }
    if (startIndex != nullptr) {
        *startIndex = std::max(0, first);
    }
    return paths;
}

QStringList PlaylistView::selectedOnlyItemPaths() const
{
    QStringList paths;
    QVector<int> rows;
    const QModelIndexList selected = m_itemTable->selectionModel() != nullptr
        ? m_itemTable->selectionModel()->selectedRows(0)
        : QModelIndexList();
    if (selected.isEmpty()) {
        rows.push_back(currentItemRow());
    } else {
        rows.reserve(selected.size());
        for (const QModelIndex &index : selected) {
            rows.push_back(index.row());
        }
        std::sort(rows.begin(), rows.end());
    }

    for (int row : rows) {
        const PlaylistItem *item = itemForDisplayRow(row);
        if (item != nullptr && item->status == PlaylistItemStatus::Matched && !item->trackPath.isEmpty()) {
            paths << item->trackPath;
        }
    }
    return paths;
}

void PlaylistView::playCurrentPlaylist()
{
    if (currentSelectionIsSavedQueue()) {
        emit playSavedQueueRequested(m_currentQueueSnapshotId, 0);
        return;
    }
    if (m_db == nullptr || m_currentPlaylistId <= 0) {
        return;
    }
    QStringList paths;
    for (const PlaylistItem &item : m_db->items(m_currentPlaylistId)) {
        if (item.status == PlaylistItemStatus::Matched && !item.trackPath.isEmpty()) {
            paths << item.trackPath;
        }
    }
    if (!paths.isEmpty()) {
        emit playPathsRequested(paths, 0);
    }
}

void PlaylistView::addCurrentPlaylistToQueue()
{
    enqueueCurrentPlaylist(/*playNext=*/false, /*temporary=*/false);
}

void PlaylistView::playNextCurrentPlaylist()
{
    enqueueCurrentPlaylist(/*playNext=*/true, /*temporary=*/false);
}

void PlaylistView::enqueueCurrentPlaylist(bool playNext, bool temporary)
{
    if (currentSelectionIsSavedQueue()) {
        // Saved queues are not the playlist that backs the queue, so there is
        // nothing to mirror — "temporary" collapses to the normal saved-queue add.
        if (playNext) {
            emit playNextSavedQueueRequested(m_currentQueueSnapshotId);
        } else {
            emit addSavedQueueToQueueRequested(m_currentQueueSnapshotId);
        }
        return;
    }
    if (m_db == nullptr || m_currentPlaylistId <= 0) {
        return;
    }
    QStringList paths;
    for (const PlaylistItem &item : m_db->items(m_currentPlaylistId)) {
        if (item.status == PlaylistItemStatus::Matched && !item.trackPath.isEmpty()) {
            paths << item.trackPath;
        }
    }
    if (paths.isEmpty()) {
        return;
    }
    if (playNext) {
        temporary ? emit playNextPathsTemporaryRequested(paths) : emit playNextPathsRequested(paths);
    } else {
        temporary ? emit addPathsToQueueTemporaryRequested(paths) : emit addPathsToQueueRequested(paths);
    }
}

void PlaylistView::addSongToCurrentPlaylist()
{
    if (!currentSelectionIsSavedQueue() && m_currentPlaylistId > 0) {
        emit addSongRequested(m_currentPlaylistId);
    }
}

void PlaylistView::importIntoCurrentPlaylist()
{
    if (!currentSelectionIsSavedQueue() && m_currentPlaylistId > 0) {
        emit importRequested(m_currentPlaylistId);
    }
}

void PlaylistView::moveCurrentItemUp()
{
    moveSelectedItems(-1);
}

void PlaylistView::moveCurrentItemDown()
{
    moveSelectedItems(+1);
}

void PlaylistView::playCurrentItem()
{
    if (currentSelectionIsSavedQueue()) {
        int startIndex = 0;
        const QStringList paths = selectedItemPaths(&startIndex);
        if (!paths.isEmpty()) {
            emit playSavedQueueRequested(m_currentQueueSnapshotId, startIndex);
        }
        return;
    }
    int startIndex = 0;
    const QStringList paths = selectedItemPaths(&startIndex);
    if (!paths.isEmpty()) {
        emit playPathsRequested(paths, startIndex);
    }
}

void PlaylistView::playNextSelectedItems()
{
    enqueueSelectedItems(/*playNext=*/true, /*temporary=*/false);
}

void PlaylistView::addSelectedItemsToQueue()
{
    enqueueSelectedItems(/*playNext=*/false, /*temporary=*/false);
}

void PlaylistView::enqueueSelectedItems(bool playNext, bool temporary)
{
    const QStringList paths = selectedOnlyItemPaths();
    if (paths.isEmpty()) {
        return;
    }
    if (playNext) {
        temporary ? emit playNextPathsTemporaryRequested(paths) : emit playNextPathsRequested(paths);
    } else {
        temporary ? emit addPathsToQueueTemporaryRequested(paths) : emit addPathsToQueueRequested(paths);
    }
}

void PlaylistView::addSelectedItemsToPlaylist()
{
    const QStringList paths = selectedOnlyItemPaths();
    if (!paths.isEmpty()) {
        emit addToPlaylistRequested(paths);
    }
}

void PlaylistView::editCurrentItem()
{
    const PlaylistItem *item = itemForDisplayRow(currentItemRow());
    if (item != nullptr && m_currentPlaylistId > 0) {
        emit editItemRequested(m_currentPlaylistId, item->id, item->query);
    }
}

void PlaylistView::showPlaylistMenu(const QPoint &pos)
{
    const int row = m_playlistList->row(m_playlistList->itemAt(pos));
    if (row >= 0) {
        m_playlistList->setCurrentRow(row);
    }

    QMenu menu(this);
    const bool savedQueue = currentSelectionIsSavedQueue();
    const bool hasPlayableCollection = savedQueue || m_currentPlaylistId > 0;
    QAction *play = menu.addAction(QStringLiteral("Play"));
    play->setEnabled(hasPlayableCollection);
    connect(play, &QAction::triggered, this, &PlaylistView::playCurrentPlaylist);
    QAction *addToQueue = menu.addAction(QStringLiteral("Add to queue"));
    addToQueue->setEnabled(hasPlayableCollection);
    connect(addToQueue, &QAction::triggered, this, &PlaylistView::addCurrentPlaylistToQueue);
    QAction *playNext = menu.addAction(QStringLiteral("Play next"));
    playNext->setEnabled(hasPlayableCollection);
    connect(playNext, &QAction::triggered, this, &PlaylistView::playNextCurrentPlaylist);
    if (m_queueIsPlaylistSourced && hasPlayableCollection && !savedQueue) {
        QAction *playNextTemp = menu.addAction(QStringLiteral("Play next (don't save to playlist)"));
        connect(playNextTemp, &QAction::triggered, this, [this]() { enqueueCurrentPlaylist(true, true); });
        QAction *addToQueueTemp = menu.addAction(QStringLiteral("Add to queue (don't save to playlist)"));
        connect(addToQueueTemp, &QAction::triggered, this, [this]() { enqueueCurrentPlaylist(false, true); });
    }

    menu.addSeparator();
    QAction *addSong = menu.addAction(QStringLiteral("Add song..."));
    addSong->setEnabled(!savedQueue && m_currentPlaylistId > 0);
    connect(addSong, &QAction::triggered, this, &PlaylistView::addSongToCurrentPlaylist);
    QAction *importAction = menu.addAction(QStringLiteral("Import into this playlist..."));
    importAction->setEnabled(!savedQueue && m_currentPlaylistId > 0);
    connect(importAction, &QAction::triggered, this, &PlaylistView::importIntoCurrentPlaylist);
    QAction *importNew = menu.addAction(QStringLiteral("Import playlist..."));
    connect(importNew, &QAction::triggered, this, &PlaylistView::importNewRequested);
    menu.addAction(QStringLiteral("New playlist..."), this, &PlaylistView::createPlaylist);
    QAction *rename = menu.addAction(QStringLiteral("Rename"));
    rename->setEnabled(!savedQueue && m_currentPlaylistId > 0);
    connect(rename, &QAction::triggered, this, &PlaylistView::renameCurrentPlaylist);
    QAction *exportAction = menu.addAction(QStringLiteral("Export..."));
    exportAction->setEnabled(!savedQueue && m_currentPlaylistId > 0);
    connect(exportAction, &QAction::triggered, this, &PlaylistView::exportCurrentPlaylist);
    QAction *deleteAction = menu.addAction(QStringLiteral("Delete"));
    deleteAction->setEnabled(!savedQueue && m_currentPlaylistId > 0);
    connect(deleteAction, &QAction::triggered, this, &PlaylistView::deleteCurrentPlaylist);
    QAction *deleteSavedQueue = menu.addAction(QStringLiteral("Delete saved queue"));
    deleteSavedQueue->setEnabled(savedQueue);
    connect(deleteSavedQueue, &QAction::triggered, this, [this]() {
        if (!m_currentQueueSnapshotId.isEmpty()) {
            emit deleteSavedQueueRequested(m_currentQueueSnapshotId);
        }
    });

    menu.exec(m_playlistList->viewport()->mapToGlobal(pos));
}

void PlaylistView::showItemMenu(const QPoint &pos)
{
    const QModelIndex index = m_itemTable->indexAt(pos);
    if (!index.isValid()) {
        QMenu menu(this);
        QAction *addSong = menu.addAction(QStringLiteral("Add song..."));
        addSong->setEnabled(m_currentPlaylistId > 0);
        connect(addSong, &QAction::triggered, this, &PlaylistView::addSongToCurrentPlaylist);
        menu.exec(m_itemTable->viewport()->mapToGlobal(pos));
        return;
    }
    if (!m_itemTable->selectionModel()->isRowSelected(index.row(), QModelIndex())) {
        setCurrentItemRow(index.row());
    } else {
        m_itemTable->setCurrentIndex(m_itemModel->index(index.row(), 0));
    }

    const PlaylistItem *item = itemForDisplayRow(index.row());
    const bool hasPlayableSelection = !selectedOnlyItemPaths().isEmpty();
    QMenu menu(this);
    QAction *play = menu.addAction(QStringLiteral("Play"));
    play->setEnabled(item != nullptr && item->status == PlaylistItemStatus::Matched && !item->trackPath.isEmpty());
    connect(play, &QAction::triggered, this, &PlaylistView::playCurrentItem);
    QAction *playNext = menu.addAction(QStringLiteral("Play next"));
    playNext->setEnabled(hasPlayableSelection);
    connect(playNext, &QAction::triggered, this, &PlaylistView::playNextSelectedItems);
    QAction *addToQueue = menu.addAction(QStringLiteral("Add to queue"));
    addToQueue->setEnabled(hasPlayableSelection);
    connect(addToQueue, &QAction::triggered, this, &PlaylistView::addSelectedItemsToQueue);
    if (m_queueIsPlaylistSourced) {
        QAction *playNextTemp = menu.addAction(QStringLiteral("Play next (don't save to playlist)"));
        playNextTemp->setEnabled(hasPlayableSelection);
        connect(playNextTemp, &QAction::triggered, this, [this]() { enqueueSelectedItems(true, true); });
        QAction *addToQueueTemp = menu.addAction(QStringLiteral("Add to queue (don't save to playlist)"));
        addToQueueTemp->setEnabled(hasPlayableSelection);
        connect(addToQueueTemp, &QAction::triggered, this, [this]() { enqueueSelectedItems(false, true); });
    }
    QAction *addToPlaylist = menu.addAction(QStringLiteral("Add to playlist..."));
    addToPlaylist->setEnabled(hasPlayableSelection);
    connect(addToPlaylist, &QAction::triggered, this, &PlaylistView::addSelectedItemsToPlaylist);

    menu.addSeparator();
    QAction *edit = menu.addAction(QStringLiteral("Edit match"));
    edit->setEnabled(item != nullptr && !currentSelectionIsSavedQueue() && m_currentPlaylistId > 0);
    connect(edit, &QAction::triggered, this, &PlaylistView::editCurrentItem);
    QAction *properties = menu.addAction(QStringLiteral("Properties"));
    properties->setEnabled(item != nullptr && item->status == PlaylistItemStatus::Matched && !item->trackPath.isEmpty());
    connect(properties, &QAction::triggered, this, [this, item]() {
        if (item != nullptr && item->status == PlaylistItemStatus::Matched && !item->trackPath.isEmpty()) {
            emit propertiesForPathRequested(item->trackPath);
        }
    });
    if (item != nullptr && item->status == PlaylistItemStatus::Missing && !currentSelectionIsSavedQueue()) {
        menu.addSeparator();
        QAction *removeMissing = menu.addAction(QStringLiteral("Remove missing tracks from playlist"));
        connect(removeMissing, &QAction::triggered, this, &PlaylistView::removeSelectedMissingItems);
        QAction *removeAllMissing = menu.addAction(QStringLiteral("Remove all missing tracks from library"));
        connect(removeAllMissing, &QAction::triggered, this, [this]() {
            emit removeAllMissingTracksRequested();
        });
    }
    QAction *remove = menu.addAction(QStringLiteral("Remove selected"));
    remove->setEnabled(item != nullptr && !currentSelectionIsSavedQueue());
    connect(remove, &QAction::triggered, this, &PlaylistView::removeSelectedItems);

    menu.addSeparator();
    QAction *moveUp = menu.addAction(QStringLiteral("Move up"));
    moveUp->setEnabled(item != nullptr && !currentSelectionIsSavedQueue() && m_itemModel->rowCount() > 1);
    connect(moveUp, &QAction::triggered, this, [this]() { moveSelectedItems(-1); });
    QAction *moveDown = menu.addAction(QStringLiteral("Move down"));
    moveDown->setEnabled(item != nullptr && !currentSelectionIsSavedQueue() && m_itemModel->rowCount() > 1);
    connect(moveDown, &QAction::triggered, this, [this]() { moveSelectedItems(+1); });

    menu.exec(m_itemTable->viewport()->mapToGlobal(pos));
}

void PlaylistView::showHeaderMenu(const QPoint &pos)
{
    QMenu menu(this);
    QSet<QString> visibleKeys = m_columnLayout->userVisibleColumns();
    for (const PlaylistColumnSpec &spec : playlistColumns) {
        QAction *action = menu.addAction(QString::fromLatin1(spec.label));
        action->setCheckable(true);
        const QString key = QString::fromLatin1(spec.key);
        action->setChecked(visibleKeys.contains(key));
        connect(action, &QAction::toggled, this, [this, key](bool checked) {
            QSet<QString> keys = m_columnLayout->userVisibleColumns();
            if (checked) {
                keys.insert(key);
            } else {
                keys.remove(key);
            }
            m_columnLayout->setUserVisibleColumns(keys);
            emit viewSettingsChanged();
        });
    }

    menu.addSeparator();
    QMenu *priorityMenu = menu.addMenu(QStringLiteral("Responsive priority"));
    for (const PlaylistColumnSpec &spec : playlistColumns) {
        const QString key = QString::fromLatin1(spec.key);
        QMenu *columnMenu = priorityMenu->addMenu(QString::fromLatin1(spec.label));
        auto *group = new QActionGroup(columnMenu);
        group->setExclusive(true);
        for (const ResponsiveColumnPriority priority : {ResponsiveColumnPriority::Keep,
                                                        ResponsiveColumnPriority::Normal,
                                                        ResponsiveColumnPriority::HideEarly}) {
            QAction *action = columnMenu->addAction(priorityLabel(priority));
            action->setCheckable(true);
            action->setActionGroup(group);
            action->setChecked(m_columnLayout->columnPriority(key) == priority);
            connect(action, &QAction::triggered, this, [this, key, priority]() {
                m_columnLayout->setColumnPriority(key, priority);
            });
        }
    }

    menu.addSeparator();
    QAction *responsiveOptions = menu.addAction(QStringLiteral("Responsive options..."));
    connect(responsiveOptions, &QAction::triggered, this, [this]() {
        ResponsiveColumnOptionsDialog dialog(m_columnLayout, playlistResponsiveOptions(), this);
        dialog.exec();
    });

    menu.addSeparator();
    QAction *resetLayout = menu.addAction(QStringLiteral("Reset table layout to defaults"));
    connect(resetLayout, &QAction::triggered, this, &PlaylistView::resetViewSettings);

    menu.exec(m_itemTable->horizontalHeader()->mapToGlobal(pos));
}

void PlaylistView::setCurrentItemRow(int row, int direction)
{
    m_itemTable->setCurrentNavigationRow(row, direction);
}

int PlaylistView::currentItemRow() const
{
    return m_itemTable->currentNavigationRow();
}

void PlaylistView::setHoveredItemRow(int row)
{
    if (m_itemHoveredRow == row) {
        return;
    }
    const int previous = m_itemHoveredRow;
    m_itemHoveredRow = row;
    if (auto *denseDelegate = qobject_cast<DenseTableDelegate *>(m_itemTable->itemDelegate())) {
        denseDelegate->setHoveredRow(row);
    }
    if (m_ratingDelegate != nullptr) {
        m_ratingDelegate->setHoveredRow(row);
    }
    if (previous >= 0) {
        const QRect rect = m_itemTable->visualRect(m_itemModel->index(previous, 0));
        m_itemTable->viewport()->update(QRect(0, rect.top(), m_itemTable->viewport()->width(), rect.height()));
    }
    if (row >= 0) {
        const QRect rect = m_itemTable->visualRect(m_itemModel->index(row, 0));
        m_itemTable->viewport()->update(QRect(0, rect.top(), m_itemTable->viewport()->width(), rect.height()));
    }
}

QVector<qint64> PlaylistView::displayedItemIds() const
{
    QVector<qint64> ids;
    ids.reserve(m_itemModel->rowCount());
    for (int row = 0; row < m_itemModel->rowCount(); ++row) {
        if (const PlaylistItem *item = itemForDisplayRow(row)) {
            ids.push_back(item->id);
        }
    }
    return ids;
}

void PlaylistView::pushUndoSnapshot()
{
    if (m_db == nullptr || m_currentPlaylistId <= 0) {
        return;
    }
    constexpr int kMaxUndo = 50;
    m_undoStack.push_back({m_currentPlaylistId, m_items});
    if (m_undoStack.size() > kMaxUndo) {
        m_undoStack.removeFirst();
    }
}

void PlaylistView::undoLastChange()
{
    if (m_db == nullptr || m_undoStack.isEmpty()) {
        return;
    }
    const UndoSnapshot snapshot = m_undoStack.takeLast();
    restorePlaylistItems(snapshot.playlistId, snapshot.items);
    // Make the restored playlist current and refresh both panes.
    selectPlaylist(snapshot.playlistId);
    reloadItems();
    reloadPlaylists();
}

void PlaylistView::restorePlaylistItems(qint64 playlistId, const QVector<PlaylistItem> &snapshot)
{
    if (m_db == nullptr || playlistId <= 0) {
        return;
    }
    const QVector<PlaylistItem> current = m_db->items(playlistId);
    QSet<qint64> currentIds;
    for (const PlaylistItem &item : current) {
        currentIds.insert(item.id);
    }
    QSet<qint64> snapshotIds;
    for (const PlaylistItem &item : snapshot) {
        snapshotIds.insert(item.id);
    }
    // Rebuild the snapshot order, re-adding any rows that were removed since (they
    // come back with fresh ids, but identical data — see addItem).
    QVector<qint64> desired;
    desired.reserve(snapshot.size());
    for (const PlaylistItem &item : snapshot) {
        qint64 id = item.id;
        if (!currentIds.contains(id)) {
            id = m_db->addItem(playlistId, item);
            if (id <= 0) {
                continue;
            }
        }
        desired.push_back(id);
    }
    // Drop anything added after the snapshot (undoing an addition).
    for (const PlaylistItem &item : current) {
        if (!snapshotIds.contains(item.id)) {
            m_db->removeItem(item.id);
        }
    }
    m_db->reorderItems(playlistId, desired);
}

void PlaylistView::moveSelectedItems(int delta)
{
    if (m_db == nullptr || m_currentPlaylistId <= 0 || delta == 0 || m_itemModel->rowCount() <= 1) {
        return;
    }
    if (m_sortKey != SortKey::Ordinal || m_sortDescending) {
        m_sortKey = SortKey::Ordinal;
        m_sortDescending = false;
        populateItems();
        emit viewSettingsChanged();
    }

    QVector<int> rows;
    const QModelIndexList selected = m_itemTable->selectionModel() != nullptr
        ? m_itemTable->selectionModel()->selectedRows(0)
        : QModelIndexList();
    if (selected.isEmpty()) {
        rows.push_back(currentItemRow());
    } else {
        rows.reserve(selected.size());
        for (const QModelIndex &index : selected) {
            rows.push_back(index.row());
        }
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    rows.erase(std::remove_if(rows.begin(), rows.end(), [this](int row) {
        return row < 0 || row >= m_itemModel->rowCount();
    }), rows.end());
    if (rows.isEmpty()) {
        return;
    }
    if (delta < 0 && rows.first() == 0) {
        return;
    }
    if (delta > 0 && rows.last() == m_itemModel->rowCount() - 1) {
        return;
    }

    QVector<qint64> ids = displayedItemIds();
    if (delta < 0) {
        for (int row : rows) {
            ids.swapItemsAt(row, row - 1);
        }
    } else {
        for (auto it = rows.crbegin(); it != rows.crend(); ++it) {
            const int row = *it;
            ids.swapItemsAt(row, row + 1);
        }
    }
    pushUndoSnapshot();
    if (!m_db->reorderItems(m_currentPlaylistId, ids)) {
        QMessageBox::warning(this, QStringLiteral("Playlist"), m_db->lastError());
        return;
    }

    const int current = std::clamp(currentItemRow() + delta, 0, m_itemModel->rowCount() - 1);
    reloadItems();
    // reloadPlaylists() clear()s the selector list, which transiently empties the
    // item table (currentRowChanged → reloadItems with no current playlist) and
    // would clamp the cursor back to row 0. Refresh the selector first, then plant
    // the cursor on the moved row last so repeated Shift+n/p chains stay put.
    reloadPlaylists();
    setCurrentItemRow(current, delta);
}

void PlaylistView::moveItemsToIndex(const QVector<int> &rows, int destinationRow)
{
    if (m_db == nullptr || m_currentPlaylistId <= 0 || rows.isEmpty() || m_itemModel->rowCount() <= 1) {
        return;
    }
    // Reordering is only meaningful against the canonical ordinal order; a drag
    // while a display sort is active falls back to it first (mirrors the keyboard
    // Shift+n/p path).
    if (m_sortKey != SortKey::Ordinal || m_sortDescending) {
        m_sortKey = SortKey::Ordinal;
        m_sortDescending = false;
        populateItems();
        emit viewSettingsChanged();
    }

    QVector<qint64> ids = displayedItemIds();
    const int count = static_cast<int>(ids.size());
    QSet<int> moving;
    for (int row : rows) {
        if (row >= 0 && row < count) {
            moving.insert(row);
        }
    }
    if (moving.isEmpty()) {
        return;
    }
    // Pull the dragged ids out (keeping their relative order), then splice them
    // back in at the insertion point, adjusted for rows removed from above it.
    QVector<qint64> movingIds;
    QVector<qint64> rest;
    movingIds.reserve(moving.size());
    rest.reserve(count - moving.size());
    int removedBefore = 0;
    for (int i = 0; i < count; ++i) {
        if (moving.contains(i)) {
            movingIds.push_back(ids.at(i));
            if (i < destinationRow) {
                ++removedBefore;
            }
        } else {
            rest.push_back(ids.at(i));
        }
    }
    const int insertAt = std::clamp(destinationRow - removedBefore, 0, static_cast<int>(rest.size()));
    if (movingIds == ids.mid(insertAt, movingIds.size())) {
        return;  // already in place — nothing to do
    }

    QVector<qint64> reordered;
    reordered.reserve(count);
    reordered += rest.mid(0, insertAt);
    reordered += movingIds;
    reordered += rest.mid(insertAt);
    pushUndoSnapshot();
    if (!m_db->reorderItems(m_currentPlaylistId, reordered)) {
        QMessageBox::warning(this, QStringLiteral("Playlist"), m_db->lastError());
        return;
    }

    reloadItems();
    reloadPlaylists();
    // Park the cursor on the first moved row and select the moved block so it
    // stays visible and chainable.
    setCurrentItemRow(insertAt, 0);
    if (m_itemTable->selectionModel() != nullptr && movingIds.size() > 1) {
        const int lastRow = insertAt + static_cast<int>(movingIds.size()) - 1;
        QItemSelection selection(m_itemModel->index(insertAt, 0),
                                 m_itemModel->index(lastRow, m_itemModel->columnCount() - 1));
        m_itemTable->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
    }
}

void PlaylistView::updatePaneFocus()
{
    const bool itemActive = m_itemTable->hasFocus();
    m_itemTable->setMainPanelActive(itemActive);
    const bool listActive = !itemActive;
    for (int row = 0; row < m_playlistList->count(); ++row) {
        m_playlistList->item(row)->setData(PlaylistListActiveRole, listActive);
    }
    m_playlistList->viewport()->update();
}

void PlaylistView::applyPlaylistRowHeights()
{
    for (int row = 0; row < m_playlistList->count(); ++row) {
        QListWidgetItem *item = m_playlistList->item(row);
        if (item->data(PlaylistSpacerRole).toBool()) {
            item->setSizeHint(QSize(0, 0));
            continue;
        }
        if (item->data(PlaylistSeparatorRole).toBool()) {
            item->setSizeHint(QSize(0, 20));
            continue;
        }
        const bool hasMetadata = !item->data(PlaylistMetaRole).toString().isEmpty();
        item->setSizeHint(QSize(0, hasMetadata ? m_playlistRowHeight + 18 : m_playlistRowHeight));
    }
    updateSavedQueueSpacerHeight();
}

void PlaylistView::updateSavedQueueSpacerHeight()
{
    if (m_playlistList == nullptr) {
        return;
    }
    QListWidgetItem *spacer = nullptr;
    int usedHeight = 0;
    for (int row = 0; row < m_playlistList->count(); ++row) {
        QListWidgetItem *item = m_playlistList->item(row);
        if (item->data(PlaylistSpacerRole).toBool()) {
            spacer = item;
            continue;
        }
        usedHeight += std::max(0, item->sizeHint().height());
    }
    if (spacer == nullptr) {
        return;
    }
    const int extra = std::max(0, m_playlistList->viewport()->height() - usedHeight - 2);
    spacer->setSizeHint(QSize(0, extra));
}

QString PlaylistView::selectorMetadataForPlaylist(const Playlist &playlist) const
{
    switch (m_selectorMetadata) {
    case SelectorMetadata::CreatedAt:
        return timestampMetadata(playlist.createdAt, m_selectorDateFormat);
    case SelectorMetadata::UpdatedAt:
        return timestampMetadata(playlist.updatedAt, m_selectorDateFormat);
    case SelectorMetadata::Comment:
        return playlist.comment;
    case SelectorMetadata::None:
        break;
    }
    return {};
}

QString PlaylistView::selectorMetadataForSavedQueue(const SavedQueuePlaylistEntry &queue) const
{
    switch (m_selectorMetadata) {
    case SelectorMetadata::CreatedAt:
    case SelectorMetadata::UpdatedAt:
        return timestampMetadata(queue.savedAt, m_selectorDateFormat);
    case SelectorMetadata::Comment:
    case SelectorMetadata::None:
        break;
    }
    return {};
}

void PlaylistView::createPlaylist()
{
    if (m_db == nullptr) {
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("New playlist"),
                                               QStringLiteral("Name:"), QLineEdit::Normal,
                                               QStringLiteral("New playlist"), &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    const qint64 id = m_db->createPlaylist(name);
    if (id <= 0) {
        QMessageBox::warning(this, QStringLiteral("Playlist"), m_db->lastError());
        return;
    }
    m_currentPlaylistId = id;
    reloadPlaylists();
}

void PlaylistView::renameCurrentPlaylist()
{
    if (m_db == nullptr || m_currentPlaylistId <= 0) {
        return;
    }
    const Playlist playlist = m_db->playlist(m_currentPlaylistId);
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Rename playlist"),
                                               QStringLiteral("Name:"), QLineEdit::Normal,
                                               playlist.name, &ok)
                             .trimmed();
    if (!ok || name.isEmpty()) {
        return;
    }
    m_db->renamePlaylist(m_currentPlaylistId, name);
    reloadPlaylists();
}

void PlaylistView::editCurrentPlaylistComment()
{
    if (m_db == nullptr || m_currentPlaylistId <= 0) {
        return;
    }
    const Playlist playlist = m_db->playlist(m_currentPlaylistId);
    bool ok = false;
    const QString comment = QInputDialog::getMultiLineText(this, QStringLiteral("Playlist comment"),
                                                           QStringLiteral("Comment for \"%1\":").arg(playlist.name),
                                                           playlist.comment, &ok);
    if (!ok) {
        return;
    }
    m_db->setPlaylistComment(m_currentPlaylistId, comment.trimmed());
    reloadPlaylists();
}

void PlaylistView::editCurrentItemComment()
{
    const PlaylistItem *current = itemForDisplayRow(currentItemRow());
    if (m_db == nullptr || current == nullptr || m_currentPlaylistId <= 0) {
        return;
    }
    const QString label = current->titleSnapshot.isEmpty() ? current->trackPath : current->titleSnapshot;
    bool ok = false;
    const QString comment = QInputDialog::getMultiLineText(this, QStringLiteral("Item comment"),
                                                           QStringLiteral("Comment for \"%1\":").arg(label),
                                                           current->comment, &ok);
    if (!ok) {
        return;
    }
    PlaylistItem item = *current;
    item.comment = comment.trimmed();
    m_db->updateItem(item);
    reloadItems();
}

void PlaylistView::deleteCurrentPlaylist()
{
    if (m_db == nullptr || m_currentPlaylistId <= 0) {
        return;
    }
    const Playlist playlist = m_db->playlist(m_currentPlaylistId);
    if (QMessageBox::question(this, QStringLiteral("Delete playlist"),
                              QStringLiteral("Delete \"%1\"?").arg(playlist.name))
        != QMessageBox::Yes) {
        return;
    }
    m_db->deletePlaylist(m_currentPlaylistId);
    m_currentPlaylistId = 0;
    reloadPlaylists();
}

void PlaylistView::exportCurrentPlaylist()
{
    if (m_db == nullptr || m_currentPlaylistId <= 0) {
        return;
    }
    const Playlist playlist = m_db->playlist(m_currentPlaylistId);
    const QVector<PlaylistItem> items = m_db->items(m_currentPlaylistId);

    QString selectedFilter;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export playlist"),
        QStringLiteral("%1.m3u8").arg(playlist.name),
        QStringLiteral("M3U8 playlist (*.m3u8);;CSV (*.csv)"), &selectedFilter);
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return;
    }
    QTextStream out(&file);
    const bool csv = path.endsWith(QStringLiteral(".csv"), Qt::CaseInsensitive)
        || selectedFilter.contains(QStringLiteral("csv"), Qt::CaseInsensitive);

    if (csv) {
        const auto field = [](const QString &value) {
            QString escaped = value;
            escaped.replace(QLatin1Char('"'), QStringLiteral("\"\""));
            return QStringLiteral("\"%1\"").arg(escaped);
        };
        out << "ordinal,title,artist,album,duration_ms,path,status,query,comment,external_id,source_text\n";
        for (const PlaylistItem &item : items) {
            out << item.ordinal + 1 << ','
                << field(item.titleSnapshot) << ',' << field(item.artistSnapshot) << ','
                << field(item.albumSnapshot) << ',' << item.durationMs << ','
                << field(item.trackPath) << ',' << field(statusLabel(item.status)) << ','
                << field(item.query) << ',' << field(item.comment) << ','
                << field(item.externalId) << ',' << field(item.sourceText) << '\n';
        }
    } else {
        out << "#EXTM3U\n";
        for (const PlaylistItem &item : items) {
            if (item.status != PlaylistItemStatus::Matched || item.trackPath.isEmpty()) {
                continue;  // unresolved (pending/missing) rows have no playable path
            }
            const int seconds = static_cast<int>(item.durationMs / 1000);
            out << "#EXTINF:" << seconds << ',' << item.artistSnapshot << " - "
                << item.titleSnapshot << '\n' << item.trackPath << '\n';
        }
    }
}

void PlaylistView::removeSelectedItems()
{
    removeSelectedItemsImpl(/*missingOnly=*/false);
}

void PlaylistView::removeSelectedMissingItems()
{
    removeSelectedItemsImpl(/*missingOnly=*/true);
}

void PlaylistView::removeSelectedItemsImpl(bool missingOnly)
{
    if (m_db == nullptr || m_currentPlaylistId <= 0 || m_itemTable->selectionModel() == nullptr) {
        return;
    }
    QVector<int> rows;
    for (const QModelIndex &index : m_itemTable->selectionModel()->selectedRows(0)) {
        rows.push_back(index.row());
    }
    if (rows.isEmpty() && currentItemRow() >= 0) {
        rows.push_back(currentItemRow());
    }
    // Resolve to item ids up front (display rows may be sorted), then delete by
    // descending ordinal so the per-removal ordinal compaction stays valid.
    // When missingOnly, keep just the broken rows so a rough range-select drops
    // only the missing entries and leaves the playable ones in place.
    QVector<PlaylistItem> targets;
    for (int row : rows) {
        if (const PlaylistItem *item = itemForDisplayRow(row)) {
            if (missingOnly && item->status != PlaylistItemStatus::Missing) {
                continue;
            }
            targets.push_back(*item);
        }
    }
    if (targets.isEmpty()) {
        return;
    }
    std::sort(targets.begin(), targets.end(),
              [](const PlaylistItem &a, const PlaylistItem &b) { return a.ordinal > b.ordinal; });
    pushUndoSnapshot();
    for (const PlaylistItem &item : targets) {
        m_db->removeItem(item.id);
    }
    reloadItems();
    reloadPlaylists();
}

// NOTE: the Keybinds-dialog reference for this view lives in
// ui/ViewKeyReferences.cpp — update it when changing keys here.
bool PlaylistView::eventFilter(QObject *watched, QEvent *event)
{
    const bool playlistListWatched = m_playlistList != nullptr
        && (watched == m_playlistList || watched == m_playlistList->viewport());
    const bool itemTableWatched = m_itemTable != nullptr
        && (watched == m_itemTable || watched == m_itemTable->viewport());
    if ((watched == m_playlistList || watched == m_itemTable) && event->type() == QEvent::FocusIn) {
        updatePaneFocus();
        return QWidget::eventFilter(watched, event);
    }
    if ((watched == m_playlistList || watched == m_itemTable) && event->type() == QEvent::FocusOut) {
        updatePaneFocus();
        return QWidget::eventFilter(watched, event);
    }
    if (itemTableWatched && event->type() == QEvent::MouseMove) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        const QModelIndex index = m_itemTable->indexAt(mouse->pos());
        setHoveredItemRow(index.isValid() ? index.row() : -1);
        const QModelIndex ratingIndex = index.isValid() && index.column() == RatingColumn ? index : QModelIndex();
        if (m_hoverRatingIndex.isValid() && m_hoverRatingIndex != ratingIndex) {
            m_itemModel->setData(m_hoverRatingIndex, StarRating::unset, HoverRatingRole);
        }
        m_hoverRatingIndex = ratingIndex;
        return QWidget::eventFilter(watched, event);
    }
    if (itemTableWatched && event->type() == QEvent::Leave) {
        setHoveredItemRow(-1);
        if (m_hoverRatingIndex.isValid()) {
            m_itemModel->setData(m_hoverRatingIndex, StarRating::unset, HoverRatingRole);
            m_hoverRatingIndex = QModelIndex();
        }
        return QWidget::eventFilter(watched, event);
    }
    if (event->type() == QEvent::Wheel) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        if (playlistListWatched
            && ui::applyCtrlWheelRowHeight(wheel, m_playlistRowHeight, 18, 48, [this](int h) {
                   m_playlistRowHeight = h;
                   applyPlaylistRowHeights();
                   emit viewSettingsChanged();
               })) {
            return true;
        }
        if (itemTableWatched
            && ui::applyCtrlWheelRowHeight(wheel, m_itemTable->verticalHeader()->defaultSectionSize(), 20, 48,
                   [this](int h) {
                       m_itemTable->verticalHeader()->setDefaultSectionSize(h);
                       emit viewSettingsChanged();
                   })) {
            return true;
        }
    }
    if (playlistListWatched && event->type() == QEvent::Resize) {
        updateSavedQueueSpacerHeight();
        return QWidget::eventFilter(watched, event);
    }
    if (event->type() != QEvent::KeyPress) {
        return QWidget::eventFilter(watched, event);
    }
    auto *ke = static_cast<QKeyEvent *>(event);
    const int key = ke->key();
    const Qt::KeyboardModifiers mods = ke->modifiers();

    // Esc / C-g interrupts a running background drop-import (and only then, so it
    // doesn't swallow Escape the rest of the time).
    if ((key == Qt::Key_Escape || (mods == Qt::ControlModifier && key == Qt::Key_G))
        && !m_importingPlaylists.isEmpty()) {
        emit importInterruptRequested();
        return true;
    }

    if (watched == m_playlistList) {
        if (mods == Qt::ControlModifier && key == Qt::Key_D) {
            deleteCurrentPlaylist();
            return true;
        }
        if (mods == Qt::NoModifier && key == Qt::Key_U) {
            undoLastChange();
            return true;
        }
        switch (key) {
        case Qt::Key_J:
        case Qt::Key_N:
            m_playlistList->setCurrentRow(nextSelectablePlaylistRow(
                m_playlistList, std::min(m_playlistList->count() - 1, m_playlistList->currentRow() + 1), +1));
            return true;
        case Qt::Key_K:
        case Qt::Key_P:
            m_playlistList->setCurrentRow(nextSelectablePlaylistRow(
                m_playlistList, std::max(0, m_playlistList->currentRow() - 1), -1));
            return true;
        case Qt::Key_L:
        case Qt::Key_Right:
            if (m_currentPlaylistId > 0 || currentSelectionIsSavedQueue()) {
                m_itemTable->setFocus(Qt::OtherFocusReason);
                if (currentItemRow() < 0 && m_itemModel->rowCount() > 0) {
                    setCurrentItemRow(0);
                }
            }
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            playCurrentPlaylist();
            return true;
        case Qt::Key_Space:
            addCurrentPlaylistToQueue();
            return true;
        case Qt::Key_Equal:
        case Qt::Key_Plus:
            addSongToCurrentPlaylist();
            return true;
        case Qt::Key_Insert:
        case Qt::Key_A:
            createPlaylist();
            return true;
        case Qt::Key_T:
            m_selectorMetadata = m_selectorMetadata == SelectorMetadata::None
                ? SelectorMetadata::CreatedAt
                : SelectorMetadata::None;
            reloadPlaylists();
            emit viewSettingsChanged();
            return true;
        case Qt::Key_R:
        case Qt::Key_F2:
            renameCurrentPlaylist();
            return true;
        case Qt::Key_C:
            editCurrentPlaylistComment();
            return true;
        case Qt::Key_Delete:
            deleteCurrentPlaylist();
            return true;
        case Qt::Key_X:
            exportCurrentPlaylist();
            return true;
        case Qt::Key_I:
            importIntoCurrentPlaylist();
            return true;
        default:
            break;
        }
        return QWidget::eventFilter(watched, event);
    }

    if (watched == m_itemTable) {
        // Shift+n/p (and the hjkl-equivalent Shift+j/k) reorder the selection.
        // Arrow variants are intentionally omitted — Shift+Arrow is the platform
        // selection-extend gesture and rebinding it would surprise.
        if (mods == Qt::ShiftModifier && (key == Qt::Key_N || key == Qt::Key_J)) {
            moveSelectedItems(+1);
            return true;
        }
        if (mods == Qt::ShiftModifier && (key == Qt::Key_P || key == Qt::Key_K)) {
            moveSelectedItems(-1);
            return true;
        }
        if (mods == Qt::ControlModifier && key == Qt::Key_D) {
            removeSelectedItems();
            return true;
        }
        if (mods == Qt::NoModifier && key == Qt::Key_U) {
            undoLastChange();
            return true;
        }
        switch (key) {
        case Qt::Key_J:
        case Qt::Key_N:
            setCurrentItemRow(std::min(m_itemModel->rowCount() - 1, currentItemRow() + 1), +1);
            return true;
        case Qt::Key_K:
        case Qt::Key_P:
            setCurrentItemRow(std::max(0, currentItemRow() - 1), -1);
            return true;
        case Qt::Key_H:
        case Qt::Key_Left:
            focusPlaylistList();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter: {
            playCurrentItem();
            return true;
        }
        case Qt::Key_Space: {
            addSelectedItemsToQueue();
            return true;
        }
        case Qt::Key_Equal:  // = and + (shift+=) both open the add-song modal
        case Qt::Key_Plus:
            addSongToCurrentPlaylist();
            return true;
        case Qt::Key_A: {
            addSelectedItemsToPlaylist();
            return true;
        }
        case Qt::Key_E: {
            editCurrentItem();
            return true;
        }
        case Qt::Key_C:
            editCurrentItemComment();
            return true;
        case Qt::Key_Delete:
            removeSelectedItems();
            return true;
        case Qt::Key_S:
            if (mods == Qt::NoModifier) {
                cycleAddedSort();
                return true;
            }
            break;
        case Qt::Key_I:
            if (mods == Qt::NoModifier) {
                const PlaylistItem *item = itemForDisplayRow(currentItemRow());
                if (item != nullptr && item->status == PlaylistItemStatus::Matched && !item->trackPath.isEmpty()) {
                    emit propertiesForPathRequested(item->trackPath);
                }
                return true;
            }
            break;
        default:
            break;
        }
        return QWidget::eventFilter(watched, event);
    }

    return QWidget::eventFilter(watched, event);
}

void PlaylistView::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::StyleChange) {
        restylePanelBorders();
        if (m_playlistList != nullptr) {
            m_playlistList->viewport()->update();
        }
        if (m_itemTable != nullptr) {
            applyHeaderViewStyle(m_itemTable->horizontalHeader(), kTableHeaderStyle);
            m_itemTable->viewport()->update();
            m_itemTable->horizontalHeader()->viewport()->update();
        }
    }
}

namespace {
// Local importable files (extensions PlaylistImport::parseFile understands).
QStringList importableDroppedPaths(const QMimeData *mime)
{
    QStringList paths;
    if (mime == nullptr || !mime->hasUrls()) {
        return paths;
    }
    static const QStringList exts = {QStringLiteral("jsonl"), QStringLiteral("ndjson"),
                                     QStringLiteral("json"),  QStringLiteral("m3u"),
                                     QStringLiteral("m3u8"),  QStringLiteral("csv"),
                                     QStringLiteral("txt")};
    for (const QUrl &url : mime->urls()) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QString local = url.toLocalFile();
        if (exts.contains(QFileInfo(local).suffix().toLower())) {
            paths << local;
        }
    }
    return paths;
}
} // namespace

void PlaylistView::dragEnterEvent(QDragEnterEvent *event)
{
    if (!importableDroppedPaths(event->mimeData()).isEmpty()) {
        event->acceptProposedAction();
    }
}

void PlaylistView::dropEvent(QDropEvent *event)
{
    const QStringList paths = importableDroppedPaths(event->mimeData());
    if (paths.isEmpty()) {
        return;
    }
    event->acceptProposedAction();
    emit playlistFilesDropped(paths);
}

void PlaylistView::restylePanelBorders()
{
    if (m_splitter != nullptr) {
        applyPanelBorderStyleSheet(m_splitter,
                                   QStringLiteral("QSplitter#PlaylistFrame"),
                                   panelAllBorders());
    }
    if (m_playlistList != nullptr) {
        const QString style = panelBorderStyleSheet(QStringLiteral("QListWidget#PlaylistList"),
                                                    panelRightBorder(),
                                                    m_playlistList);
        if (m_playlistList->styleSheet() != style) {
            m_playlistList->setStyleSheet(style);
        }
    }
    if (m_itemTable != nullptr) {
        m_itemTable->setPanelBorders(panelNoBorders());
    }
}
