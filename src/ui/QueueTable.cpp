#include "ui/QueueTable.h"

#include "ui/NeighborColumnResizer.h"
#include "ui/NavigableTableView.h"
#include "ui/OverlayScrollBar.h"
#include "ui/QueueKeybindings.h"
#include "ui/QueueStore.h"
#include "ui/ResponsiveColumnLayout.h"
#include "ui/ResponsiveColumnOptionsDialog.h"
#include "ui/SelectionColors.h"
#include "ui/StarRating.h"
#include "ui/StarRatingDelegate.h"

#include <QAbstractItemView>
#include <QAbstractTableModel>
#include <QAction>
#include <QActionGroup>
#include <QDataStream>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCursor>
#include <QKeyEvent>
#include <QLabel>
#include <QLine>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <functional>

namespace {

struct ColumnSpec {
    const char *key;
    const char *label;
    int index;
    int sidebarPreferredWidth;
    int fullScreenWeight;
    int minWidth = 24;
};

constexpr ColumnSpec columns[] = {
    {"position", "#", 0, 38, 4},
    {"title", "Title", 1, 180, 94, 140},
    {"ratingEdit", "Rating", 2, 96, 7, 56},
    {"rating", "Rating (short)", 3, 56, 6, 12},
    {"artist", "Artist", 4, 120, 27},
    {"album", "Album", 5, 120, 59},
    {"duration", "Duration", 6, 70, 5, 24},
    {"year", "Year", 7, 58, 5},
    {"track", "Track", 8, 36, 13, 12},
};

constexpr auto queueRowsMimeType = "application/x-muzaiten-queue-rows";
constexpr int kCellHorizontalPadding = 3;

enum QueueRoles {
    TrackRole = Qt::UserRole + 1,
    HoverRatingRole = Qt::UserRole + 2,
    PlayNextOrdinalRole = Qt::UserRole + 3,
};

const ColumnSpec *specForColumn(int column)
{
    for (const ColumnSpec &spec : columns) {
        if (spec.index == column) {
            return &spec;
        }
    }
    return nullptr;
}

int minWidthForColumn(int column)
{
    if (const ColumnSpec *spec = specForColumn(column)) {
        return spec->minWidth;
    }
    return 24;
}

int preferredWidthForColumn(int column, QueueTablePreset preset)
{
    if (const ColumnSpec *spec = specForColumn(column)) {
        return preset == QueueTablePreset::FullScreen
            ? std::max(spec->minWidth, spec->fullScreenWeight * 2)
            : spec->sidebarPreferredWidth;
    }
    return 60;
}

QString columnKey(int column)
{
    if (const ColumnSpec *spec = specForColumn(column)) {
        return QString::fromLatin1(spec->key);
    }
    return {};
}

ResponsiveColumnPriority defaultPriorityForColumn(int column)
{
    switch (column) {
    case 1:
        return ResponsiveColumnPriority::Keep;
    case 4:
    case 5:
    case 2:
        return ResponsiveColumnPriority::Normal;
    default:
        return ResponsiveColumnPriority::HideEarly;
    }
}

QVector<int> queueResponsiveDropOrder()
{
    return {7, 0, 3, 8, 6, 2, 5, 4, 1};
}

QVector<ResponsiveColumnSpec> responsiveSpecsForPreset(QueueTablePreset preset)
{
    QVector<ResponsiveColumnSpec> specs;
    for (int column : queueResponsiveDropOrder()) {
        if (const ColumnSpec *spec = specForColumn(column)) {
            specs.push_back({
                spec->index,
                QString::fromLatin1(spec->key),
                preferredWidthForColumn(spec->index, preset),
                minWidthForColumn(spec->index),
                defaultPriorityForColumn(spec->index),
                spec->index == 1,
            });
        }
    }
    return specs;
}

QVector<int> defaultVisibleColumnsForPreset(QueueTablePreset preset)
{
    return preset == QueueTablePreset::FullScreen
        ? QVector<int>{4, 8, 1, 5, 6, 2}
        : QVector<int>{0, 1, 3};
}

QSet<QString> defaultVisibleKeysForPreset(QueueTablePreset preset)
{
    QSet<QString> keys;
    for (int column : defaultVisibleColumnsForPreset(preset)) {
        keys.insert(columnKey(column));
    }
    return keys;
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

QVector<ResponsiveColumnOption> responsiveColumnOptions()
{
    QVector<ResponsiveColumnOption> options;
    for (const ColumnSpec &spec : columns) {
        options.push_back({QString::fromLatin1(spec.key), QString::fromLatin1(spec.label)});
    }
    return options;
}

QString ratingText(int rating0To100)
{
    if (rating0To100 < 0) {
        return {};
    }
    return QStringLiteral("%1 %2").arg(rating0To100 / 20.0, 0, 'f', 1).arg(QChar(0x2605));
}

QString formatDuration(qint64 durationMs)
{
    if (durationMs <= 0) {
        return {};
    }

    const qint64 totalSeconds = durationMs / 1000;
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}

QString trackNumberText(const Track &track)
{
    if (track.trackNumber > 0) {
        return track.discNumber > 1
            ? QStringLiteral("%1.%2").arg(track.discNumber).arg(track.trackNumber, 2, 10, QLatin1Char('0'))
            : QString::number(track.trackNumber);
    }

    const QString base = QFileInfo(track.filename.isEmpty() ? track.path : track.filename).completeBaseName();
    int end = 0;
    while (end < base.size() && base.at(end).isDigit()) {
        ++end;
    }
    if (end > 0) {
        return base.left(end);
    }
    return {};
}

QString displayYear(const Track &track)
{
    for (const QString &candidate : {track.originalDate, track.date}) {
        const QString trimmed = candidate.trimmed();
        if (!trimmed.isEmpty()) {
            return trimmed.left(4);
        }
    }
    return {};
}

QString displayTitle(const Track &track)
{
    if (!track.title.trimmed().isEmpty()) {
        return track.title;
    }
    const QString file = track.filename.isEmpty() ? track.path : track.filename;
    const QString base = QFileInfo(file).completeBaseName();
    return base.isEmpty() ? file : base;
}

QString displayArtist(const Track &track)
{
    return track.albumArtistName.trimmed().isEmpty() ? track.artistName : track.albumArtistName;
}

QKeySequence keySequenceForEvent(QKeyEvent *event)
{
    const Qt::KeyboardModifiers mods = event->modifiers()
        & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    return QKeySequence(static_cast<int>(mods) | event->key());
}

class QueueTableView final : public NavigableTableView {
public:
    explicit QueueTableView(QWidget *parent = nullptr)
        : NavigableTableView(parent)
    {
    }

    void setPreset(QueueTablePreset preset)
    {
        m_preset = preset;
    }

    void setCurrentPlayingRow(int row)
    {
        if (m_currentPlayingRow == row) {
            return;
        }
        m_currentPlayingRow = row;
        viewport()->update();
    }

    int currentPlayingRow() const
    {
        return m_currentPlayingRow;
    }

    std::function<void(const QVector<int> &rows, int destinationRow)> rowsMoveRequested;

protected:
    bool isQueueDrag(const QMimeData *mime) const
    {
        return mime != nullptr && mime->hasFormat(QString::fromLatin1(queueRowsMimeType));
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (isQueueDrag(event->mimeData())) {
            event->setDropAction(Qt::MoveAction);
            event->accept();
        } else {
            NavigableTableView::dragEnterEvent(event);
        }
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (isQueueDrag(event->mimeData())) {
            event->setDropAction(Qt::MoveAction);
            event->accept();
        } else {
            NavigableTableView::dragMoveEvent(event);
        }
        setDropIndicatorRow(rowForDropPosition(event->position().toPoint()));
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        NavigableTableView::dragLeaveEvent(event);
        setDropIndicatorRow(-1);
    }

    void dropEvent(QDropEvent *event) override
    {
        if (isQueueDrag(event->mimeData())) {
            QVector<int> rows;
            QByteArray payload = event->mimeData()->data(QString::fromLatin1(queueRowsMimeType));
            QDataStream stream(&payload, QIODevice::ReadOnly);
            stream >> rows;
            if (rowsMoveRequested) {
                rowsMoveRequested(rows, rowForDropPosition(event->position().toPoint()));
            }
            event->acceptProposedAction();
        } else {
            NavigableTableView::dropEvent(event);
        }
        setDropIndicatorRow(-1);
    }

    void paintEvent(QPaintEvent *event) override
    {
        NavigableTableView::paintEvent(event);
        if (model() == nullptr) {
            return;
        }

        QPainter painter(viewport());
        if (m_currentPlayingRow >= 0 && m_currentPlayingRow < model()->rowCount()) {
            const int y = rowViewportPosition(m_currentPlayingRow);
            const int h = rowHeight(m_currentPlayingRow);
            if (h > 0 && y + h > 0 && y < viewport()->height()) {
                const QColor color = palette().color(QPalette::Highlight);
                painter.fillRect(QRect(0, y, 3, h), color);
            }
        }

        if (m_dropIndicatorRow >= 0) {
            const int y = yForDropRow(m_dropIndicatorRow);
            QColor color = palette().color(QPalette::Highlight);
            color.setAlpha(210);
            QPen pen(color, 2);
            painter.setPen(pen);
            painter.drawLine(QLine(0, y, viewport()->width(), y));
        }
    }

private:
    int rowForDropPosition(const QPoint &pos) const
    {
        const QModelIndex index = indexAt(pos);
        if (!index.isValid()) {
            return model() == nullptr ? -1 : model()->rowCount();
        }
        const QRect rect = visualRect(index);
        return pos.y() < rect.center().y() ? index.row() : index.row() + 1;
    }

    int yForDropRow(int row) const
    {
        if (model() == nullptr || model()->rowCount() == 0) {
            return 0;
        }
        const int lastRow = model()->rowCount() - 1;
        if (row > lastRow) {
            return rowViewportPosition(lastRow) + rowHeight(lastRow);
        }
        return rowViewportPosition(row);
    }

    void setDropIndicatorRow(int row)
    {
        if (m_dropIndicatorRow == row) {
            return;
        }
        m_dropIndicatorRow = row;
        viewport()->update();
    }

    QueueTablePreset m_preset = QueueTablePreset::Sidebar;
    int m_dropIndicatorRow = -1;
    int m_currentPlayingRow = -1;
};

class QueueItemDelegate final : public QStyledItemDelegate {
public:
    explicit QueueItemDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    void setHoveredRow(int row)
    {
        m_hoveredRow = row;
    }

    void setShowTitleAccent(bool show)
    {
        m_showTitleAccent = show;
    }

    void setCurrentRow(int row)
    {
        m_currentRow = row;
    }

    void setForcePlayingHighlight(bool force)
    {
        m_forcePlayingHighlight = force;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);

        const bool selected = opt.state & QStyle::State_Selected;
        // Hover is strictly row-based (driven by m_hoveredRow, which the view
        // keeps in sync on mouse-move and scroll) — never the single cell under
        // the pointer, so the whole row always highlights.
        const bool hovered = (m_hoveredRow == index.row());
        const bool playing = index.row() == m_currentRow;

        const bool forcePlayingHighlight = playing && m_forcePlayingHighlight;

        if (forcePlayingHighlight) {
            QColor tint = opt.palette.color(QPalette::Highlight);
            tint.setAlpha(48);
            painter->fillRect(opt.rect, tint);
        } else if (selected) {
            painter->fillRect(opt.rect, SelectionColors::selectedFill(opt));
        } else if (hovered) {
            QColor hover = opt.palette.color(QPalette::Highlight);
            hover.setAlpha(34);
            painter->fillRect(opt.rect, hover);
        } else if (playing) {
            QColor tint = opt.palette.color(QPalette::Highlight);
            tint.setAlpha(48);
            painter->fillRect(opt.rect, tint);
        } else if (index.row() % 2 == 1) {
            painter->fillRect(opt.rect, opt.palette.color(QPalette::AlternateBase));
        }

        if (selected && !forcePlayingHighlight) {
            SelectionColors::applySelectedPalette(&opt);
        } else {
            opt.state &= ~QStyle::State_MouseOver;
        }
        opt.state &= ~QStyle::State_MouseOver;
        opt.state &= ~QStyle::State_Selected;
        opt.features &= ~QStyleOptionViewItem::Alternate;
        opt.backgroundBrush = Qt::NoBrush;
        opt.displayAlignment = opt.displayAlignment | Qt::AlignVCenter;
        if (index.column() == 6 || index.column() == 8) {
            opt.displayAlignment = Qt::AlignRight | Qt::AlignVCenter;
        }
        opt.rect.adjust(kCellHorizontalPadding, 0, -kCellHorizontalPadding, 0);
        QStyledItemDelegate::paint(painter, opt, index);

        if (m_showTitleAccent && index.column() == 1) {
            const int ordinal = index.data(PlayNextOrdinalRole).toInt();
            if (ordinal > 0) {
                painter->save();
                QColor color = selected ? SelectionColors::selectedText(option) : option.palette.color(QPalette::Highlight);
                color.setAlpha(selected ? 160 : 130);
                painter->setPen(color);
                QFont f = painter->font();
                f.setPointSizeF(f.pointSizeF() * 0.8);
                painter->setFont(f);
                painter->drawText(opt.rect.adjusted(0, 0, -4, 0),
                                  Qt::AlignRight | Qt::AlignVCenter,
                                  QString::number(ordinal));
                painter->restore();
            }
        }
    }

private:
    int m_hoveredRow = -1;
    int m_currentRow = -1;
    bool m_showTitleAccent = false;
    bool m_forcePlayingHighlight = false;
};

class QueueTableModel final : public QAbstractTableModel {
public:
    explicit QueueTableModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent)
    {
    }

    void setStore(QueueStore *store)
    {
        if (m_store == store) {
            return;
        }
        if (m_store != nullptr) {
            disconnect(m_store, nullptr, this, nullptr);
        }
        m_store = store;
        if (m_store != nullptr) {
            connect(m_store, &QueueStore::tracksAboutToReset, this, [this]() { beginResetModel(); });
            connect(m_store, &QueueStore::tracksReset, this, [this]() {
                m_hoverRatings.fill(StarRating::unset, rowCount());
                endResetModel();
            });
            connect(m_store, &QueueStore::playNextRangeChanged, this, [this]() {
                if (rowCount() > 0) {
                    emit dataChanged(index(0, 0), index(rowCount() - 1, 1));
                }
            });
        }
        beginResetModel();
        m_hoverRatings.fill(StarRating::unset, rowCount());
        endResetModel();
    }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() || m_store == nullptr ? 0 : static_cast<int>(m_store->tracks().size());
    }

    int columnCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : 9;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return {};
        }
        if (const ColumnSpec *spec = specForColumn(section)) {
            return QString::fromLatin1(spec->label);
        }
        return {};
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (m_store == nullptr || !index.isValid() || index.row() < 0 || index.row() >= m_store->tracks().size()) {
            return {};
        }

        const Track &track = m_store->tracks().at(index.row());
        if (role == TrackRole) {
            return QVariant::fromValue(track);
        }
        if (role == PlayNextOrdinalRole) {
            return playNextOrdinalForRow(index.row());
        }
        if (index.column() == 2 && role == Qt::UserRole) {
            return track.effectiveRating0To100;
        }
        if (index.column() == 2 && role == HoverRatingRole) {
            return m_hoverRatings.value(index.row(), StarRating::unset);
        }
        if (role == Qt::TextAlignmentRole && (index.column() == 6 || index.column() == 8)) {
            return QVariant::fromValue(Qt::Alignment(Qt::AlignRight | Qt::AlignVCenter));
        }
        if (role != Qt::DisplayRole) {
            return {};
        }

        switch (index.column()) {
        case 0: {
            if (m_showPlayNextBadge) {
                const int ordinal = playNextOrdinalForRow(index.row());
                if (ordinal > 0) {
                    return QStringLiteral("▸%1").arg(ordinal);
                }
            }
            return QString::number(index.row() + 1);
        }
        case 1:
            return displayTitle(track);
        case 2:
            return {};
        case 3:
            return ratingText(track.effectiveRating0To100);
        case 4:
            return displayArtist(track);
        case 5:
            return track.albumTitle;
        case 6:
            return formatDuration(track.durationMs);
        case 7:
            return displayYear(track);
        case 8:
            return trackNumberText(track);
        default:
            return {};
        }
    }

    bool setData(const QModelIndex &index, const QVariant &value, int role) override
    {
        if (!index.isValid() || index.column() != 2 || role != HoverRatingRole || index.row() >= m_hoverRatings.size()) {
            return false;
        }
        m_hoverRatings[index.row()] = value.toInt();
        emit dataChanged(index, index, {HoverRatingRole});
        return true;
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
        return {QString::fromLatin1(queueRowsMimeType)};
    }

    QMimeData *mimeData(const QModelIndexList &indexes) const override
    {
        auto *mime = new QMimeData;
        QVector<int> rows;
        rows.reserve(indexes.size());
        for (const QModelIndex &index : indexes) {
            if (index.isValid() && !rows.contains(index.row())) {
                rows.push_back(index.row());
            }
        }
        std::sort(rows.begin(), rows.end());

        QByteArray payload;
        QDataStream stream(&payload, QIODevice::WriteOnly);
        stream << rows;
        mime->setData(QString::fromLatin1(queueRowsMimeType), payload);
        return mime;
    }

    bool dropMimeData(const QMimeData *data, Qt::DropAction action, int row, int, const QModelIndex &parent) override
    {
        if (action != Qt::MoveAction || data == nullptr || !data->hasFormat(QString::fromLatin1(queueRowsMimeType))) {
            return false;
        }

        QVector<int> rows;
        QByteArray payload = data->data(QString::fromLatin1(queueRowsMimeType));
        QDataStream stream(&payload, QIODevice::ReadOnly);
        stream >> rows;
        int destinationRow = row;
        if (destinationRow < 0 && parent.isValid()) {
            destinationRow = parent.row();
        }
        if (destinationRow < 0) {
            destinationRow = rowCount();
        }
        if (rowsMoveRequested) {
            rowsMoveRequested(rows, destinationRow);
        }
        return true;
    }

    void setShowPlayNextBadge(bool show)
    {
        if (m_showPlayNextBadge == show) {
            return;
        }
        m_showPlayNextBadge = show;
        if (rowCount() > 0) {
            emit dataChanged(index(0, 0), index(rowCount() - 1, 0), {Qt::DisplayRole});
        }
    }

    std::function<void(const QVector<int> &rows, int destinationRow)> rowsMoveRequested;

private:
    int playNextOrdinalForRow(int row) const
    {
        if (m_store == nullptr || m_store->playNextBegin() < 0 || m_store->playNextEnd() <= m_store->playNextBegin()
            || row < m_store->playNextBegin() || row >= m_store->playNextEnd()) {
            return 0;
        }
        return row - m_store->playNextBegin() + 1;
    }

    QueueStore *m_store = nullptr;
    QVector<int> m_hoverRatings;
    bool m_showPlayNextBadge = true;
};

} // namespace

QueueTable::QueueTable(QueueTablePreset preset, QWidget *parent)
    : QWidget(parent)
    , m_preset(preset)
    , m_keyBindings(queueBindingMapForProfile(defaultQueueKeyBindingProfileName()))
    , m_keyBindingProfileName(defaultQueueKeyBindingProfileName())
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *queueModel = new QueueTableModel(this);
    auto *queueView = new QueueTableView(this);
    m_model = queueModel;
    m_view = queueView;
    queueView->setPreset(preset);
    m_view->setModel(m_model);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setDragEnabled(true);
    m_view->setAcceptDrops(true);
    m_view->setDropIndicatorShown(false);
    m_view->setDragDropMode(QAbstractItemView::InternalMove);
    m_view->setDefaultDropAction(Qt::MoveAction);
    m_view->setDragDropOverwriteMode(false);
    m_itemDelegate = new QueueItemDelegate(this);
    static_cast<QueueItemDelegate *>(m_itemDelegate)->setForcePlayingHighlight(preset == QueueTablePreset::Sidebar);
    m_view->setItemDelegate(m_itemDelegate);
    m_ratingDelegate = new StarRatingDelegate(this);
    m_view->setItemDelegateForColumn(2, m_ratingDelegate);
    m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_view->setShowGrid(false);
    m_view->setWordWrap(false);
    m_view->setMouseTracking(true);
    m_view->viewport()->setMouseTracking(true);
    // A scroll slides a different row under a stationary cursor with no
    // mouse-move; re-derive the hovered row from the cursor on every scroll.
    connect(queueView, &NavigableTableView::contentsScrolled, this, &QueueTable::updateHoverFromCursor);
    m_view->verticalHeader()->setVisible(false);
    m_view->verticalHeader()->setDefaultSectionSize(preset == QueueTablePreset::FullScreen ? 20 : 18);
    m_view->verticalHeader()->setMinimumSectionSize(preset == QueueTablePreset::FullScreen ? 20 : 18);
    m_view->horizontalHeader()->setFixedHeight(20);
    m_view->horizontalHeader()->setMinimumSectionSize(8);
    m_view->horizontalHeader()->setSectionsMovable(true);
    m_view->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->horizontalHeader()->setStretchLastSection(false);
    m_view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_view->setAlternatingRowColors(true);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->installEventFilter(this);
    m_view->viewport()->installEventFilter(this);
    if (preset == QueueTablePreset::FullScreen) {
        queueView->setMainPanelActive(true);
    }
    layout->addWidget(m_view, 1);

    if (preset == QueueTablePreset::FullScreen) {
        m_searchBar = new QWidget(this);
        auto *searchLayout = new QHBoxLayout(m_searchBar);
        searchLayout->setContentsMargins(8, 4, 8, 4);
        searchLayout->setSpacing(6);
        m_searchPrompt = new QLabel(QStringLiteral("Search Queue:"), m_searchBar);
        m_searchEdit = new QLineEdit(m_searchBar);
        m_searchEdit->setClearButtonEnabled(true);
        m_searchStatus = new QLabel(m_searchBar);
        m_searchStatus->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        searchLayout->addWidget(m_searchPrompt);
        searchLayout->addWidget(m_searchEdit, 1);
        searchLayout->addWidget(m_searchStatus);
        m_searchBar->setVisible(false);
        m_searchEdit->installEventFilter(this);
        connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
            setSearchQuery(text);
        });
        layout->addWidget(m_searchBar);
    }

    m_columnLayout = new ResponsiveColumnLayout(m_view, responsiveSpecsForPreset(preset), this);
    applyPresetDefaults();

    connect(m_view, &QTableView::doubleClicked, this, [this](const QModelIndex &index) {
        if (index.isValid()) {
            emit trackActivated(index.row());
        }
    });
    connect(m_view->horizontalHeader(), &QHeaderView::customContextMenuRequested, this, &QueueTable::showHeaderMenu);
    connect(m_view->horizontalHeader(), &QHeaderView::sectionMoved, this, [this]() {
        emit viewSettingsChanged();
    });
    auto *queueColumnResizer = NeighborColumnResizer::install(
        m_view->horizontalHeader(), [](int column) { return minWidthForColumn(column); });
    connect(queueColumnResizer, qOverload<int, int>(&NeighborColumnResizer::columnResized), this, [this](int leftLogical, int rightLogical) {
        m_columnLayout->updateBaselineWidthsForResize(leftLogical, rightLogical);
    });
    connect(m_columnLayout, &ResponsiveColumnLayout::layoutSettingsChanged, this, [this]() {
        emit viewSettingsChanged();
    });
    connect(m_view->verticalScrollBar(), &QScrollBar::rangeChanged, this, [this](int, int) {
        m_columnLayout->relayout();
        scheduleRestoreScrollToCurrentRow();
    });
    connect(m_view, &QWidget::customContextMenuRequested, this, &QueueTable::showQueueMenu);
    queueView->rowsMoveRequested = [this](const QVector<int> &rows, int destinationRow) {
        emit rowsMoveRequested(rows, destinationRow);
    };
    queueModel->rowsMoveRequested = [this](const QVector<int> &rows, int destinationRow) {
        emit rowsMoveRequested(rows, destinationRow);
    };
    connect(m_ratingDelegate, &StarRatingDelegate::ratingEdited, this, [this](const QModelIndex &index, int rating) {
        const Track track = index.data(TrackRole).value<Track>();
        if (!track.path.isEmpty()) {
            emit trackRatingChanged(track, rating);
        }
    });
    OverlayScrollBar::install(m_view);
}

void QueueTable::setQueueStore(QueueStore *store)
{
    if (m_store == store) {
        return;
    }
    if (m_store != nullptr) {
        disconnect(m_store, nullptr, this, nullptr);
    }
    m_store = store;
    static_cast<QueueTableModel *>(m_model)->setStore(store);
    if (m_store == nullptr) {
        return;
    }
    connect(m_store, &QueueStore::tracksReset, this, [this]() {
        const int row = currentRow();
        if (row < 0 && rowCount() > 0) {
            setCurrentRow(std::max(0, m_store->currentIndex()));
        }
        QTimer::singleShot(0, this, [this]() {
            if (m_preset == QueueTablePreset::Sidebar || m_view->isVisible()) {
                m_columnLayout->relayout();
            }
            scheduleRestoreScrollToCurrentRow();
        });
        // Recompute matches against the new queue contents without yanking the
        // cursor; the status line stays accurate while a search is open.
        if (m_searchBar != nullptr && m_searchBar->isVisible() && !m_searchQuery.isEmpty()) {
            rebuildSearchMatches(false);
        }
    });
    connect(m_store, &QueueStore::currentIndexChanged, this, [this](int index) {
        static_cast<QueueTableView *>(m_view)->setCurrentPlayingRow(index);
        static_cast<QueueItemDelegate *>(m_itemDelegate)->setCurrentRow(index);
        m_view->viewport()->update();
    });
    static_cast<QueueTableView *>(m_view)->setCurrentPlayingRow(store->currentIndex());
    static_cast<QueueItemDelegate *>(m_itemDelegate)->setCurrentRow(store->currentIndex());
}

QString QueueTable::viewSettingsJson() const
{
    QJsonArray visibleColumns;
    const QSet<QString> userVisible = m_columnLayout->userVisibleColumns();
    for (const ColumnSpec &spec : columns) {
        if (userVisible.contains(QString::fromLatin1(spec.key))) {
            visibleColumns.append(QString::fromLatin1(spec.key));
        }
    }

    QJsonObject root;
    root.insert(QStringLiteral("visibleColumns"), visibleColumns);
    root.insert(QStringLiteral("headerHeight"), m_view->horizontalHeader()->height());
    root.insert(QStringLiteral("rowHeight"), m_view->verticalHeader()->defaultSectionSize());
    root.insert(QStringLiteral("headerState"), QString::fromLatin1(m_view->horizontalHeader()->saveState().toBase64()));
    m_columnLayout->writeSavedWidthsJson(&root);
    m_columnLayout->writePrioritiesJson(&root);
    m_columnLayout->writeMinimumWidthsJson(&root);
    m_columnLayout->writeDropOrderJson(&root);
    root.insert(QStringLiteral("currentRow"), currentRow());
    root.insert(QStringLiteral("showPlayNextBadge"), m_showPlayNextBadge);
    root.insert(QStringLiteral("showPlayNextTitleAccent"), m_showPlayNextTitleAccent);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void QueueTable::applyViewSettingsJson(const QString &json)
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

    setHeaderHeight(root.value(QStringLiteral("headerHeight")).toInt(20));
    const int defaultRowHeight = m_preset == QueueTablePreset::FullScreen ? 20 : 18;
    m_view->verticalHeader()->setDefaultSectionSize(std::clamp(root.value(QStringLiteral("rowHeight")).toInt(defaultRowHeight), defaultRowHeight, 48));
    const QByteArray headerState = QByteArray::fromBase64(root.value(QStringLiteral("headerState")).toString().toLatin1());
    if (!headerState.isEmpty()) {
        m_view->horizontalHeader()->restoreState(headerState);
    }
    m_columnLayout->applyPrioritiesJson(root);
    m_columnLayout->applyMinimumWidthsJson(root);
    m_columnLayout->applyDropOrderJson(root);
    m_columnLayout->applySavedWidthsJson(root);
    m_columnLayout->setUserVisibleColumns(visibleKeys);

    m_showPlayNextBadge = root.value(QStringLiteral("showPlayNextBadge")).toBool(true);
    static_cast<QueueTableModel *>(m_model)->setShowPlayNextBadge(m_showPlayNextBadge);
    m_showPlayNextTitleAccent = root.value(QStringLiteral("showPlayNextTitleAccent")).toBool(false);
    static_cast<QueueItemDelegate *>(m_itemDelegate)->setShowTitleAccent(m_showPlayNextTitleAccent);

    m_pendingRestoreRow = root.value(QStringLiteral("currentRow")).toInt(-1);
    m_restoreScrollPending = true;
    scheduleRestoreScrollToCurrentRow();
}

void QueueTable::resetViewSettings()
{
    const QSignalBlocker headerBlocker(m_view->horizontalHeader());
    m_restoreScrollPending = false;
    m_restoreScrollScheduled = false;
    m_pendingRestoreRow = -1;
    m_showPlayNextBadge = true;
    m_showPlayNextTitleAccent = false;
    static_cast<QueueTableModel *>(m_model)->setShowPlayNextBadge(m_showPlayNextBadge);
    static_cast<QueueItemDelegate *>(m_itemDelegate)->setShowTitleAccent(m_showPlayNextTitleAccent);
    setHeaderHeight(20);
    const int defaultRowHeight = m_preset == QueueTablePreset::FullScreen ? 20 : 18;
    m_view->verticalHeader()->setDefaultSectionSize(defaultRowHeight);
    applyPresetDefaults();
    m_view->viewport()->update();
    emit viewSettingsChanged();
}

QWidget *QueueTable::navigationWidget() const
{
    return m_view;
}

int QueueTable::rowCount() const
{
    return m_model->rowCount();
}

int QueueTable::currentRow() const
{
    return static_cast<QueueTableView *>(m_view)->currentNavigationRow();
}

void QueueTable::setCurrentRow(int row)
{
    setCurrentRow(row, 0);
}

void QueueTable::setCurrentRow(int row, int scrollDirection)
{
    static_cast<QueueTableView *>(m_view)->setCurrentNavigationRow(row, scrollDirection);
}

void QueueTable::moveCurrentRow(int delta)
{
    if (rowCount() == 0) {
        return;
    }
    const int row = currentRow() >= 0 ? currentRow() : 0;
    setCurrentRow(std::clamp(row + delta, 0, rowCount() - 1), delta);
}

void QueueTable::activateCurrentRow()
{
    const int row = currentRow();
    if (row >= 0 && row < rowCount()) {
        emit trackActivated(row);
    }
}

void QueueTable::revealCurrentPlaying()
{
    const int row = m_store == nullptr ? -1 : m_store->currentIndex();
    if (row >= 0 && row < rowCount()) {
        m_view->scrollTo(m_model->index(row, 0), QAbstractItemView::PositionAtCenter);
        setCurrentRow(row);
    }
}

void QueueTable::setNavigationScrollPadding(int rows)
{
    static_cast<QueueTableView *>(m_view)->setNavigationScrollPadding(rows);
}

void QueueTable::setKeyBindingProfileName(const QString &name)
{
    m_keyBindingProfileName = name.isEmpty() ? defaultQueueKeyBindingProfileName() : name;
    m_keyBindings = queueBindingMapForProfile(m_keyBindingProfileName);
}

bool QueueTable::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_view && event->type() == QEvent::Show) {
        scheduleRestoreScrollToCurrentRow();
    }
    if (m_searchEdit != nullptr && watched == m_searchEdit && event->type() == QEvent::KeyPress) {
        if (handleSearchEditKey(static_cast<QKeyEvent *>(event))) {
            return true;
        }
    }
    if ((watched == m_view || watched == m_view->viewport()) && event->type() == QEvent::KeyPress) {
        if (handleKeyPress(static_cast<QKeyEvent *>(event))) {
            return true;
        }
    }
    if (watched == m_view->viewport() && event->type() == QEvent::Wheel) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        if (wheel->modifiers() & Qt::ControlModifier) {
            const int step = wheel->angleDelta().y() > 0 ? 2 : -2;
            const int minHeight = m_preset == QueueTablePreset::FullScreen ? 20 : 18;
            const int rowHeight = std::clamp(m_view->verticalHeader()->defaultSectionSize() + step, minHeight, 48);
            m_view->verticalHeader()->setDefaultSectionSize(rowHeight);
            emit viewSettingsChanged();
            wheel->accept();
            return true;
        }
    }
    if (watched == m_view->viewport() && event->type() == QEvent::MouseMove) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        const QModelIndex index = m_view->indexAt(mouse->pos());
        setHoveredRow(index.isValid() ? index.row() : -1);
    } else if (watched == m_view->viewport() && event->type() == QEvent::Leave) {
        setHoveredRow(-1);
    }
    return QWidget::eventFilter(watched, event);
}

void QueueTable::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange) {
        if (auto *view = qobject_cast<NavigableTableView *>(m_view)) {
            view->refreshTheme();
        }
        m_view->viewport()->update();
        m_view->horizontalHeader()->viewport()->update();
    }
}

void QueueTable::showHeaderMenu(const QPoint &pos)
{
    QMenu menu(this);
    QSet<QString> visibleKeys = m_columnLayout->userVisibleColumns();
    for (const ColumnSpec &spec : columns) {
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
    for (const ColumnSpec &spec : columns) {
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
        ResponsiveColumnOptionsDialog dialog(m_columnLayout, responsiveColumnOptions(), this);
        dialog.exec();
    });

    QAction *badgeAction = menu.addAction(QStringLiteral("Show play-next badge in # column"));
    badgeAction->setCheckable(true);
    badgeAction->setChecked(m_showPlayNextBadge);
    connect(badgeAction, &QAction::toggled, this, [this](bool checked) {
        m_showPlayNextBadge = checked;
        static_cast<QueueTableModel *>(m_model)->setShowPlayNextBadge(checked);
        emit viewSettingsChanged();
    });

    QAction *accentAction = menu.addAction(QStringLiteral("Show play-next ordinal in title"));
    accentAction->setCheckable(true);
    accentAction->setChecked(m_showPlayNextTitleAccent);
    connect(accentAction, &QAction::toggled, this, [this](bool checked) {
        m_showPlayNextTitleAccent = checked;
        static_cast<QueueItemDelegate *>(m_itemDelegate)->setShowTitleAccent(checked);
        m_view->viewport()->update();
        emit viewSettingsChanged();
    });

    menu.addSeparator();
    QAction *resetLayout = menu.addAction(QStringLiteral("Reset table layout to defaults"));
    connect(resetLayout, &QAction::triggered, this, &QueueTable::resetViewSettings);

    menu.exec(m_view->horizontalHeader()->mapToGlobal(pos));
}

void QueueTable::showQueueMenu(const QPoint &pos)
{
    if (m_store == nullptr) {
        return;
    }
    const int row = m_view->rowAt(pos.y());
    if (row < 0 || row >= m_store->tracks().size()) {
        return;
    }
    if (!m_view->selectionModel()->isRowSelected(row, QModelIndex())) {
        m_view->selectRow(row);
    }

    const Track track = m_store->tracks().at(row);
    QMenu menu(this);
    QAction *play = menu.addAction(QStringLiteral("Play"));
    connect(play, &QAction::triggered, this, [this, row]() {
        emit trackActivated(row);
    });
    menu.addSeparator();
    QAction *findInLibrary = menu.addAction(QStringLiteral("Find in library"));
    connect(findInLibrary, &QAction::triggered, this, [this, track]() {
        emit trackLibraryRequested(track);
    });
    QAction *findFile = menu.addAction(QStringLiteral("Open containing directory"));
    connect(findFile, &QAction::triggered, this, [this, track]() {
        emit findFileRequested(track);
    });
    QAction *removeSelected = menu.addAction(QStringLiteral("Remove selected"));
    connect(removeSelected, &QAction::triggered, this, [this]() {
        QVector<int> rows;
        const QModelIndexList selected = m_view->selectionModel()->selectedRows();
        rows.reserve(selected.size());
        for (const QModelIndex &index : selected) {
            rows.push_back(index.row());
        }
        emit rowsRemoveRequested(rows);
    });
    QAction *clearPlayNext = menu.addAction(QStringLiteral("Clear play next priority"));
    clearPlayNext->setEnabled(m_store->playNextEnd() > m_store->playNextBegin() && m_store->playNextBegin() >= 0);
    connect(clearPlayNext, &QAction::triggered, this, [this]() {
        emit clearPlayNextPriorityRequested();
    });
    QAction *clearQueue = menu.addAction(QStringLiteral("Clear queue"));
    connect(clearQueue, &QAction::triggered, this, [this]() {
        emit clearRequested();
    });
    menu.exec(m_view->viewport()->mapToGlobal(pos));
}

bool QueueTable::handleKeyPress(QKeyEvent *event)
{
    if (m_preset != QueueTablePreset::FullScreen) {
        return false;
    }

    QString action = m_keyBindings.value(keySequenceForEvent(event));
    if (action.isEmpty() && event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_V) {
        action = QString::fromLatin1(QueueAction::PageDown);
    }
    if (action.isEmpty()) {
        return false;
    }

    if (action == QString::fromLatin1(QueueAction::MoveDown)) {
        moveCurrentRow(+1);
    } else if (action == QString::fromLatin1(QueueAction::MoveUp)) {
        moveCurrentRow(-1);
    } else if (action == QString::fromLatin1(QueueAction::PageDown)) {
        setCurrentRow(std::min(rowCount() - 1, std::max(0, currentRow()) + pageStepRows()), +1);
    } else if (action == QString::fromLatin1(QueueAction::PageUp)) {
        setCurrentRow(std::max(0, std::max(0, currentRow()) - pageStepRows()), -1);
    } else if (action == QString::fromLatin1(QueueAction::PlaySelected)) {
        activateCurrentRow();
    } else if (action == QString::fromLatin1(QueueAction::RemoveSelected)) {
        emit rowsRemoveRequested(selectedRowsForAction());
    } else if (action == QString::fromLatin1(QueueAction::ClearQueue)) {
        emit clearRequested();
    } else if (action == QString::fromLatin1(QueueAction::ClearPlayNext)) {
        emit clearPlayNextPriorityRequested();
    } else if (action == QString::fromLatin1(QueueAction::FindFile)) {
        const Track track = currentTrackForAction();
        if (!track.path.isEmpty()) {
            emit findFileRequested(track);
        }
    } else if (action == QString::fromLatin1(QueueAction::FindLibrary)) {
        const Track track = currentTrackForAction();
        if (!track.path.isEmpty()) {
            emit trackLibraryRequested(track);
        }
    } else if (action == QString::fromLatin1(QueueAction::JumpPlaying)) {
        revealCurrentPlaying();
    } else if (action == QString::fromLatin1(QueueAction::Search)) {
        openSearch();
    } else if (action == QString::fromLatin1(QueueAction::SearchNext)) {
        cycleSearchMatch(+1);
    } else if (action == QString::fromLatin1(QueueAction::SearchPrevious)) {
        cycleSearchMatch(-1);
    } else if (action == QString::fromLatin1(QueueAction::Escape)) {
        escapeSearch();
    } else {
        return false;
    }
    return true;
}

void QueueTable::openSearch()
{
    if (m_searchBar == nullptr) {
        return;
    }
    m_searchBar->setVisible(true);
    updateSearchUi();
    const QSignalBlocker blocker(m_searchEdit);
    m_searchEdit->setText(m_searchQuery);
    m_searchEdit->setFocus();
    m_searchEdit->selectAll();
}

void QueueTable::escapeSearch()
{
    if (m_searchBar == nullptr) {
        return;
    }
    // First Escape clears a non-empty query; a second one dismisses the bar.
    if (!m_searchEdit->text().isEmpty()) {
        m_searchEdit->clear();
        return;
    }
    m_searchQuery.clear();
    m_searchMatches.clear();
    m_searchCurrentMatch = -1;
    m_searchBar->setVisible(false);
    navigationWidget()->setFocus();
}

void QueueTable::setSearchQuery(const QString &query)
{
    m_searchQuery = query.trimmed();
    rebuildSearchMatches(true);
}

void QueueTable::rebuildSearchMatches(bool jumpToFirst)
{
    m_searchMatches.clear();
    m_searchCurrentMatch = -1;

    if (m_searchQuery.isEmpty()) {
        updateSearchUi();
        return;
    }

    const QVector<Search::MatchDocument> docs =
        m_store == nullptr ? QVector<Search::MatchDocument>() : m_store->searchDocuments();
    m_searchMatches = Search::matchDocumentsInDisplayOrder(
        docs, Search::SearchQuery::parse(m_searchQuery), m_searchFuzzy);

    if (!m_searchMatches.isEmpty()) {
        int matchIndex = 0;
        const int current = currentRow();
        for (int i = 0; i < m_searchMatches.size(); ++i) {
            if (m_searchMatches.at(i).row >= current) {
                matchIndex = i;
                break;
            }
        }
        m_searchCurrentMatch = matchIndex;
        if (jumpToFirst) {
            setCurrentRow(m_searchMatches.at(matchIndex).row);
        }
    }
    updateSearchUi();
}

void QueueTable::updateSearchUi()
{
    if (m_searchBar == nullptr) {
        return;
    }
    const QString mode = m_searchFuzzy ? QStringLiteral("fuzzy") : QStringLiteral("exact");
    const int rows = rowCount();
    if (rows == 0) {
        m_searchStatus->setText(QStringLiteral("No rows · %1").arg(mode));
    } else if (m_searchQuery.isEmpty()) {
        m_searchStatus->setText(QStringLiteral("%1 rows · %2").arg(rows).arg(mode));
    } else if (m_searchMatches.isEmpty()) {
        m_searchStatus->setText(QStringLiteral("No matches · %1").arg(mode));
    } else {
        m_searchStatus->setText(QStringLiteral("%1/%2 · %3")
                                    .arg(m_searchCurrentMatch + 1)
                                    .arg(m_searchMatches.size())
                                    .arg(mode));
    }
}

void QueueTable::cycleSearchMatch(int direction)
{
    if (m_searchBar == nullptr) {
        return;
    }
    if (m_searchQuery.isEmpty()) {
        openSearch();
        return;
    }
    rebuildSearchMatches(false);
    if (m_searchMatches.isEmpty()) {
        return;
    }
    if (m_searchCurrentMatch < 0) {
        m_searchCurrentMatch = 0;
    } else {
        const qsizetype size = m_searchMatches.size();
        const qsizetype next = (static_cast<qsizetype>(m_searchCurrentMatch) + direction + size) % size;
        m_searchCurrentMatch = static_cast<int>(next);
    }
    setCurrentRow(m_searchMatches.at(m_searchCurrentMatch).row);
    updateSearchUi();
}

bool QueueTable::handleSearchEditKey(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        escapeSearch();
        return true;
    }
    const QString action = m_keyBindings.value(keySequenceForEvent(event));
    if (action == QString::fromLatin1(QueueAction::Escape)) {
        escapeSearch();
        return true;
    }
    if (action == QString::fromLatin1(QueueAction::SearchNext)) {
        cycleSearchMatch(+1);
        return true;
    }
    if (action == QString::fromLatin1(QueueAction::SearchPrevious)) {
        cycleSearchMatch(-1);
        return true;
    }
    if (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_F) {
        m_searchFuzzy = !m_searchFuzzy;
        rebuildSearchMatches(false);
        return true;
    }
    return false;
}

int QueueTable::pageStepRows() const
{
    const int rowHeight = std::max(1, m_view->verticalHeader()->defaultSectionSize());
    return std::max(1, m_view->viewport()->height() / rowHeight - 1);
}

QVector<int> QueueTable::selectedRowsForAction() const
{
    QVector<int> rows;
    if (m_view->selectionModel() != nullptr) {
        const QModelIndexList selected = m_view->selectionModel()->selectedRows();
        rows.reserve(selected.size());
        for (const QModelIndex &index : selected) {
            rows.push_back(index.row());
        }
    }
    if (rows.isEmpty() && currentRow() >= 0) {
        rows.push_back(currentRow());
    }
    std::sort(rows.begin(), rows.end());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    return rows;
}

Track QueueTable::currentTrackForAction() const
{
    const int row = currentRow();
    if (m_store == nullptr || row < 0 || row >= m_store->tracks().size()) {
        return {};
    }
    return m_store->tracks().at(row);
}

void QueueTable::scheduleRestoreScrollToCurrentRow()
{
    if (!m_restoreScrollPending || m_restoreScrollScheduled) {
        return;
    }
    m_restoreScrollScheduled = true;
    QTimer::singleShot(0, this, &QueueTable::restoreScrollToCurrentRowOnce);
}

void QueueTable::restoreScrollToCurrentRowOnce()
{
    m_restoreScrollScheduled = false;
    if (!m_restoreScrollPending) {
        return;
    }
    if (m_model == nullptr || m_view == nullptr || m_model->rowCount() <= 0) {
        m_restoreScrollPending = false;
        m_pendingRestoreRow = -1;
        return;
    }
    if (m_preset == QueueTablePreset::FullScreen && !m_view->isVisible()) {
        return;
    }
    if (m_view->viewport()->height() <= 0) {
        return;
    }

    int row = m_pendingRestoreRow;
    if (row < 0) {
        row = currentRow();
    }
    if (row < 0 && m_store != nullptr) {
        row = m_store->currentIndex();
    }
    if (row < 0) {
        row = 0;
    }
    row = std::clamp(row, 0, m_model->rowCount() - 1);

    const int rowHeightPx = std::max(1, m_view->rowHeight(row));
    const int visibleRows = std::max(1, m_view->viewport()->height() / rowHeightPx);
    if (row >= visibleRows && m_view->verticalScrollBar()->maximum() == 0) {
        return;
    }

    setCurrentRow(row);
    m_restoreScrollPending = false;
    m_pendingRestoreRow = -1;
}

void QueueTable::setHoveredRow(int row)
{
    if (m_hoveredRow == row) {
        return;
    }

    const int previous = m_hoveredRow;
    m_hoveredRow = row;
    static_cast<QueueItemDelegate *>(m_itemDelegate)->setHoveredRow(row);
    m_ratingDelegate->setHoveredRow(row);
    if (previous >= 0) {
        m_model->setData(m_model->index(previous, 2), StarRating::unset, HoverRatingRole);
        const QRect rect = m_view->visualRect(m_model->index(previous, 0));
        m_view->viewport()->update(QRect(0, rect.top(), m_view->viewport()->width(), rect.height()));
    }
    if (row >= 0) {
        const QRect rect = m_view->visualRect(m_model->index(row, 0));
        m_view->viewport()->update(QRect(0, rect.top(), m_view->viewport()->width(), rect.height()));
    }
}

void QueueTable::updateHoverFromCursor()
{
    const QPoint pos = m_view->viewport()->mapFromGlobal(QCursor::pos());
    if (!m_view->viewport()->rect().contains(pos)) {
        setHoveredRow(-1);
        return;
    }
    const QModelIndex index = m_view->indexAt(pos);
    setHoveredRow(index.isValid() ? index.row() : -1);
}

void QueueTable::applyPresetDefaults()
{
    const QVector<int> visible = defaultVisibleColumnsForPreset(m_preset);
    for (int visual = 0; visual < visible.size(); ++visual) {
        m_view->horizontalHeader()->moveSection(m_view->horizontalHeader()->visualIndex(visible.at(visual)), visual);
    }
    m_columnLayout->resetToDefaults();
    m_columnLayout->setUserVisibleColumns(defaultVisibleKeysForPreset(m_preset));
}

void QueueTable::setHeaderHeight(int height)
{
    m_view->horizontalHeader()->setFixedHeight(std::clamp(height, 18, 40));
}
