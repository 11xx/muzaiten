#include "ui/ListeningHistoryDialog.h"

#include "core/HumanQuantity.h"
#include "ui/DenseTableDelegate.h"
#include "ui/NeighborColumnResizer.h"
#include "ui/ResponsiveColumnLayout.h"
#include "ui/RowHeightWheel.h"

#include <QAbstractTableModel>
#include <QAbstractItemView>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QEvent>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QItemSelectionModel>
#include <QLabel>
#include <QPushButton>
#include <QSet>
#include <QTableView>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>

namespace {

constexpr int kHistoryLimit = 5000;

enum Column {
    TimeColumn,
    TitleColumn,
    ArtistColumn,
    AlbumColumn,
    DurationColumn,
    LastFmColumn,
    ListenBrainzColumn,
    PathColumn,
    ColumnCount,
};

QString serviceStatus(bool owed, bool sent)
{
    if (sent) {
        return QStringLiteral("Sent");
    }
    if (owed) {
        return QStringLiteral("Pending");
    }
    return QStringLiteral("Not queued");
}

bool rowQueueableForService(const ListenHistoryStore::HistoryRow &row, const QString &service)
{
    if (service == ListenHistoryStore::LastFm) {
        return !row.sentLastFm && !row.owedLastFm;
    }
    if (service == ListenHistoryStore::ListenBrainz) {
        return !row.sentListenBrainz && !row.owedListenBrainz;
    }
    return false;
}

QVector<ResponsiveColumnSpec> historyResponsiveSpecs()
{
    return {
        {TimeColumn, QStringLiteral("time"), 145, 120, ResponsiveColumnPriority::Keep},
        {TitleColumn, QStringLiteral("title"), 260, 140, ResponsiveColumnPriority::Keep, true},
        {ArtistColumn, QStringLiteral("artist"), 200, 110, ResponsiveColumnPriority::Keep},
        {AlbumColumn, QStringLiteral("album"), 220, 110, ResponsiveColumnPriority::Normal},
        {DurationColumn, QStringLiteral("duration"), 80, 70, ResponsiveColumnPriority::Normal},
        {LastFmColumn, QStringLiteral("lastfm"), 105, 90, ResponsiveColumnPriority::Keep},
        {ListenBrainzColumn, QStringLiteral("listenbrainz"), 120, 100, ResponsiveColumnPriority::Keep},
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
        case LastFmColumn:
            return QStringLiteral("Last.fm");
        case ListenBrainzColumn:
            return QStringLiteral("ListenBrainz");
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
        case LastFmColumn:
            return serviceStatus(row.owedLastFm, row.sentLastFm);
        case ListenBrainzColumn:
            return serviceStatus(row.owedListenBrainz, row.sentListenBrainz);
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

private:
    QList<ListenHistoryStore::HistoryRow> m_rows;
};

} // namespace

ListeningHistoryDialog::ListeningHistoryDialog(ListenHistoryStore *store, QWidget *parent)
    : QDialog(parent)
    , m_store(store)
{
    setWindowTitle(QStringLiteral("Listening history"));
    resize(1100, 650);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

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
    m_queueLastFm = new QPushButton(QStringLiteral("Queue selected for Last.fm"), this);
    m_queueListenBrainz = new QPushButton(QStringLiteral("Queue selected for ListenBrainz"), this);
    m_clearLastFm = new QPushButton(QStringLiteral("Clear Last.fm backlog"), this);
    m_clearListenBrainz = new QPushButton(QStringLiteral("Clear ListenBrainz backlog"), this);
    auto *refresh = new QPushButton(QStringLiteral("Refresh"), this);
    actions->addWidget(m_queueLastFm);
    actions->addWidget(m_queueListenBrainz);
    actions->addStretch();
    actions->addWidget(m_clearLastFm);
    actions->addWidget(m_clearListenBrainz);
    actions->addWidget(refresh);
    layout->addLayout(actions);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(refresh, &QPushButton::clicked, this, &ListeningHistoryDialog::reload);
    connect(m_queueLastFm, &QPushButton::clicked, this, [this]() { queueSelected(ListenHistoryStore::LastFm); });
    connect(m_queueListenBrainz, &QPushButton::clicked, this, [this]() { queueSelected(ListenHistoryStore::ListenBrainz); });
    connect(m_clearLastFm, &QPushButton::clicked, this, [this]() { clearPending(ListenHistoryStore::LastFm); });
    connect(m_clearListenBrainz, &QPushButton::clicked, this, [this]() { clearPending(ListenHistoryStore::ListenBrainz); });
    connect(m_view->selectionModel(), &QItemSelectionModel::selectionChanged, this, [this]() { updateActions(); });

    reload();
}

bool ListeningHistoryDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_view->viewport() && event->type() == QEvent::Wheel) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        if (ui::applyCtrlWheelRowHeight(wheel, m_view->verticalHeader()->defaultSectionSize(), 18, 48,
                [this](int h) { m_view->verticalHeader()->setDefaultSectionSize(h); })) {
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
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
    const int lastFmPending = m_store->pendingCount(ListenHistoryStore::LastFm);
    const int listenBrainzPending = m_store->pendingCount(ListenHistoryStore::ListenBrainz);
    m_summary->setText(QStringLiteral("Showing latest %1 of %2 listens. Pending: Last.fm %3, ListenBrainz %4.")
                           .arg(std::min(kHistoryLimit, total))
                           .arg(total)
                           .arg(lastFmPending)
                           .arg(listenBrainzPending));
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

void ListeningHistoryDialog::queueSelected(const QString &service)
{
    if (m_store == nullptr || m_view == nullptr || m_view->selectionModel() == nullptr) {
        return;
    }
    const QList<qint64> ids = static_cast<ListeningHistoryModel *>(m_model)->idsQueueableForService(m_view->selectionModel()->selectedRows(), service);
    const int changed = m_store->markOwed(service, ids);
    reload();
    emit backlogChanged(service, changed);
    emit statusMessageRequested(QStringLiteral("Queued %1 history listens for %2")
                                    .arg(changed)
                                    .arg(service == ListenHistoryStore::LastFm ? QStringLiteral("Last.fm") : QStringLiteral("ListenBrainz")),
                                5000);
}

void ListeningHistoryDialog::clearPending(const QString &service)
{
    if (m_store == nullptr) {
        return;
    }
    const int cleared = m_store->clearPending(service);
    reload();
    emit backlogChanged(service, cleared);
    emit statusMessageRequested(QStringLiteral("Cleared %1 pending %2 scrobbles")
                                    .arg(cleared)
                                    .arg(service == ListenHistoryStore::LastFm ? QStringLiteral("Last.fm") : QStringLiteral("ListenBrainz")),
                                5000);
}

void ListeningHistoryDialog::updateActions()
{
    const bool hasSelection = !selectedIds().isEmpty();
    const QModelIndexList selectedRows = m_view->selectionModel() == nullptr ? QModelIndexList{} : m_view->selectionModel()->selectedRows();
    auto *model = static_cast<ListeningHistoryModel *>(m_model);
    m_queueLastFm->setEnabled(hasSelection && model->hasQueueableForService(selectedRows, ListenHistoryStore::LastFm));
    m_queueListenBrainz->setEnabled(hasSelection && model->hasQueueableForService(selectedRows, ListenHistoryStore::ListenBrainz));
    const bool hasStore = m_store != nullptr && m_store->isOpen();
    m_clearLastFm->setEnabled(hasStore && m_store->pendingCount(ListenHistoryStore::LastFm) > 0);
    m_clearListenBrainz->setEnabled(hasStore && m_store->pendingCount(ListenHistoryStore::ListenBrainz) > 0);
}
