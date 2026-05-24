#include "ui/LinkRootsDialog.h"

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace {

enum Column {
    Name = 0,
    SourcePrefix,
    TargetPrefix,
    Priority,
    Readable,
    Writable,
    Enabled,
    Count,
};

QTableWidgetItem *textItem(const QString &text)
{
    return new QTableWidgetItem(text);
}

QTableWidgetItem *checkItem(bool checked)
{
    auto *item = new QTableWidgetItem;
    item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    return item;
}

bool itemChecked(const QTableWidgetItem *item)
{
    return item != nullptr && item->checkState() == Qt::Checked;
}

QString itemText(const QTableWidget *table, int row, int column)
{
    const QTableWidgetItem *item = table->item(row, column);
    return item != nullptr ? item->text().trimmed() : QString();
}

} // namespace

LinkRootsDialog::LinkRootsDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Link roots"));
    resize(900, 420);

    auto *layout = new QVBoxLayout(this);

    m_table = new QTableWidget(0, Count, this);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("Name"),
        QStringLiteral("Source prefix"),
        QStringLiteral("Target prefix"),
        QStringLiteral("Priority"),
        QStringLiteral("Read"),
        QStringLiteral("Write"),
        QStringLiteral("Enabled"),
    });
    m_table->horizontalHeader()->setSectionResizeMode(SourcePrefix, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(TargetPrefix, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(m_table, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    QPushButton *add = buttons->addButton(QStringLiteral("Add"), QDialogButtonBox::ActionRole);
    QPushButton *remove = buttons->addButton(QStringLiteral("Remove"), QDialogButtonBox::ActionRole);
    connect(add, &QPushButton::clicked, this, &LinkRootsDialog::addBlankRow);
    connect(remove, &QPushButton::clicked, this, &LinkRootsDialog::removeSelectedRows);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void LinkRootsDialog::setLinkRoots(const QVector<LinkRoot> &roots)
{
    m_table->setRowCount(0);
    for (const LinkRoot &root : roots) {
        addRootRow(root);
    }
}

QVector<LinkRoot> LinkRootsDialog::linkRoots() const
{
    QVector<LinkRoot> roots;
    roots.reserve(m_table->rowCount());
    for (int row = 0; row < m_table->rowCount(); ++row) {
        LinkRoot root;
        root.id = m_table->item(row, Name) != nullptr ? m_table->item(row, Name)->data(Qt::UserRole).toInt() : 0;
        root.name = itemText(m_table, row, Name);
        root.sourcePrefix = itemText(m_table, row, SourcePrefix);
        root.targetPrefix = itemText(m_table, row, TargetPrefix);
        root.priority = itemText(m_table, row, Priority).toInt();
        root.readable = itemChecked(m_table->item(row, Readable));
        root.writable = itemChecked(m_table->item(row, Writable));
        root.enabled = itemChecked(m_table->item(row, Enabled));
        if (!root.sourcePrefix.isEmpty() || !root.targetPrefix.isEmpty()) {
            roots.push_back(root);
        }
    }
    return roots;
}

void LinkRootsDialog::addRootRow(const LinkRoot &root)
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    QTableWidgetItem *name = textItem(root.name);
    name->setData(Qt::UserRole, root.id);
    m_table->setItem(row, Name, name);
    m_table->setItem(row, SourcePrefix, textItem(root.sourcePrefix));
    m_table->setItem(row, TargetPrefix, textItem(root.targetPrefix));
    m_table->setItem(row, Priority, textItem(QString::number(root.priority)));
    m_table->setItem(row, Readable, checkItem(root.readable));
    m_table->setItem(row, Writable, checkItem(root.writable));
    m_table->setItem(row, Enabled, checkItem(root.enabled));
}

void LinkRootsDialog::addBlankRow()
{
    LinkRoot root;
    root.name = QStringLiteral("Link root");
    root.priority = 100 + (m_table->rowCount() * 10);
    addRootRow(root);
}

void LinkRootsDialog::removeSelectedRows()
{
    QList<int> rows;
    for (const QModelIndex &index : m_table->selectionModel()->selectedRows()) {
        rows.push_back(index.row());
    }
    std::sort(rows.begin(), rows.end(), std::greater<>());
    for (int row : rows) {
        m_table->removeRow(row);
    }
}
