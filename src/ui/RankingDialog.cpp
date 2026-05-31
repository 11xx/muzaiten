#include "ui/RankingDialog.h"

#include "core/MusicSort.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <functional>

using Search::ExcludeRule;
using Search::ExcludeScope;
using Search::RankConfig;
using Search::RankKind;
using Search::RankRule;

namespace {

// Flat list of selectable criteria. Each maps to a (kind, field) pair so the
// criterion combo fully determines the rule kind.
struct CritChoice {
    const char           *label;
    RankKind              kind;
    MusicSort::SortField  field;
};

const std::array<CritChoice, 16> kChoices = {{
    {"Relevance",            RankKind::Relevance,          MusicSort::SortField::AlbumArtist},
    {"Audio quality",        RankKind::AudioQuality,       MusicSort::SortField::AlbumArtist},
    {"Preferred directory",  RankKind::PreferredDirectory, MusicSort::SortField::AlbumArtist},
    {"Library order",        RankKind::LibraryOrder,       MusicSort::SortField::AlbumArtist},
    {"Sort: Artist",         RankKind::MusicField,         MusicSort::SortField::Artist},
    {"Sort: Album artist",   RankKind::MusicField,         MusicSort::SortField::AlbumArtist},
    {"Sort: Album",          RankKind::MusicField,         MusicSort::SortField::AlbumTitle},
    {"Sort: Title",          RankKind::MusicField,         MusicSort::SortField::Title},
    {"Sort: Year",           RankKind::MusicField,         MusicSort::SortField::Year},
    {"Sort: Track number",   RankKind::MusicField,         MusicSort::SortField::TrackNumber},
    {"Sort: Disc number",    RankKind::MusicField,         MusicSort::SortField::DiscNumber},
    {"Sort: Duration",       RankKind::MusicField,         MusicSort::SortField::Duration},
    {"Sort: Rating",         RankKind::MusicField,         MusicSort::SortField::Rating},
    {"Sort: Date added",     RankKind::MusicField,         MusicSort::SortField::DateAdded},
    {"Sort: File name",      RankKind::MusicField,         MusicSort::SortField::FileName},
    {"Sort: File size",      RankKind::MusicField,         MusicSort::SortField::FileSize},
}};

int choiceIndexFor(const RankRule &rule)
{
    for (int i = 0; i < static_cast<int>(kChoices.size()); ++i) {
        if (kChoices[i].kind != rule.kind) continue;
        if (rule.kind == RankKind::MusicField && kChoices[i].field != rule.field) continue;
        return i;
    }
    return 0;
}

enum CritColumn { CEnabled = 0, CCriterion, CDirection, CParam, CCount };
enum ExclColumn { EPattern = 0, EScope, ECount };

QComboBox *makeCriterionCombo(int currentChoice)
{
    auto *combo = new QComboBox;
    for (int i = 0; i < static_cast<int>(kChoices.size()); ++i) {
        combo->addItem(QString::fromUtf8(kChoices[i].label), i);
    }
    combo->setCurrentIndex(combo->findData(currentChoice));
    return combo;
}

QComboBox *makeDirectionCombo(MusicSort::SortDirection dir)
{
    auto *combo = new QComboBox;
    combo->addItem(QStringLiteral("Descending"), static_cast<int>(MusicSort::SortDirection::Descending));
    combo->addItem(QStringLiteral("Ascending"),  static_cast<int>(MusicSort::SortDirection::Ascending));
    combo->setCurrentIndex(combo->findData(static_cast<int>(dir)));
    return combo;
}

QComboBox *makeScopeCombo(ExcludeScope scope)
{
    auto *combo = new QComboBox;
    combo->addItem(QStringLiteral("Path"),      static_cast<int>(ExcludeScope::Path));
    combo->addItem(QStringLiteral("Any field"), static_cast<int>(ExcludeScope::AnyField));
    combo->setCurrentIndex(combo->findData(static_cast<int>(scope)));
    return combo;
}

QTableWidgetItem *checkItem(bool checked)
{
    auto *item = new QTableWidgetItem;
    item->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    return item;
}

} // namespace

RankingDialog::RankingDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Search ranking"));
    resize(720, 560);

    auto *layout = new QVBoxLayout(this);

    // --- Ranking criteria ---
    auto *critBox = new QGroupBox(QStringLiteral("Ranking criteria (top = highest priority)"), this);
    auto *critLayout = new QVBoxLayout(critBox);
    m_criteria = new QTableWidget(0, CCount, critBox);
    m_criteria->setHorizontalHeaderLabels({QStringLiteral("On"), QStringLiteral("Criterion"),
                                           QStringLiteral("Direction"),
                                           QStringLiteral("Directory (for \"Preferred directory\")")});
    m_criteria->horizontalHeader()->setSectionResizeMode(CParam, QHeaderView::Stretch);
    m_criteria->verticalHeader()->setVisible(false);
    m_criteria->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_criteria->setSelectionMode(QAbstractItemView::SingleSelection);
    critLayout->addWidget(m_criteria, 1);

    auto *critButtons = new QHBoxLayout;
    auto *addCrit = new QPushButton(QStringLiteral("Add"), critBox);
    auto *removeCrit = new QPushButton(QStringLiteral("Remove"), critBox);
    auto *upCrit = new QPushButton(QStringLiteral("Move up"), critBox);
    auto *downCrit = new QPushButton(QStringLiteral("Move down"), critBox);
    critButtons->addWidget(addCrit);
    critButtons->addWidget(removeCrit);
    critButtons->addStretch(1);
    critButtons->addWidget(upCrit);
    critButtons->addWidget(downCrit);
    critLayout->addLayout(critButtons);
    layout->addWidget(critBox, 1);

    connect(addCrit, &QPushButton::clicked, this, &RankingDialog::addBlankCriterionRow);
    connect(removeCrit, &QPushButton::clicked, this, &RankingDialog::removeSelectedCriteria);
    connect(upCrit, &QPushButton::clicked, this, [this]() { moveCriterion(-1); });
    connect(downCrit, &QPushButton::clicked, this, [this]() { moveCriterion(1); });

    // --- Exclusions ---
    auto *exclBox = new QGroupBox(QStringLiteral("Exclude patterns (glob: *, ?)"), this);
    auto *exclLayout = new QVBoxLayout(exclBox);
    m_excludes = new QTableWidget(0, ECount, exclBox);
    m_excludes->setHorizontalHeaderLabels({QStringLiteral("Pattern"), QStringLiteral("Scope")});
    m_excludes->horizontalHeader()->setSectionResizeMode(EPattern, QHeaderView::Stretch);
    m_excludes->verticalHeader()->setVisible(false);
    m_excludes->setSelectionBehavior(QAbstractItemView::SelectRows);
    exclLayout->addWidget(m_excludes, 1);

    auto *exclButtons = new QHBoxLayout;
    auto *addExcl = new QPushButton(QStringLiteral("Add"), exclBox);
    auto *removeExcl = new QPushButton(QStringLiteral("Remove"), exclBox);
    exclButtons->addWidget(addExcl);
    exclButtons->addWidget(removeExcl);
    exclButtons->addStretch(1);
    exclLayout->addLayout(exclButtons);
    layout->addWidget(exclBox, 1);

    connect(addExcl, &QPushButton::clicked, this, &RankingDialog::addBlankExcludeRow);
    connect(removeExcl, &QPushButton::clicked, this, &RankingDialog::removeSelectedExcludes);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void RankingDialog::addCriterionRow(const RankRule &rule)
{
    const int row = m_criteria->rowCount();
    m_criteria->insertRow(row);
    m_criteria->setItem(row, CEnabled, checkItem(rule.enabled));
    m_criteria->setCellWidget(row, CCriterion, makeCriterionCombo(choiceIndexFor(rule)));
    m_criteria->setCellWidget(row, CDirection, makeDirectionCombo(rule.dir));
    m_criteria->setItem(row, CParam, new QTableWidgetItem(rule.param));
}

void RankingDialog::addBlankCriterionRow()
{
    addCriterionRow(RankRule{});
}

void RankingDialog::removeSelectedCriteria()
{
    const auto rows = m_criteria->selectionModel()->selectedRows();
    if (!rows.isEmpty()) m_criteria->removeRow(rows.first().row());
}

void RankingDialog::moveCriterion(int delta)
{
    const auto rows = m_criteria->selectionModel()->selectedRows();
    if (rows.isEmpty()) return;
    const int from = rows.first().row();
    const int to = from + delta;
    if (to < 0 || to >= m_criteria->rowCount()) return;

    // Snapshot both rows as rules, then rewrite in swapped order. Simplest and
    // robust against cell-widget ownership quirks.
    const RankConfig snapshot = config();
    QVector<RankRule> r = snapshot.rules;
    std::swap(r[from], r[to]);
    m_criteria->setRowCount(0);
    for (const RankRule &rule : r) addCriterionRow(rule);
    m_criteria->selectRow(to);
}

void RankingDialog::addExcludeRow(const ExcludeRule &rule)
{
    const int row = m_excludes->rowCount();
    m_excludes->insertRow(row);
    m_excludes->setItem(row, EPattern, new QTableWidgetItem(rule.glob));
    m_excludes->setCellWidget(row, EScope, makeScopeCombo(rule.scope));
}

void RankingDialog::addBlankExcludeRow()
{
    addExcludeRow(ExcludeRule{});
}

void RankingDialog::removeSelectedExcludes()
{
    QList<int> rows;
    for (const QModelIndex &index : m_excludes->selectionModel()->selectedRows()) {
        rows.push_back(index.row());
    }
    std::sort(rows.begin(), rows.end(), std::greater<>());
    for (int row : rows) m_excludes->removeRow(row);
}

void RankingDialog::setConfig(const RankConfig &config)
{
    m_criteria->setRowCount(0);
    for (const RankRule &rule : config.rules) addCriterionRow(rule);
    m_excludes->setRowCount(0);
    for (const ExcludeRule &rule : config.excludes) addExcludeRow(rule);
}

RankConfig RankingDialog::config() const
{
    RankConfig cfg;
    for (int row = 0; row < m_criteria->rowCount(); ++row) {
        RankRule rule;
        const auto *enabled = m_criteria->item(row, CEnabled);
        rule.enabled = enabled != nullptr && enabled->checkState() == Qt::Checked;
        if (const auto *combo = qobject_cast<QComboBox *>(m_criteria->cellWidget(row, CCriterion))) {
            const int choice = combo->currentData().toInt();
            if (choice >= 0 && choice < static_cast<int>(kChoices.size())) {
                rule.kind = kChoices[choice].kind;
                rule.field = kChoices[choice].field;
            }
        }
        if (const auto *dirCombo = qobject_cast<QComboBox *>(m_criteria->cellWidget(row, CDirection))) {
            rule.dir = static_cast<MusicSort::SortDirection>(dirCombo->currentData().toInt());
        }
        if (const auto *param = m_criteria->item(row, CParam)) {
            rule.param = param->text().trimmed();
        }
        cfg.rules.push_back(rule);
    }
    for (int row = 0; row < m_excludes->rowCount(); ++row) {
        ExcludeRule rule;
        if (const auto *pattern = m_excludes->item(row, EPattern)) {
            rule.glob = pattern->text().trimmed();
        }
        if (const auto *scopeCombo = qobject_cast<QComboBox *>(m_excludes->cellWidget(row, EScope))) {
            rule.scope = static_cast<ExcludeScope>(scopeCombo->currentData().toInt());
        }
        if (!rule.glob.isEmpty()) cfg.excludes.push_back(rule);
    }
    return cfg;
}
