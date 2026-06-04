#include "ui/PanelOrderDialog.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QVBoxLayout>

namespace {

constexpr int kPanelIdRole = Qt::UserRole;

QString panelLabel(MainPanelId id)
{
    switch (id) {
    case MainPanelId::Queue:   return QStringLiteral("Queue");
    case MainPanelId::Artists: return QStringLiteral("Artists");
    case MainPanelId::Albums:  return QStringLiteral("Albums");
    case MainPanelId::Tracks:  return QStringLiteral("Tracks");
    }
    return QStringLiteral("Panel");
}

void addPanelRow(QListWidget *list, MainPanelId id, bool checked)
{
    auto *item = new QListWidgetItem(panelLabel(id), list);
    item->setData(kPanelIdRole, static_cast<int>(id));
    item->setFlags((item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled)
                   & ~Qt::ItemIsDropEnabled);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
}

} // namespace

PanelOrderDialog::PanelOrderDialog(const QVector<MainPanelId> &currentOrder, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Panel order"));

    auto *layout = new QVBoxLayout(this);

    auto *hint = new QLabel(
        QStringLiteral("Drag to reorder how h/l moves between panels on the main view. "
                       "Uncheck a panel to remove it from focus cycling."),
        this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_list = new QListWidget(this);
    m_list->setDragDropMode(QAbstractItemView::InternalMove);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(m_list, 1);

    // Checked panels first, in their saved order, then any remaining (removed)
    // panels unchecked so they can be re-enabled and repositioned.
    for (MainPanelId id : currentOrder) {
        addPanelRow(m_list, id, true);
    }
    for (MainPanelId id : defaultMainPanelFocusOrder()) {
        if (!currentOrder.contains(id)) {
            addPanelRow(m_list, id, false);
        }
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QVector<MainPanelId> PanelOrderDialog::resultOrder() const
{
    QVector<MainPanelId> order;
    for (int row = 0; row < m_list->count(); ++row) {
        const QListWidgetItem *item = m_list->item(row);
        if (item->checkState() == Qt::Checked) {
            order.push_back(static_cast<MainPanelId>(item->data(kPanelIdRole).toInt()));
        }
    }
    return order.isEmpty() ? defaultMainPanelFocusOrder() : order;
}
