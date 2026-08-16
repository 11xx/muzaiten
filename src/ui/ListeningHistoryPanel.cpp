#include "ui/ListeningHistoryPanel.h"

#include "core/HumanQuantity.h"
#include "ui/DenseTableDelegate.h"
#include "ui/NeighborColumnResizer.h"
#include "ui/ResponsiveColumnLayout.h"
#include "ui/RowHeightWheel.h"
#include "ui/StickyMenu.h"

#include <QAbstractTableModel>
#include <QAbstractItemView>
#include <QAction>
#include <QCheckBox>
#include <QDateTime>
#include <QEvent>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSet>
#include <QTableView>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <optional>

namespace {

constexpr int kHistoryLimit = 5000;

// The table's placeholder for a cell with no value.
const auto kEmptyCell = QStringLiteral("—");

enum Column {
    TimeColumn,
    TitleColumn,
    ArtistColumn,
    AlbumColumn,
    DurationColumn,
    ScrobbledColumn,
    PathColumn,
    ColumnCount,
};

// The scrobbled cell for one listen, reported over exactly the destinations the
// view is scoped to. Naming one destination answers for that one; naming
// several answers for those several, which is what makes the ratio answer the
// question the user actually asked.
QString scrobbledText(const ListenHistoryStore::HistoryRow &row, const QStringList &destinationIds)
{
    int owed = 0;
    int sent = 0;
    for (const QString &destinationId : destinationIds) {
        if (!row.owedTo(destinationId)) {
            continue;
        }
        ++owed;
        if (row.sentTo(destinationId)) {
            ++sent;
        }
    }
    if (owed == 0) {
        return destinationIds.size() == 1 ? QStringLiteral("Not queued") : kEmptyCell;
    }
    if (destinationIds.size() == 1) {
        return sent == 1 ? QStringLiteral("Sent") : QStringLiteral("Pending");
    }
    return QStringLiteral("%1/%2 sent").arg(sent).arg(owed);
}

QVector<ResponsiveColumnSpec> historyResponsiveSpecs()
{
    return {
        {TimeColumn, QStringLiteral("time"), 145, 120, ResponsiveColumnPriority::Keep},
        {TitleColumn, QStringLiteral("title"), 260, 140, ResponsiveColumnPriority::Keep, true},
        {ArtistColumn, QStringLiteral("artist"), 200, 110, ResponsiveColumnPriority::Keep},
        {AlbumColumn, QStringLiteral("album"), 220, 110, ResponsiveColumnPriority::Normal},
        {DurationColumn, QStringLiteral("duration"), 80, 70, ResponsiveColumnPriority::Normal},
        {ScrobbledColumn, QStringLiteral("scrobbled"), 120, 100, ResponsiveColumnPriority::Keep},
        {PathColumn, QStringLiteral("path"), 320, 120, ResponsiveColumnPriority::HideEarly},
    };
}

// A popup borrows the window's activation, and Qt then resolves the selection
// colours from the inactive palette group. On themes where that inactive
// highlight sits close to the alternating-row shade, a selected odd row stops
// looking selected for as long as the popup is up. Pinning the inactive brushes
// to the active ones keeps the selection readable throughout.
void keepSelectionVisibleWhileUnfocused(QAbstractItemView *view)
{
    QPalette palette = view->palette();
    palette.setBrush(QPalette::Inactive, QPalette::Highlight, palette.brush(QPalette::Active, QPalette::Highlight));
    palette.setBrush(QPalette::Inactive, QPalette::HighlightedText,
                     palette.brush(QPalette::Active, QPalette::HighlightedText));
    view->setPalette(palette);
}

class ListeningHistoryModel final : public QAbstractTableModel {
public:
    explicit ListeningHistoryModel(QObject *parent = nullptr)
        : QAbstractTableModel(parent)
    {
    }

    void setRows(QList<ListenHistoryStore::HistoryRow> rows)
    {
        beginResetModel();
        m_rows = std::move(rows);
        endResetModel();
    }

    int rowCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
    }

    int columnCount(const QModelIndex &parent = {}) const override
    {
        return parent.isValid() ? 0 : ColumnCount;
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) {
            return {};
        }
        switch (section) {
        case TimeColumn:
            return QStringLiteral("Listened");
        case TitleColumn:
            return QStringLiteral("Title");
        case ArtistColumn:
            return QStringLiteral("Artist");
        case AlbumColumn:
            return QStringLiteral("Album");
        case DurationColumn:
            return QStringLiteral("Duration");
        case ScrobbledColumn:
            return QStringLiteral("Scrobbled");
        case PathColumn:
            return QStringLiteral("Path");
        }
        return {};
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
            return {};
        }
        const ListenHistoryStore::HistoryRow &row = m_rows.at(index.row());
        if (role == Qt::UserRole) {
            return row.id;
        }
        if (role != Qt::DisplayRole) {
            return {};
        }

        switch (index.column()) {
        case TimeColumn:
            return QDateTime::fromSecsSinceEpoch(row.listenedAtSecs).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
        case TitleColumn:
            return row.track.title.trimmed().isEmpty() ? row.track.filename : row.track.title;
        case ArtistColumn:
            return row.track.artistName.trimmed().isEmpty() ? row.track.albumArtistName : row.track.artistName;
        case AlbumColumn:
            return row.track.albumTitle;
        case DurationColumn:
            return humanquantity::formatClock(row.track.durationMs);
        case ScrobbledColumn:
            return scrobbledText(row, m_destinationIds);
        case PathColumn:
            return row.track.path;
        }
        return {};
    }

    // Listens among `indexes` that are not yet owed to `destinationId`, which
    // are the ones marking a backlog would actually change.
    QList<qint64> idsQueueableFor(const QModelIndexList &indexes, const QString &destinationId) const
    {
        QList<qint64> ids;
        QSet<int> seenRows;
        for (const QModelIndex &index : indexes) {
            if (!index.isValid() || seenRows.contains(index.row()) || index.row() >= m_rows.size()) {
                continue;
            }
            seenRows.insert(index.row());
            const ListenHistoryStore::HistoryRow &row = m_rows.at(index.row());
            if (!row.owedTo(destinationId)) {
                ids.push_back(row.id);
            }
        }
        return ids;
    }

    bool hasQueueableFor(const QModelIndexList &indexes, const QStringList &destinationIds) const
    {
        return std::any_of(destinationIds.cbegin(), destinationIds.cend(), [this, &indexes](const QString &id) {
            return !idsQueueableFor(indexes, id).isEmpty();
        });
    }

    std::optional<ListenHistoryStore::HistoryRow> rowAt(int row) const
    {
        if (row < 0 || row >= m_rows.size()) {
            return std::nullopt;
        }
        return m_rows.at(row);
    }

    void setDestinations(QStringList destinationIds)
    {
        beginResetModel();
        m_destinationIds = std::move(destinationIds);
        endResetModel();
    }

private:
    QList<ListenHistoryStore::HistoryRow> m_rows;
    QStringList m_destinationIds;
};

} // namespace

ListeningHistoryPanel::ListeningHistoryPanel(ListenHistoryStore *store,
                                               ScrobbleDestinationSet destinations, QWidget *parent)
    : QWidget(parent)
    , m_store(store)
    , m_destinations(std::move(destinations))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    // Several destinations can be picked in one visit, and the choice lasts as
    // long as the window: it scopes what is shown and acted on, and is not a
    // setting worth outliving the question that prompted it.
    m_destinationButton = new QPushButton(this);
    auto *destinationMenu = new StickyMenu(m_destinationButton);
    m_destinationButton->setMenu(destinationMenu);
    for (const ScrobbleDestination &destination : m_destinations.items) {
        QAction *action = destinationMenu->addAction(destination.name);
        action->setCheckable(true);
        action->setData(destination.id);
        connect(action, &QAction::triggered, this, [this]() {
            static_cast<ListeningHistoryModel *>(m_model)->setDestinations(scopedDestinationIds());
            updateDestinationButton();
            reload();
        });
        m_destinationActions.push_back(action);
    }
    destinationMenu->addSeparator();
    QAction *showAll = destinationMenu->addAction(QStringLiteral("Show all destinations"));
    connect(showAll, &QAction::triggered, this, [this]() {
        for (QAction *action : std::as_const(m_destinationActions)) {
            action->setChecked(false);
        }
        static_cast<ListeningHistoryModel *>(m_model)->setDestinations(scopedDestinationIds());
        updateDestinationButton();
        reload();
    });

    auto *selectorRow = new QHBoxLayout;
    selectorRow->addWidget(new QLabel(QStringLiteral("Destination"), this));
    selectorRow->addWidget(m_destinationButton);
    selectorRow->addStretch();
    layout->addLayout(selectorRow);

    m_summary = new QLabel(this);
    layout->addWidget(m_summary);

    m_model = new ListeningHistoryModel(this);
    m_view = new QTableView(this);
    m_view->setModel(m_model);
    m_view->setAlternatingRowColors(true);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setSortingEnabled(false);
    m_view->setWordWrap(false);
    m_view->setItemDelegate(new DenseTableDelegate(this));
    m_view->verticalHeader()->setVisible(false);
    m_view->verticalHeader()->setDefaultSectionSize(18);
    m_view->verticalHeader()->setMinimumSectionSize(18);
    m_view->horizontalHeader()->setSectionsMovable(false);
    m_view->horizontalHeader()->setStretchLastSection(false);
    m_view->viewport()->installEventFilter(this);
    keepSelectionVisibleWhileUnfocused(m_view);
    m_columnLayout = new ResponsiveColumnLayout(m_view, historyResponsiveSpecs(), this);
    const QVector<ResponsiveColumnSpec> specs = historyResponsiveSpecs();
    auto *resizer = NeighborColumnResizer::install(
        m_view->horizontalHeader(),
        [this, specs](int logical) {
            return m_columnLayout == nullptr || logical < 0 || logical >= specs.size()
                ? 60
                : m_columnLayout->columnMinimumWidth(specs.at(logical).key);
        });
    connect(resizer, qOverload<int, int>(&NeighborColumnResizer::columnResized), this, [this](int left, int right) {
        if (m_columnLayout != nullptr) {
            m_columnLayout->updateBaselineWidthsForResize(left, right);
        }
    });
    layout->addWidget(m_view, 1);

    auto *actions = new QHBoxLayout;
    m_queueSelected = new QPushButton(QStringLiteral("Scrobble selected"), this);
    m_forgetBehavior = new QPushButton(QStringLiteral("Forget track's listening behavior…"), this);
    m_retryPending = new QPushButton(QStringLiteral("Retry pending"), this);
    m_clearBacklog = new QPushButton(QStringLiteral("Clear backlog"), this);
    auto *refresh = new QPushButton(QStringLiteral("Refresh"), this);
    actions->addWidget(m_queueSelected);
    actions->addWidget(m_forgetBehavior);
    actions->addStretch();
    actions->addWidget(m_retryPending);
    actions->addWidget(m_clearBacklog);
    actions->addWidget(refresh);
    layout->addLayout(actions);

    connect(refresh, &QPushButton::clicked, this, &ListeningHistoryPanel::reload);
    connect(m_queueSelected, &QPushButton::clicked, this, &ListeningHistoryPanel::queueSelected);
    connect(m_forgetBehavior, &QPushButton::clicked, this, &ListeningHistoryPanel::forgetSelectedBehavior);
    connect(m_retryPending, &QPushButton::clicked, this, &ListeningHistoryPanel::retryPending);
    connect(m_clearBacklog, &QPushButton::clicked, this, &ListeningHistoryPanel::clearBacklogs);
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() { updateActions(); });

    static_cast<ListeningHistoryModel *>(m_model)->setDestinations(scopedDestinationIds());
    updateDestinationButton();
    reload();
}

bool ListeningHistoryPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_view->viewport() && event->type() == QEvent::Wheel) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        if (ui::applyCtrlWheelRowHeight(wheel, m_view->verticalHeader()->defaultSectionSize(), 18, 48,
                [this](int h) {
                    m_view->verticalHeader()->setDefaultSectionSize(h);
                    emit rowHeightChanged(h);  // persisted by the owner; dialog is per-open
                })) {
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ListeningHistoryPanel::setRowHeight(int height)
{
    const int clamped = std::clamp(height, 18, 48);
    m_view->verticalHeader()->setDefaultSectionSize(clamped);
}

QStringList ListeningHistoryPanel::scopedDestinationIds() const
{
    QStringList ids;
    for (const QAction *action : m_destinationActions) {
        if (action->isChecked()) {
            ids << action->data().toString();
        }
    }
    return ids;
}

void ListeningHistoryPanel::updateDestinationButton()
{
    const QStringList ids = scopedDestinationIds();
    if (ids.isEmpty()) {
        m_destinationButton->setText(QStringLiteral("All destinations"));
    } else if (ids.size() == 1) {
        m_destinationButton->setText(destinationName(ids.first()));
    } else {
        m_destinationButton->setText(
            QStringLiteral("%1 of %2 destinations").arg(ids.size()).arg(m_destinations.items.size()));
    }
}

void ListeningHistoryPanel::reload()
{
    if (m_store == nullptr || !m_store->isOpen()) {
        static_cast<ListeningHistoryModel *>(m_model)->setRows({});
        m_summary->setText(QStringLiteral("Listening history is unavailable."));
        updateActions();
        return;
    }

    static_cast<ListeningHistoryModel *>(m_model)->setRows(m_store->historyRows(kHistoryLimit));
    const int total = m_store->totalCount();
    const QStringList ids = scopedDestinationIds();
    const QString shown = QStringLiteral("Showing latest %1 of %2 listens.")
                              .arg(std::min(kHistoryLimit, total))
                              .arg(total);
    if (ids.isEmpty()) {
        // The overview reports the whole picture and mutates nothing, so it sums
        // deliveries across destinations instead of naming a backlog.
        int owed = 0;
        int sent = 0;
        for (const ScrobbleDestination &destination : m_destinations.items) {
            owed += m_store->pendingCount(destination.id) + m_store->sentCount(destination.id);
            sent += m_store->sentCount(destination.id);
        }
        m_summary->setText(QStringLiteral("%1 %2/%3 sent across %4 destinations.")
                               .arg(shown)
                               .arg(sent)
                               .arg(owed)
                               .arg(m_destinations.items.size()));
    } else if (ids.size() == 1) {
        m_summary->setText(QStringLiteral("%1 %2: %3 pending, %4 sent.")
                               .arg(shown, destinationName(ids.first()))
                               .arg(m_store->pendingCount(ids.first()))
                               .arg(m_store->sentCount(ids.first())));
    } else {
        int pending = 0;
        int sent = 0;
        for (const QString &id : ids) {
            pending += m_store->pendingCount(id);
            sent += m_store->sentCount(id);
        }
        m_summary->setText(QStringLiteral("%1 %2 destinations: %3 pending, %4 sent.")
                               .arg(shown)
                               .arg(ids.size())
                               .arg(pending)
                               .arg(sent));
    }
    updateActions();
}

QList<qint64> ListeningHistoryPanel::selectedIds() const
{
    if (m_view == nullptr || m_view->selectionModel() == nullptr) {
        return {};
    }
    QList<qint64> ids;
    for (const QModelIndex &index : m_view->selectionModel()->selectedRows()) {
        ids.push_back(index.data(Qt::UserRole).toLongLong());
    }
    return ids;
}

std::optional<ListenHistoryStore::HistoryRow> ListeningHistoryPanel::selectedHistoryRow() const
{
    if (m_view == nullptr || m_view->selectionModel() == nullptr || m_model == nullptr) {
        return std::nullopt;
    }
    const QModelIndexList rows = m_view->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        return std::nullopt;
    }
    return static_cast<ListeningHistoryModel *>(m_model)->rowAt(rows.first().row());
}

QString ListeningHistoryPanel::destinationName(const QString &destinationId) const
{
    if (const ScrobbleDestination *destination = m_destinations.find(destinationId)) {
        return destination->name;
    }
    return destinationId;
}

void ListeningHistoryPanel::queueSelected()
{
    const QStringList ids = scopedDestinationIds();
    if (m_store == nullptr || ids.isEmpty() || m_view == nullptr || m_view->selectionModel() == nullptr) {
        return;
    }
    auto *model = static_cast<ListeningHistoryModel *>(m_model);
    const QModelIndexList selected = m_view->selectionModel()->selectedRows();

    int total = 0;
    for (const QString &destinationId : ids) {
        const int changed = m_store->markOwed(destinationId, model->idsQueueableFor(selected, destinationId));
        total += changed;
        emit backlogChanged(destinationId, changed);
    }
    reload();
    emit statusMessageRequested(QStringLiteral("Marked %1 history listens to scrobble to %2")
                                    .arg(total)
                                    .arg(ids.size() == 1 ? destinationName(ids.first())
                                                         : QStringLiteral("%1 destinations").arg(ids.size())),
                                5000);
}

void ListeningHistoryPanel::retryPending()
{
    const QStringList ids = scopedDestinationIds();
    if (m_store == nullptr || ids.isEmpty()) {
        return;
    }
    int total = 0;
    for (const QString &destinationId : ids) {
        const int pending = m_store->pendingCount(destinationId);
        total += pending;
        emit backlogChanged(destinationId, pending);
    }
    emit statusMessageRequested(QStringLiteral("Retrying %1 pending listens for %2")
                                    .arg(total)
                                    .arg(ids.size() == 1 ? destinationName(ids.first())
                                                         : QStringLiteral("%1 destinations").arg(ids.size())),
                                5000);
}

void ListeningHistoryPanel::clearBacklogs()
{
    const QStringList ids = scopedDestinationIds();
    if (m_store == nullptr || ids.isEmpty()) {
        return;
    }
    int total = 0;
    for (const QString &destinationId : ids) {
        total += m_store->clearPending(destinationId);
        // Clearing leaves nothing owed, so the owner is told the backlog moved
        // without being asked to upload it.
        emit backlogChanged(destinationId, 0);
    }
    reload();
    emit statusMessageRequested(QStringLiteral("Cleared %1 pending scrobbles for %2")
                                    .arg(total)
                                    .arg(ids.size() == 1 ? destinationName(ids.first())
                                                         : QStringLiteral("%1 destinations").arg(ids.size())),
                                5000);
}

void ListeningHistoryPanel::forgetSelectedBehavior()
{
    const std::optional<ListenHistoryStore::HistoryRow> row = selectedHistoryRow();
    if (!row.has_value() || row->track.path.isEmpty()) {
        return;
    }

    const QString title = row->track.title.trimmed().isEmpty() ? row->track.filename : row->track.title.trimmed();
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Forget listening behavior"));
    box.setText(QStringLiteral("Forget listening behavior for \"%1\"?").arg(title));
    box.setInformativeText(QStringLiteral(
        "This deletes local recommendation behavior for every library copy of this song. Scrobble history is not deleted."));
    auto *includeImported = new QCheckBox(QStringLiteral("Also remove imported listens matched to this song"), &box);
    box.setCheckBox(includeImported);
    QPushButton *forgetButton = box.addButton(QStringLiteral("Forget"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != forgetButton) {
        return;
    }

    emit forgetBehaviorRequested(row->track, includeImported->isChecked());
    reload();
}

void ListeningHistoryPanel::updateActions()
{
    const std::optional<ListenHistoryStore::HistoryRow> selectedRow = selectedHistoryRow();
    const QModelIndexList selectedRows = m_view->selectionModel() == nullptr ? QModelIndexList{}
                                                                            : m_view->selectionModel()->selectedRows();
    auto *model = static_cast<ListeningHistoryModel *>(m_model);
    const QStringList ids = scopedDestinationIds();
    // The overview is read-only: with no destination named there is no backlog
    // to queue into, clear, or retry.
    int pending = 0;
    for (const QString &destinationId : ids) {
        pending += m_store == nullptr ? 0 : m_store->pendingCount(destinationId);
    }

    m_queueSelected->setEnabled(!ids.isEmpty() && !selectedRows.isEmpty()
                                && model->hasQueueableFor(selectedRows, ids));
    m_forgetBehavior->setEnabled(selectedRow.has_value() && !selectedRow->track.path.isEmpty());
    m_clearBacklog->setEnabled(!ids.isEmpty() && pending > 0);
    m_retryPending->setEnabled(!ids.isEmpty() && pending > 0);
}
