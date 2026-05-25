#include "ui/SourceDirectoriesDialog.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace {

enum Column {
    Name = 0,
    Path,
    Scan,
    Library,
    Status,
    LastScanned,
    Count,
};

QString cleanPath(const QString &path)
{
    const QString cleaned = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    if (cleaned.size() > 1 && cleaned.endsWith(QLatin1Char('/'))) {
        return cleaned.left(cleaned.size() - 1);
    }
    return cleaned;
}

bool isChildOf(const QString &child, const QString &parent)
{
    return child != parent && child.startsWith(parent + QLatin1Char('/'));
}

QTableWidgetItem *textItem(const QString &text)
{
    return new QTableWidgetItem(text);
}

QTableWidgetItem *readonlyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return item;
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

SourceDirectoriesDialog::SourceDirectoriesDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Source directories"));
    resize(980, 460);

    auto *layout = new QVBoxLayout(this);
    auto *description = new QLabel(QStringLiteral(
        "Source directories are reusable shortcuts for scanning local music into the library. "
        "Disabling library access hides matching files without deleting metadata."));
    description->setWordWrap(true);
    layout->addWidget(description);

    m_table = new QTableWidget(0, Count, this);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("Name"),
        QStringLiteral("Path"),
        QStringLiteral("Scan"),
        QStringLiteral("Library"),
        QStringLiteral("Status"),
        QStringLiteral("Last scanned"),
    });
    m_table->horizontalHeader()->setSectionResizeMode(Name, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(Path, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(Status, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(LastScanned, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    layout->addWidget(m_table, 1);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    QPushButton *add = buttons->addButton(QStringLiteral("Add..."), QDialogButtonBox::ActionRole);
    QPushButton *remove = buttons->addButton(QStringLiteral("Remove"), QDialogButtonBox::ActionRole);
    QPushButton *scanSelected = buttons->addButton(QStringLiteral("Scan Selected"), QDialogButtonBox::ActionRole);
    QPushButton *scanEnabled = buttons->addButton(QStringLiteral("Scan Enabled"), QDialogButtonBox::ActionRole);
    connect(add, &QPushButton::clicked, this, &SourceDirectoriesDialog::addDirectory);
    connect(remove, &QPushButton::clicked, this, &SourceDirectoriesDialog::removeSelectedRows);
    connect(scanSelected, &QPushButton::clicked, this, &SourceDirectoriesDialog::scanSelectedRows);
    connect(scanEnabled, &QPushButton::clicked, this, &SourceDirectoriesDialog::scanEnabledRows);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_table, &QTableWidget::itemChanged, this, &SourceDirectoriesDialog::updateStatuses);
}

void SourceDirectoriesDialog::setScanRoots(const QVector<ScanRoot> &roots)
{
    m_table->setRowCount(0);
    for (const ScanRoot &root : roots) {
        addRootRow(root);
    }
    updateStatuses();
}

QVector<ScanRoot> SourceDirectoriesDialog::scanRoots() const
{
    QVector<ScanRoot> roots;
    roots.reserve(m_table->rowCount());
    for (int row = 0; row < m_table->rowCount(); ++row) {
        ScanRoot root;
        root.id = m_table->item(row, Name) != nullptr ? m_table->item(row, Name)->data(Qt::UserRole).toInt() : 0;
        root.name = itemText(m_table, row, Name);
        root.path = itemText(m_table, row, Path);
        root.scanEnabled = itemChecked(m_table->item(row, Scan));
        root.libraryEnabled = itemChecked(m_table->item(row, Library));
        if (!root.path.isEmpty()) {
            roots.push_back(root);
        }
    }
    return roots;
}

void SourceDirectoriesDialog::addDirectory()
{
    const QString path = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose source directory"));
    if (path.isEmpty()) {
        return;
    }

    const QFileInfo info(path);
    ScanRoot root;
    root.name = info.fileName().isEmpty() ? cleanPath(path) : info.fileName();
    root.path = cleanPath(path);
    root.scanEnabled = true;
    root.libraryEnabled = true;
    addRootRow(root);
    updateStatuses();
}

void SourceDirectoriesDialog::removeSelectedRows()
{
    QList<int> rows;
    for (const QModelIndex &index : m_table->selectionModel()->selectedRows()) {
        rows.push_back(index.row());
    }
    std::sort(rows.begin(), rows.end(), std::greater<>());
    for (int row : rows) {
        m_table->removeRow(row);
    }
    updateStatuses();
}

void SourceDirectoriesDialog::scanSelectedRows()
{
    QVector<ScanRoot> roots;
    for (const QModelIndex &index : m_table->selectionModel()->selectedRows()) {
        const int row = index.row();
        if (!itemChecked(m_table->item(row, Scan))) {
            continue;
        }
        ScanRoot root;
        root.id = m_table->item(row, Name) != nullptr ? m_table->item(row, Name)->data(Qt::UserRole).toInt() : 0;
        root.name = itemText(m_table, row, Name);
        root.path = itemText(m_table, row, Path);
        root.scanEnabled = true;
        root.libraryEnabled = itemChecked(m_table->item(row, Library));
        roots.push_back(root);
    }
    if (!roots.isEmpty()) {
        emit scanRootsRequested(roots);
    }
}

void SourceDirectoriesDialog::scanEnabledRows()
{
    QVector<ScanRoot> roots;
    for (const ScanRoot &root : scanRoots()) {
        if (root.scanEnabled) {
            roots.push_back(root);
        }
    }
    if (!roots.isEmpty()) {
        emit scanRootsRequested(roots);
    }
}

void SourceDirectoriesDialog::addRootRow(const ScanRoot &root)
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    QTableWidgetItem *name = textItem(root.name);
    name->setData(Qt::UserRole, root.id);
    m_table->setItem(row, Name, name);
    m_table->setItem(row, Path, textItem(cleanPath(root.path)));
    m_table->setItem(row, Scan, checkItem(root.scanEnabled));
    m_table->setItem(row, Library, checkItem(root.libraryEnabled));
    m_table->setItem(row, Status, readonlyItem(root.lastError.isEmpty() ? QStringLiteral("Ready") : root.lastError));
    m_table->setItem(row, LastScanned, readonlyItem(root.lastScannedAt));
}

void SourceDirectoriesDialog::updateStatuses()
{
    if (m_table == nullptr) {
        return;
    }

    const QSignalBlocker blocker(m_table);
    QStringList warnings;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QString path = cleanPath(itemText(m_table, row, Path));
        QString status = QStringLiteral("Ready");
        for (int other = 0; other < m_table->rowCount(); ++other) {
            if (row == other) {
                continue;
            }
            const QString otherPath = cleanPath(itemText(m_table, other, Path));
            if (isChildOf(path, otherPath) && itemChecked(m_table->item(other, Scan)) && itemChecked(m_table->item(row, Scan))) {
                status = QStringLiteral("Covered by %1 during full rescan").arg(otherPath);
                break;
            }
            if (isChildOf(otherPath, path) && itemChecked(m_table->item(other, Library)) && !itemChecked(m_table->item(row, Library))) {
                status = QStringLiteral("Child roots may remain visible if enabled");
            }
        }
        if (m_table->item(row, Status) != nullptr) {
            m_table->item(row, Status)->setText(status);
        }
        if (status != QStringLiteral("Ready")) {
            warnings.push_back(status);
        }
    }
    warnings.removeDuplicates();
    m_status->setText(warnings.isEmpty() ? QString() : warnings.join(QStringLiteral("  ")));
}
