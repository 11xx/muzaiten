#include "ui/ScoringWeightsDialog.h"

#include "db/Database.h"

#include <QAbstractItemView>
#include <QDate>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <utility>

namespace {

constexpr const char kRadioScoringWeightsKey[] = "radio.scoringWeights";

QTableWidgetItem *textItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return item;
}

QTableWidgetItem *numberItem(double value, int decimals = 3)
{
    auto *item = new QTableWidgetItem(QString::number(value, 'f', decimals));
    item->setData(Qt::DisplayRole, value);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return item;
}

QTableWidgetItem *intItem(int value)
{
    auto *item = new QTableWidgetItem;
    item->setData(Qt::DisplayRole, value);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    return item;
}

} // namespace

ScoringWeightsDialog::ScoringWeightsDialog(Database *db, QString historyPath, QWidget *parent)
    : QDialog(parent)
    , m_db(db)
    , m_historyPath(std::move(historyPath))
{
    setWindowTitle(QStringLiteral("Scoring weights"));
    resize(720, 640);

    auto *root = new QVBoxLayout(this);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto *editor = new QWidget(scroll);
    auto *form = new QFormLayout(editor);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    for (const TrackScorer::WeightSpec &spec : TrackScorer::weightSpecs()) {
        auto *spin = new QDoubleSpinBox(editor);
        spin->setDecimals(3);
        spin->setSingleStep(0.1);
        spin->setRange(spec.minimum, spec.maximum);
        spin->setValue(spec.defaultValue);
        spin->setToolTip(spec.tooltip);
        spin->setKeyboardTracking(false);
        m_spins.insert(spec.key, spin);
        auto *label = new QLabel(spec.label, editor);
        label->setToolTip(QStringLiteral("%1\n%2").arg(spec.key, spec.tooltip));
        form->addRow(label, spin);
    }
    scroll->setWidget(editor);
    root->addWidget(scroll, 1);

    auto *profileLayout = new QVBoxLayout;
    auto *profileLabel = new QLabel(QStringLiteral("Profiles"), this);
    profileLayout->addWidget(profileLabel);
    m_profiles = new QListWidget(this);
    m_profiles->setSelectionMode(QAbstractItemView::SingleSelection);
    profileLayout->addWidget(m_profiles);
    auto *profileButtons = new QHBoxLayout;
    auto *applyProfile = new QPushButton(QStringLiteral("Apply profile"), this);
    auto *saveProfile = new QPushButton(QStringLiteral("Save current as..."), this);
    auto *deleteProfile = new QPushButton(QStringLiteral("Delete"), this);
    profileButtons->addWidget(applyProfile);
    profileButtons->addWidget(saveProfile);
    profileButtons->addWidget(deleteProfile);
    profileButtons->addStretch(1);
    profileLayout->addLayout(profileButtons);
    root->addLayout(profileLayout);

    auto *actions = new QHBoxLayout;
    auto *suggest = new QPushButton(QStringLiteral("Suggest from listening..."), this);
    auto *reset = new QPushButton(QStringLiteral("Reset to defaults"), this);
    actions->addWidget(suggest);
    actions->addWidget(reset);
    actions->addStretch(1);
    root->addLayout(actions);

    m_status = new QLabel(QStringLiteral("Changes take effect on the next radio session."), this);
    m_status->setWordWrap(true);
    root->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Close, this);
    root->addWidget(buttons);

    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, &ScoringWeightsDialog::applyCurrentWeights);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(reset, &QPushButton::clicked, this, &ScoringWeightsDialog::resetDefaults);
    connect(applyProfile, &QPushButton::clicked, this, &ScoringWeightsDialog::applySelectedProfile);
    connect(saveProfile, &QPushButton::clicked, this, &ScoringWeightsDialog::saveCurrentAsProfile);
    connect(deleteProfile, &QPushButton::clicked, this, &ScoringWeightsDialog::deleteSelectedProfile);
    connect(suggest, &QPushButton::clicked, this, &ScoringWeightsDialog::suggestFromListening);

    loadActiveWeights();
    refreshProfiles();
}

void ScoringWeightsDialog::loadActiveWeights()
{
    if (m_db == nullptr) {
        return;
    }
    QString error;
    const TrackScorer::Weights weights =
        TrackScorer::weightsFromJson(m_db->setting(QString::fromLatin1(kRadioScoringWeightsKey)).toUtf8(), &error);
    if (!error.isEmpty()) {
        setStatus(QStringLiteral("Active weights are invalid; showing defaults: %1").arg(error));
    }
    for (const TrackScorer::WeightSpec &spec : TrackScorer::weightSpecs()) {
        double value = spec.defaultValue;
        TrackScorer::weightValue(weights, spec.key, &value);
        if (QDoubleSpinBox *spin = m_spins.value(spec.key, nullptr)) {
            spin->setValue(value);
        }
    }
}

void ScoringWeightsDialog::applyCurrentWeights()
{
    if (m_db == nullptr) {
        return;
    }
    const QByteArray json = currentWeightsJson();
    QString error;
    TrackScorer::weightsFromJson(json, &error);
    if (!error.isEmpty()) {
        showError(error);
        return;
    }
    if (!m_db->setSetting(QString::fromLatin1(kRadioScoringWeightsKey), QString::fromUtf8(json))) {
        showError(m_db->lastError());
        return;
    }
    setStatus(QStringLiteral("Scoring weights saved; changes take effect on the next radio session."));
}

void ScoringWeightsDialog::resetDefaults()
{
    const TrackScorer::Weights defaults = TrackScorer::defaultWeights();
    for (const TrackScorer::WeightSpec &spec : TrackScorer::weightSpecs()) {
        double value = spec.defaultValue;
        TrackScorer::weightValue(defaults, spec.key, &value);
        if (QDoubleSpinBox *spin = m_spins.value(spec.key, nullptr)) {
            spin->setValue(value);
        }
    }
    setStatus(QStringLiteral("Default weights loaded; apply to save them for the next radio session."));
}

void ScoringWeightsDialog::refreshProfiles()
{
    if (m_db == nullptr || m_profiles == nullptr) {
        return;
    }
    const QString selected = selectedProfileName();
    m_profiles->clear();
    for (const Database::RadioWeightProfile &profile : m_db->radioWeightProfiles()) {
        auto *item = new QListWidgetItem(profile.name);
        item->setToolTip(profile.updatedAt);
        m_profiles->addItem(item);
        if (profile.name == selected) {
            m_profiles->setCurrentItem(item);
        }
    }
}

void ScoringWeightsDialog::applySelectedProfile()
{
    if (m_db == nullptr) {
        return;
    }
    const QString name = selectedProfileName();
    if (name.isEmpty()) {
        return;
    }
    const QString profileJson = m_db->radioWeightProfile(name);
    if (profileJson.isEmpty()) {
        showError(QStringLiteral("radio-weights profile not found: %1").arg(name));
        refreshProfiles();
        return;
    }
    QString error;
    TrackScorer::weightsFromJson(profileJson.toUtf8(), &error);
    if (!error.isEmpty()) {
        showError(error);
        return;
    }
    if (!m_db->setSetting(QString::fromLatin1(kRadioScoringWeightsKey), profileJson)) {
        showError(m_db->lastError());
        return;
    }
    loadActiveWeights();
    setStatus(QStringLiteral("Profile %1 applied; changes take effect on the next radio session.").arg(name));
}

void ScoringWeightsDialog::saveCurrentAsProfile()
{
    if (m_db == nullptr) {
        return;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Save scoring profile"),
                                               QStringLiteral("Profile name:"), QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok) {
        return;
    }
    if (name.isEmpty()) {
        showError(QStringLiteral("radio-weights save needs a non-empty profile name"));
        return;
    }
    const QString json = QString::fromUtf8(currentWeightsJson());
    if (!m_db->saveRadioWeightProfile(name, json)) {
        showError(m_db->lastError());
        return;
    }
    refreshProfiles();
    setStatus(QStringLiteral("Profile %1 saved.").arg(name));
}

void ScoringWeightsDialog::deleteSelectedProfile()
{
    if (m_db == nullptr) {
        return;
    }
    const QString name = selectedProfileName();
    if (name.isEmpty()) {
        return;
    }
    if (!m_db->removeRadioWeightProfile(name)) {
        showError(m_db->lastError());
        return;
    }
    refreshProfiles();
    setStatus(QStringLiteral("Profile %1 deleted.").arg(name));
}

void ScoringWeightsDialog::suggestFromListening()
{
    if (m_db == nullptr) {
        return;
    }
    if (!QFileInfo::exists(m_historyPath)) {
        showError(QStringLiteral("history database not found at %1").arg(m_historyPath));
        return;
    }
    const WeightLearnerData::LoadResult load = WeightLearnerData::loadSamplesFromPath(m_historyPath);
    if (!load.error.isEmpty()) {
        showError(load.error);
        return;
    }
    const WeightLearner::Result learned = WeightLearner::learn(load.samples);
    if (!learned.ok) {
        showError(learned.error);
        return;
    }
    showLearnedWeights(learned, load);
}

void ScoringWeightsDialog::showLearnedWeights(const WeightLearner::Result &learned,
                                              const WeightLearnerData::LoadResult &load)
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Suggested scoring weights"));
    dialog.resize(760, 460);
    auto *root = new QVBoxLayout(&dialog);
    auto *summary = new QLabel(QStringLiteral("%1 labeled picks (%2 early skips). Skipped rows: %3 invalid weights, %4 without learnable components.")
                                   .arg(learned.sampleCount)
                                   .arg(learned.positiveLabels)
                                   .arg(load.skippedInvalidWeights)
                                   .arg(load.skippedNoSignals),
                               &dialog);
    summary->setWordWrap(true);
    root->addWidget(summary);

    auto *table = new QTableWidget(0, 5, &dialog);
    table->setHorizontalHeaderLabels({
        QStringLiteral("Component"),
        QStringLiteral("Multiplier"),
        QStringLiteral("Default"),
        QStringLiteral("Suggested"),
        QStringLiteral("Samples"),
    });
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->setVisible(false);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    root->addWidget(table, 1);
    for (const WeightLearner::ComponentResult &rowData : learned.components) {
        const int row = table->rowCount();
        table->insertRow(row);
        table->setItem(row, 0, textItem(rowData.componentName));
        table->setItem(row, 1, numberItem(rowData.multiplier));
        table->setItem(row, 2, numberItem(rowData.defaultWeight));
        table->setItem(row, 3, numberItem(rowData.suggestedWeight));
        table->setItem(row, 4, intItem(rowData.nonZeroSamples));
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto *save = buttons->addButton(QStringLiteral("Save as profile learned-..."), QDialogButtonBox::ActionRole);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    connect(save, &QPushButton::clicked, &dialog, [this, &dialog, &learned]() {
        const QString profileName = QStringLiteral("learned-%1").arg(QDate::currentDate().toString(QStringLiteral("yyyyMMdd")));
        if (m_db == nullptr) {
            return;
        }
        if (!m_db->saveRadioWeightProfile(profileName, QString::fromUtf8(learned.suggestedWeightsJson))) {
            showError(m_db->lastError());
            return;
        }
        refreshProfiles();
        setStatus(QStringLiteral("Saved profile %1; apply it explicitly when ready.").arg(profileName));
        dialog.accept();
    });
    dialog.exec();
}

TrackScorer::Weights ScoringWeightsDialog::currentWeights() const
{
    TrackScorer::Weights weights = TrackScorer::defaultWeights();
    for (const TrackScorer::WeightSpec &spec : TrackScorer::weightSpecs()) {
        const QDoubleSpinBox *spin = m_spins.value(spec.key, nullptr);
        if (spin != nullptr) {
            TrackScorer::setWeightValue(weights, spec.key, spin->value());
        }
    }
    return weights;
}

QByteArray ScoringWeightsDialog::currentWeightsJson() const
{
    return TrackScorer::weightsToJson(currentWeights());
}

QString ScoringWeightsDialog::selectedProfileName() const
{
    return m_profiles == nullptr || m_profiles->currentItem() == nullptr ? QString() : m_profiles->currentItem()->text();
}

void ScoringWeightsDialog::showError(const QString &message)
{
    QMessageBox::warning(this, QStringLiteral("Scoring weights"), message);
}

void ScoringWeightsDialog::setStatus(const QString &message)
{
    if (m_status != nullptr) {
        m_status->setText(message);
    }
}
