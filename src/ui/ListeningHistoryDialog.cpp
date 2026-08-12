#include "ui/ListeningHistoryDialog.h"

#include "core/HumanQuantity.h"
#include "ui/DenseTableDelegate.h"
#include "ui/NeighborColumnResizer.h"
#include "ui/ResponsiveColumnLayout.h"
#include "ui/RowHeightWheel.h"

#include <QAbstractTableModel>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QDateTime>
#include <QComboBox>
#include <QDialogButtonBox>
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
    DeliveryColumn,
    PathColumn,
    ColumnCount,
};

// The delivery cell for one listen. With a destination selected it reports that
// destination alone; in aggregate mode it summarizes how many of the
// destinations owed this listen have received it.
QString deliveryText(const ListenHistoryStore::HistoryRow &row, const QString &destinationId)
{
    if (destinationId.isEmpty()) {
        return row.owedCount() == 0 ? kEmptyCell
                                    : QStringLiteral("%1/%2 sent").arg(row.sentCount()).arg(row.owedCount());
    }
    if (row.sentTo(destinationId)) {
        return QStringLiteral("Sent");
    }
    return row.owedTo(destinationId) ? QStringLiteral("Pending") : QStringLiteral("Not queued");
}

bool rowQueueableForService(const ListenHistoryStore::HistoryRow &row, const QString &service)
{
    return !row.owedTo(service);
}

QVector<ResponsiveColumnSpec> historyResponsiveSpecs()
{
    return {
        {TimeColumn, QStringLiteral("time"), 145, 120, ResponsiveColumnPriority::Keep},
        {TitleColumn, QStringLiteral("title"), 260, 140, ResponsiveColumnPriority::Keep, true},
        {ArtistColumn, QStringLiteral("artist"), 200, 110, ResponsiveColumnPriority::Keep},
        {AlbumColumn, QStringLiteral("album"), 220, 110, ResponsiveColumnPriority::Normal},
        {DurationColumn, QStringLiteral("duration"), 80, 70, ResponsiveColumnPriority::Normal},
        {DeliveryColumn, QStringLiteral("delivery"), 120, 100, ResponsiveColumnPriority::Keep},
        {PathColumn, QStringLiteral("path"), 320, 120, ResponsiveColumnPriority::HideEarly},
    };
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
        case DeliveryColumn:
            return QStringLiteral("Delivery");
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
        case DeliveryColumn:
            return deliveryText(row, m_destinationId);
        case PathColumn:
            return row.track.path;
        }
        return {};
    }

    QList<qint64> idsQueueableForService(const QModelIndexList &indexes, const QString &service) const
    {
        QList<qint64> ids;
        QSet<int> seenRows;
        for (const QModelIndex &index : indexes) {
            if (!index.isValid() || seenRows.contains(index.row()) || index.row() >= m_rows.size()) {
                continue;
            }
            seenRows.insert(index.row());
            const ListenHistoryStore::HistoryRow &row = m_rows.at(index.row());
            if (rowQueueableForService(row, service)) {
                ids.push_back(row.id);
            }
        }
        return ids;
    }

    bool hasQueueableForService(const QModelIndexList &indexes, const QString &service) const
    {
        return !idsQueueableForService(indexes, service).isEmpty();
    }

    std::optional<ListenHistoryStore::HistoryRow> rowAt(int row) const
    {
        if (row < 0 || row >= m_rows.size()) {
            return std::nullopt;
        }
        return m_rows.at(row);
    }

    // Empty means the aggregate "All destinations" view.
    void setDestination(const QString &destinationId)
    {
        beginResetModel();
        m_destinationId = destinationId;
        endResetModel();
    }

private:
    QList<ListenHistoryStore::HistoryRow> m_rows;
    QString m_destinationId;
};

} // namespace

ListeningHistoryDialog::ListeningHistoryDialog(ListenHistoryStore *store,
                                               ScrobbleDestinationSet destinations, QWidget *parent)
    : QDialog(parent)
    , m_store(store)
    , m_destinations(std::move(destinations))
{
    setWindowTitle(QStringLiteral("Listening history"));
    resize(1100, 650);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    m_destinationSelector = new QComboBox(this);
    m_destinationSelector->addItem(QStringLiteral("All destinations"), QString());
    for (const ScrobbleDestination &destination : m_destinations.items) {
        m_destinationSelector->addItem(destination.name, destination.id);
    }
    auto *selectorRow = new QHBoxLayout;
    selectorRow->addWidget(new QLabel(QStringLiteral("Destination"), this));
    selectorRow->addWidget(m_destinationSelector);
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
    m_columnLayout = new ResponsiveColumnLayout(m_view, historyResponsiveSpecs(), this);
    auto *resizer = NeighborColumnResizer::install(
        m_view->horizontalHeader(),
        [this](int logical) { return m_columnLayout == nullptr ? 60 : m_columnLayout->columnMinimumWidth(historyResponsiveSpecs().at(logical).key); });
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

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(refresh, &QPushButton::clicked, this, &ListeningHistoryDialog::reload);
    connect(m_destinationSelector, &QComboBox::currentIndexChanged, this, [this]() {
        static_cast<ListeningHistoryModel *>(m_model)->setDestination(selectedDestinationId());
        reload();
    });
    connect(m_queueSelected, &QPushButton::clicked, this, [this]() { queueSelected(selectedDestinationId()); });
    connect(m_forgetBehavior, &QPushButton::clicked, this, &ListeningHistoryDialog::forgetSelectedBehavior);
    connect(m_retryPending, &QPushButton::clicked, this, [this]() {
        const QString destinationId = selectedDestinationId();
        const int pending = m_store == nullptr ? 0 : m_store->pendingCount(destinationId);
        emit backlogChanged(destinationId, pending);
        emit statusMessageRequested(QStringLiteral("Retrying %1 pending listens for %2")
                                        .arg(pending)
                                        .arg(destinationName(destinationId)),
                                    5000);
    });
    connect(m_clearBacklog, &QPushButton::clicked, this, [this]() { clearPending(selectedDestinationId()); });
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() { updateActions(); });

    reload();
}

bool ListeningHistoryDialog::eventFilter(QObject *watched, QEvent *event)
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
    return QDialog::eventFilter(watched, event);
}

void ListeningHistoryDialog::setRowHeight(int height)
{
    const int clamped = std::clamp(height, 18, 48);
    m_view->verticalHeader()->setDefaultSectionSize(clamped);
}

void ListeningHistoryDialog::reload()
{
    if (m_store == nullptr || !m_store->isOpen()) {
        static_cast<ListeningHistoryModel *>(m_model)->setRows({});
        m_summary->setText(QStringLiteral("Listening history is unavailable."));
        updateActions();
        return;
    }

    static_cast<ListeningHistoryModel *>(m_model)->setRows(m_store->historyRows(kHistoryLimit));
    const int total = m_store->totalCount();
    const QString destinationId = selectedDestinationId();
    const QString shown = QStringLiteral("Showing latest %1 of %2 listens.")
                              .arg(std::min(kHistoryLimit, total))
                              .arg(total);
    if (destinationId.isEmpty()) {
        // Aggregate mode reports the whole picture and mutates nothing, so it
        // sums deliveries across destinations instead of naming a backlog.
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
    } else {
        m_summary->setText(QStringLiteral("%1 %2: %3 pending, %4 sent.")
                               .arg(shown, destinationName(destinationId))
                               .arg(m_store->pendingCount(destinationId))
                               .arg(m_store->sentCount(destinationId)));
    }
    updateActions();
}

QList<qint64> ListeningHistoryDialog::selectedIds() const
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

std::optional<ListenHistoryStore::HistoryRow> ListeningHistoryDialog::selectedHistoryRow() const
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

QString ListeningHistoryDialog::selectedDestinationId() const
{
    return m_destinationSelector == nullptr ? QString() : m_destinationSelector->currentData().toString();
}

QString ListeningHistoryDialog::destinationName(const QString &destinationId) const
{
    if (const ScrobbleDestination *destination = m_destinations.find(destinationId)) {
        return destination->name;
    }
    return destinationId;
}

void ListeningHistoryDialog::queueSelected(const QString &service)
{
    if (m_store == nullptr || service.isEmpty() || m_view == nullptr || m_view->selectionModel() == nullptr) {
        return;
    }
    const QList<qint64> ids = static_cast<ListeningHistoryModel *>(m_model)->idsQueueableForService(m_view->selectionModel()->selectedRows(), service);
    const int changed = m_store->markOwed(service, ids);
    reload();
    emit backlogChanged(service, changed);
    emit statusMessageRequested(QStringLiteral("Marked %1 history listens to scrobble to %2")
                                    .arg(changed)
                                    .arg(destinationName(service)),
                                5000);
}

void ListeningHistoryDialog::clearPending(const QString &service)
{
    if (m_store == nullptr || service.isEmpty()) {
        return;
    }
    const int cleared = m_store->clearPending(service);
    reload();
    emit backlogChanged(service, cleared);
    emit statusMessageRequested(QStringLiteral("Cleared %1 pending %2 scrobbles")
                                    .arg(cleared)
                                    .arg(destinationName(service)),
                                5000);
}

void ListeningHistoryDialog::forgetSelectedBehavior()
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

void ListeningHistoryDialog::updateActions()
{
    const bool hasSelection = !selectedIds().isEmpty();
    const std::optional<ListenHistoryStore::HistoryRow> selectedRow = selectedHistoryRow();
    const QModelIndexList selectedRows = m_view->selectionModel() == nullptr ? QModelIndexList{} : m_view->selectionModel()->selectedRows();
    auto *model = static_cast<ListeningHistoryModel *>(m_model);
    const QString destinationId = selectedDestinationId();
    // Aggregate mode is a read-only overview: with no destination chosen there
    // is no single backlog to queue into, clear, or retry.
    const bool concrete = !destinationId.isEmpty();
    const int pending = concrete && m_store != nullptr ? m_store->pendingCount(destinationId) : 0;

    m_queueSelected->setEnabled(concrete && hasSelection
                                && model->hasQueueableForService(selectedRows, destinationId));
    m_forgetBehavior->setEnabled(selectedRow.has_value() && !selectedRow->track.path.isEmpty());
    m_clearBacklog->setEnabled(concrete && pending > 0);
    m_retryPending->setEnabled(concrete && pending > 0);
}
