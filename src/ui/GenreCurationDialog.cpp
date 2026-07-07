#include "ui/GenreCurationDialog.h"

#include "db/Database.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>

namespace {

QTableWidgetItem *numericItem(int value)
{
    auto *item = new QTableWidgetItem;
    item->setData(Qt::DisplayRole, value);
    return item;
}

QString selectedTableText(const QTableWidget *table, int column)
{
    if (table == nullptr || table->currentRow() < 0) {
        return {};
    }
    const QTableWidgetItem *item = table->item(table->currentRow(), column);
    return item == nullptr ? QString() : item->text();
}

} // namespace

GenreCurationDialog::GenreCurationDialog(Database *db, QWidget *parent)
    : QDialog(parent)
    , m_db(db)
{
    setWindowTitle(QStringLiteral("Genre curation"));
    resize(820, 560);

    auto *root = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    root->addWidget(tabs, 1);

    auto *aliasesPage = new QWidget(tabs);
    auto *aliasesLayout = new QVBoxLayout(aliasesPage);
    m_aliases = new QTableWidget(0, 2, aliasesPage);
    m_aliases->setHorizontalHeaderLabels({QStringLiteral("Alias"), QStringLiteral("Canonical")});
    m_aliases->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_aliases->verticalHeader()->setVisible(false);
    m_aliases->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_aliases->setSelectionMode(QAbstractItemView::SingleSelection);
    aliasesLayout->addWidget(m_aliases, 1);

    auto *aliasButtons = new QHBoxLayout;
    auto *addAliasButton = new QPushButton(QStringLiteral("Add"), aliasesPage);
    auto *editAliasButton = new QPushButton(QStringLiteral("Edit"), aliasesPage);
    auto *removeAliasButton = new QPushButton(QStringLiteral("Remove"), aliasesPage);
    aliasButtons->addWidget(addAliasButton);
    aliasButtons->addWidget(editAliasButton);
    aliasButtons->addWidget(removeAliasButton);
    aliasButtons->addStretch(1);
    aliasesLayout->addLayout(aliasButtons);
    tabs->addTab(aliasesPage, QStringLiteral("Aliases"));

    auto *ignoredPage = new QWidget(tabs);
    auto *ignoredLayout = new QVBoxLayout(ignoredPage);
    m_ignored = new QListWidget(ignoredPage);
    m_ignored->setSelectionMode(QAbstractItemView::SingleSelection);
    ignoredLayout->addWidget(m_ignored, 1);
    auto *ignoredButtons = new QHBoxLayout;
    auto *addIgnoredButton = new QPushButton(QStringLiteral("Add"), ignoredPage);
    auto *removeIgnoredButton = new QPushButton(QStringLiteral("Remove"), ignoredPage);
    ignoredButtons->addWidget(addIgnoredButton);
    ignoredButtons->addWidget(removeIgnoredButton);
    ignoredButtons->addStretch(1);
    ignoredLayout->addLayout(ignoredButtons);
    tabs->addTab(ignoredPage, QStringLiteral("Ignored radio genres"));

    auto *vocabularyPage = new QWidget(tabs);
    auto *vocabularyLayout = new QVBoxLayout(vocabularyPage);
    m_vocabularyFilter = new QLineEdit(vocabularyPage);
    m_vocabularyFilter->setPlaceholderText(QStringLiteral("Filter genres"));
    vocabularyLayout->addWidget(m_vocabularyFilter);
    m_vocabulary = new QTableWidget(0, 6, vocabularyPage);
    m_vocabulary->setHorizontalHeaderLabels({
        QStringLiteral("Genre"),
        QStringLiteral("Tracks"),
        QStringLiteral("Canonical"),
        QStringLiteral("Status"),
        QStringLiteral("Sample artists"),
        QStringLiteral("Flags"),
    });
    m_vocabulary->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_vocabulary->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_vocabulary->verticalHeader()->setVisible(false);
    m_vocabulary->setSortingEnabled(true);
    m_vocabulary->setEditTriggers(QAbstractItemView::NoEditTriggers);
    vocabularyLayout->addWidget(m_vocabulary, 1);
    tabs->addTab(vocabularyPage, QStringLiteral("Vocabulary"));

    auto *note = new QLabel(QStringLiteral("Changes apply to the next radio session."), this);
    root->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(addAliasButton, &QPushButton::clicked, this, &GenreCurationDialog::addAlias);
    connect(editAliasButton, &QPushButton::clicked, this, &GenreCurationDialog::editAlias);
    connect(removeAliasButton, &QPushButton::clicked, this, &GenreCurationDialog::removeAlias);
    connect(addIgnoredButton, &QPushButton::clicked, this, &GenreCurationDialog::addIgnoredGenre);
    connect(removeIgnoredButton, &QPushButton::clicked, this, &GenreCurationDialog::removeIgnoredGenre);
    connect(m_vocabularyFilter, &QLineEdit::textChanged, this, &GenreCurationDialog::refreshVocabulary);

    refreshAliases();
    refreshIgnored();
    int taggedTrackTotal = 0;
    m_vocabularyRows = m_db == nullptr ? QVector<GenreCuration::ReportRow>()
                                       : GenreCuration::buildReportRows(*m_db, &taggedTrackTotal);
    Q_UNUSED(taggedTrackTotal);
    refreshVocabulary();
}

void GenreCurationDialog::refreshAliases()
{
    if (m_db == nullptr || m_aliases == nullptr) {
        return;
    }
    const QHash<QString, QString> aliases = m_db->genreAliases();
    QStringList keys = aliases.keys();
    keys.sort(Qt::CaseInsensitive);
    m_aliases->setRowCount(0);
    for (const QString &alias : keys) {
        const int row = m_aliases->rowCount();
        m_aliases->insertRow(row);
        m_aliases->setItem(row, 0, new QTableWidgetItem(alias));
        m_aliases->setItem(row, 1, new QTableWidgetItem(aliases.value(alias)));
    }
}

void GenreCurationDialog::refreshIgnored()
{
    if (m_db == nullptr || m_ignored == nullptr) {
        return;
    }
    QStringList ignored;
    for (const QString &genre : m_db->ignoredRadioGenres()) {
        ignored.push_back(genre);
    }
    ignored.sort(Qt::CaseInsensitive);
    m_ignored->clear();
    m_ignored->addItems(ignored);
}

void GenreCurationDialog::refreshVocabulary()
{
    if (m_vocabulary == nullptr || m_vocabularyFilter == nullptr) {
        return;
    }
    const QString filter = m_vocabularyFilter->text().simplified().toCaseFolded();
    m_vocabulary->setSortingEnabled(false);
    m_vocabulary->setRowCount(0);
    for (const GenreCuration::ReportRow &rowData : m_vocabularyRows) {
        const QString haystack = QStringList{
            rowData.genre,
            rowData.canonical,
            rowData.status,
            rowData.sampleArtists.join(QLatin1Char(' ')),
            rowData.flags.join(QLatin1Char(' ')),
        }.join(QLatin1Char(' ')).toCaseFolded();
        if (!filter.isEmpty() && !haystack.contains(filter)) {
            continue;
        }
        const int row = m_vocabulary->rowCount();
        m_vocabulary->insertRow(row);
        m_vocabulary->setItem(row, 0, new QTableWidgetItem(rowData.genre));
        m_vocabulary->setItem(row, 1, numericItem(rowData.df));
        m_vocabulary->setItem(row, 2, new QTableWidgetItem(rowData.canonical));
        m_vocabulary->setItem(row, 3, new QTableWidgetItem(rowData.status));
        m_vocabulary->setItem(row, 4, new QTableWidgetItem(rowData.sampleArtists.join(QStringLiteral(", "))));
        m_vocabulary->setItem(row, 5, new QTableWidgetItem(rowData.flags.join(QStringLiteral(", "))));
    }
    m_vocabulary->setSortingEnabled(true);
    m_vocabulary->sortByColumn(1, Qt::DescendingOrder);
}

void GenreCurationDialog::addAlias()
{
    bool ok = false;
    const QString alias = QInputDialog::getText(this, QStringLiteral("Add genre alias"),
                                                QStringLiteral("Alias genre:"), QLineEdit::Normal, {}, &ok);
    if (!ok) {
        return;
    }
    const QString canonical = QInputDialog::getText(this, QStringLiteral("Add genre alias"),
                                                    QStringLiteral("Canonical genre:"), QLineEdit::Normal, {}, &ok);
    if (!ok) {
        return;
    }
    setAlias(alias, canonical);
}

void GenreCurationDialog::editAlias()
{
    const QString currentAlias = selectedTableText(m_aliases, 0);
    if (currentAlias.isEmpty()) {
        return;
    }
    bool ok = false;
    const QString alias = QInputDialog::getText(this, QStringLiteral("Edit genre alias"),
                                                QStringLiteral("Alias genre:"), QLineEdit::Normal,
                                                currentAlias, &ok);
    if (!ok) {
        return;
    }
    const QString canonical = QInputDialog::getText(this, QStringLiteral("Edit genre alias"),
                                                    QStringLiteral("Canonical genre:"), QLineEdit::Normal,
                                                    selectedTableText(m_aliases, 1), &ok);
    if (!ok) {
        return;
    }
    const GenreCuration::AliasValidation validation = GenreCuration::validateAlias(alias, canonical);
    if (!validation.ok()) {
        showError(validation.error);
        return;
    }
    if (validation.aliasFolded != currentAlias && !m_db->removeGenreAlias(currentAlias)) {
        showError(m_db->lastError());
        return;
    }
    setAlias(validation.aliasFolded, validation.canonicalFolded);
}

void GenreCurationDialog::removeAlias()
{
    if (m_db == nullptr) {
        return;
    }
    const QString alias = selectedTableText(m_aliases, 0);
    if (alias.isEmpty()) {
        return;
    }
    if (!m_db->removeGenreAlias(alias)) {
        showError(m_db->lastError());
        return;
    }
    refreshAliases();
    m_vocabularyRows = GenreCuration::buildReportRows(*m_db);
    refreshVocabulary();
}

void GenreCurationDialog::addIgnoredGenre()
{
    if (m_db == nullptr) {
        return;
    }
    bool ok = false;
    const QString genre = QInputDialog::getText(this, QStringLiteral("Ignore radio genre"),
                                                QStringLiteral("Genre:"), QLineEdit::Normal, {}, &ok);
    if (!ok) {
        return;
    }
    QString error;
    const QString canonical = GenreCuration::canonicalGenreInput(*m_db, genre, &error);
    if (!error.isEmpty()) {
        showError(error);
        return;
    }
    if (!m_db->setRadioGenreIgnored(canonical, true)) {
        showError(m_db->lastError());
        return;
    }
    refreshIgnored();
    m_vocabularyRows = GenreCuration::buildReportRows(*m_db);
    refreshVocabulary();
}

void GenreCurationDialog::removeIgnoredGenre()
{
    if (m_db == nullptr || m_ignored == nullptr || m_ignored->currentItem() == nullptr) {
        return;
    }
    if (!m_db->setRadioGenreIgnored(m_ignored->currentItem()->text(), false)) {
        showError(m_db->lastError());
        return;
    }
    refreshIgnored();
    m_vocabularyRows = GenreCuration::buildReportRows(*m_db);
    refreshVocabulary();
}

void GenreCurationDialog::setAlias(const QString &alias, const QString &canonical)
{
    if (m_db == nullptr) {
        return;
    }
    const GenreCuration::AliasValidation validation = GenreCuration::validateAlias(alias, canonical);
    if (!validation.ok()) {
        showError(validation.error);
        return;
    }
    if (!m_db->setGenreAlias(validation.aliasFolded, validation.canonicalFolded)) {
        showError(m_db->lastError());
        return;
    }
    refreshAliases();
    m_vocabularyRows = GenreCuration::buildReportRows(*m_db);
    refreshVocabulary();
}

void GenreCurationDialog::showError(const QString &message)
{
    QMessageBox::warning(this, QStringLiteral("Genre curation"), message);
}
