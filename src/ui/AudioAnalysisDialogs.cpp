#include "ui/AudioAnalysisDialogs.h"

#include "db/Database.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace {

QString yesNo(bool value)
{
    return value ? QStringLiteral("Yes") : QStringLiteral("No");
}

QString countText(qint64 value)
{
    return QLocale().toString(value);
}

QLabel *valueLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    return label;
}

QTableWidgetItem *textItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return item;
}

QTableWidgetItem *numericItem(qint64 value)
{
    auto *item = new QTableWidgetItem;
    item->setData(Qt::DisplayRole, value);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return item;
}

} // namespace

AudioAnalysisStatusDialog::AudioAnalysisStatusDialog(const QString &featuresPath, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Analysis status"));
    resize(560, 360);

    auto *root = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    root->addLayout(form, 1);

    const AudioAnalysisData::StatusSummary summary = AudioAnalysisData::loadStatus(featuresPath);
    form->addRow(QStringLiteral("Features path"), valueLabel(summary.path, this));
    form->addRow(QStringLiteral("Present"), valueLabel(yesNo(summary.found), this));
    form->addRow(QStringLiteral("Readable"), valueLabel(yesNo(summary.open), this));
    if (!summary.message.isEmpty()) {
        form->addRow(QStringLiteral("Status"), valueLabel(summary.message, this));
    }
    if (summary.open) {
        const FeatureStore::Status status = summary.status;
        form->addRow(QStringLiteral("Schema version"), valueLabel(QString::number(summary.schemaVersion), this));
        form->addRow(QStringLiteral("DSP version"),
                     valueLabel(status.dspVersion.isEmpty() ? QStringLiteral("unknown") : status.dspVersion, this));
        form->addRow(QStringLiteral("Files"),
                     valueLabel(QStringLiteral("%1 (%2 ok, %3 failed)")
                                    .arg(countText(status.files), countText(status.ok), countText(status.failed)),
                                this));
        form->addRow(QStringLiteral("Content groups"), valueLabel(countText(status.groups), this));
        form->addRow(QStringLiteral("Featured groups"), valueLabel(countText(status.featured), this));
        QString embeddingText = countText(status.embeddedGroups);
        if (!status.embeddingModel.isEmpty() || !status.embeddingVersion.isEmpty()) {
            embeddingText += QStringLiteral(" (%1 %2)")
                                 .arg(status.embeddingModel.isEmpty() ? QStringLiteral("unknown-model") : status.embeddingModel,
                                      status.embeddingVersion.isEmpty() ? QStringLiteral("unknown-version") : status.embeddingVersion);
        }
        form->addRow(QStringLiteral("Embedded groups"), valueLabel(embeddingText, this));
        form->addRow(QStringLiteral("Neighbor rows"), valueLabel(countText(status.neighborRows), this));
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    root->addWidget(buttons);
}

DuplicateCopiesDialog::DuplicateCopiesDialog(Database *db, const QString &featuresPath, QWidget *parent)
    : QDialog(parent)
    , m_db(db)
    , m_featuresPath(featuresPath)
{
    setWindowTitle(QStringLiteral("Duplicate copies"));
    resize(980, 560);

    auto *root = new QVBoxLayout(this);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    auto *splitter = new QSplitter(this);
    root->addWidget(splitter, 1);

    m_groupsTable = new QTableWidget(0, 3, splitter);
    m_groupsTable->setHorizontalHeaderLabels({
        QStringLiteral("Group"),
        QStringLiteral("Copies"),
        QStringLiteral("Best copy"),
    });
    m_groupsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_groupsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_groupsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_groupsTable->verticalHeader()->setVisible(false);
    m_groupsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_groupsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_groupsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    splitter->addWidget(m_groupsTable);

    m_copiesTable = new QTableWidget(0, 6, splitter);
    m_copiesTable->setHorizontalHeaderLabels({
        QStringLiteral("Best"),
        QStringLiteral("Pinned"),
        QStringLiteral("Quality"),
        QStringLiteral("Title"),
        QStringLiteral("Artist"),
        QStringLiteral("Path"),
    });
    m_copiesTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_copiesTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_copiesTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_copiesTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_copiesTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_copiesTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_copiesTable->verticalHeader()->setVisible(false);
    m_copiesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_copiesTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_copiesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    splitter->addWidget(m_copiesTable);
    splitter->setSizes({320, 660});

    auto *actions = new QHBoxLayout;
    m_pinButton = new QPushButton(QStringLiteral("Pin selected copy"), this);
    m_unpinButton = new QPushButton(QStringLiteral("Unpin group"), this);
    actions->addWidget(m_pinButton);
    actions->addWidget(m_unpinButton);
    actions->addStretch(1);
    root->addLayout(actions);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(m_groupsTable, &QTableWidget::currentCellChanged, this, [this](int row) { showGroup(row); });
    connect(m_copiesTable, &QTableWidget::currentCellChanged, this, [this]() { updateButtons(); });
    connect(m_pinButton, &QPushButton::clicked, this, &DuplicateCopiesDialog::pinSelectedCopy);
    connect(m_unpinButton, &QPushButton::clicked, this, &DuplicateCopiesDialog::unpinSelectedGroup);

    refresh();
}

void DuplicateCopiesDialog::refresh()
{
    m_groups.clear();
    m_groupsTable->setRowCount(0);
    m_copiesTable->setRowCount(0);
    m_pinButton->setEnabled(false);
    m_unpinButton->setEnabled(false);

    if (m_db == nullptr) {
        m_status->setText(QStringLiteral("Library database is unavailable."));
        return;
    }
    if (!QFileInfo::exists(m_featuresPath)) {
        m_status->setText(QStringLiteral("features.sqlite not found at %1").arg(m_featuresPath));
        return;
    }
    FeatureStore features(m_featuresPath);
    if (!features.isOpen()) {
        m_status->setText(QStringLiteral("features.sqlite is unsupported or unreadable at %1").arg(m_featuresPath));
        return;
    }

    m_groups = AudioAnalysisData::loadDuplicateGroups(*m_db, features, 2, 200);
    if (m_groups.isEmpty()) {
        m_status->setText(QStringLiteral("No duplicate groups found."));
        return;
    }
    m_status->setText(QStringLiteral("Showing up to 200 duplicate groups from %1.").arg(m_featuresPath));

    for (const AudioAnalysisData::DuplicateGroup &group : m_groups) {
        const int row = m_groupsTable->rowCount();
        m_groupsTable->insertRow(row);
        m_groupsTable->setItem(row, 0, numericItem(group.groupId));
        m_groupsTable->setItem(row, 1, numericItem(static_cast<qint64>(group.copies.size())));
        m_groupsTable->setItem(row, 2, textItem(group.bestPath));
    }
    m_groupsTable->setCurrentCell(0, 0);
}

void DuplicateCopiesDialog::showGroup(int row)
{
    m_copiesTable->setRowCount(0);
    if (row < 0 || static_cast<qsizetype>(row) >= m_groups.size()) {
        updateButtons();
        return;
    }

    const AudioAnalysisData::DuplicateGroup &group = m_groups.at(row);
    for (const AudioAnalysisData::DuplicateCopy &copy : group.copies) {
        const int copyRow = m_copiesTable->rowCount();
        m_copiesTable->insertRow(copyRow);
        auto *best = textItem(copy.best ? QStringLiteral("Best") : QString());
        best->setData(Qt::UserRole, copy.path);
        m_copiesTable->setItem(copyRow, 0, best);
        m_copiesTable->setItem(copyRow, 1, textItem(copy.pinned ? QStringLiteral("Pinned") : QString()));
        m_copiesTable->setItem(copyRow, 2, textItem(copy.qualitySummary));
        m_copiesTable->setItem(copyRow, 3, textItem(AudioAnalysisData::copyDisplayTitle(copy)));
        m_copiesTable->setItem(copyRow, 4, textItem(copy.artist));
        m_copiesTable->setItem(copyRow, 5, textItem(copy.path));
    }
    if (m_copiesTable->rowCount() > 0) {
        m_copiesTable->setCurrentCell(0, 0);
    }
    updateButtons();
}

void DuplicateCopiesDialog::pinSelectedCopy()
{
    const qint64 groupId = selectedGroupId();
    const QString path = selectedCopyPath();
    if (groupId < 0 || path.isEmpty() || m_db == nullptr) {
        return;
    }
    if (!m_db->setContentGroupPin(groupId, path)) {
        QMessageBox::warning(this, QStringLiteral("Duplicate copies"), m_db->lastError());
        return;
    }
    refresh();
}

void DuplicateCopiesDialog::unpinSelectedGroup()
{
    const qint64 groupId = selectedGroupId();
    if (groupId < 0 || m_db == nullptr) {
        return;
    }
    if (!m_db->removeContentGroupPin(groupId)) {
        QMessageBox::warning(this, QStringLiteral("Duplicate copies"), m_db->lastError());
        return;
    }
    refresh();
}

void DuplicateCopiesDialog::updateButtons()
{
    const qint64 groupId = selectedGroupId();
    const QString path = selectedCopyPath();
    const bool hasGroup = groupId >= 0;
    m_pinButton->setEnabled(hasGroup && !path.isEmpty());
    m_unpinButton->setEnabled(hasGroup);
}

qint64 DuplicateCopiesDialog::selectedGroupId() const
{
    const int row = m_groupsTable == nullptr ? -1 : m_groupsTable->currentRow();
    if (row < 0 || static_cast<qsizetype>(row) >= m_groups.size()) {
        return -1;
    }
    return m_groups.at(row).groupId;
}

QString DuplicateCopiesDialog::selectedCopyPath() const
{
    if (m_copiesTable == nullptr || m_copiesTable->currentRow() < 0) {
        return {};
    }
    const QTableWidgetItem *item = m_copiesTable->item(m_copiesTable->currentRow(), 5);
    return item == nullptr ? QString() : item->text();
}
