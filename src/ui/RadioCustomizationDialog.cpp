#include "ui/RadioCustomizationDialog.h"

#include "app/AppCore.h"
#include "reco/RadioProfileExchange.h"

#include <QComboBox>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QTextEdit>
#include <QVBoxLayout>

namespace {

QDoubleSpinBox *makeDoubleSpinBox(double minimum, double maximum, QWidget *parent)
{
    auto *spinBox = new QDoubleSpinBox(parent);
    spinBox->setRange(minimum, maximum);
    spinBox->setDecimals(3);
    spinBox->setSingleStep(0.1);
    return spinBox;
}

} // namespace

RadioCustomizationDialog::RadioCustomizationDialog(AppCore *core, QWidget *parent)
    : QDialog(parent)
    , m_core(core)
{
    setWindowTitle(QStringLiteral("Radio customization"));
    resize(560, 680);

    auto *layout = new QVBoxLayout(this);
    auto *profileRow = new QHBoxLayout;
    profileRow->addWidget(new QLabel(QStringLiteral("Profile:"), this));
    m_profiles = new QComboBox(this);
    profileRow->addWidget(m_profiles, 1);
    auto *newProfile = new QPushButton(QStringLiteral("New"), this);
    auto *rename = new QPushButton(QStringLiteral("Rename"), this);
    auto *duplicate = new QPushButton(QStringLiteral("Duplicate"), this);
    auto *remove = new QPushButton(QStringLiteral("Delete"), this);
    profileRow->addWidget(newProfile);
    profileRow->addWidget(rename);
    profileRow->addWidget(duplicate);
    profileRow->addWidget(remove);
    layout->addLayout(profileRow);

    auto *exchangeRow = new QHBoxLayout;
    auto *exportButton = new QPushButton(QStringLiteral("Export…"), this);
    auto *importButton = new QPushButton(QStringLiteral("Import…"), this);
    exportButton->setToolTip(RadioProfileExchange::contentsDescription());
    importButton->setToolTip(QStringLiteral("Preview a local tuning profile before importing. Existing profiles require explicit replace."));
    exchangeRow->addWidget(exportButton);
    exchangeRow->addWidget(importButton);
    exchangeRow->addStretch();
    layout->addLayout(exchangeRow);
    connect(exportButton, &QPushButton::clicked, this, &RadioCustomizationDialog::exportProfile);
    connect(importButton, &QPushButton::clicked, this, &RadioCustomizationDialog::importProfile);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto *content = new QWidget(scroll);
    auto *contentLayout = new QVBoxLayout(content);
    auto *weightsBox = new QGroupBox(QStringLiteral("Scoring weights"), content);
    auto *weightsForm = new QFormLayout(weightsBox);
    for (const TrackScorer::WeightSpec &spec : TrackScorer::weightSpecs()) {
        const QString label = spec.fallbackOnly
            ? spec.label + QStringLiteral(" (fallback only)")
            : spec.label;
        const QString tooltip = spec.fallbackOnly
            ? spec.tooltip + QStringLiteral("\n\nApplies only to tracks without audio analysis data. "
                                            "Tracks with DSP and CLAP features ignore this weight.")
            : spec.tooltip;
        QDoubleSpinBox *spinBox = makeDoubleSpinBox(spec.minimum, spec.maximum, weightsBox);
        spinBox->setToolTip(tooltip);
        weightsForm->addRow(label, spinBox);
        m_weightControls.insert(spec.key, spinBox);
        connect(spinBox, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                [this](double) { previewEdits(); });
    }
    contentLayout->addWidget(weightsBox);

    auto *decayBox = new QGroupBox(QStringLiteral("Session decay"), content);
    auto *decayForm = new QFormLayout(decayBox);
    m_decayStart = new QSpinBox(decayBox);
    m_decayStart->setRange(1, 1000);
    decayForm->addRow(QStringLiteral("Start after track"), m_decayStart);
    auto *curve = new QWidget(decayBox);
    auto *curveLayout = new QHBoxLayout(curve);
    curveLayout->setContentsMargins(0, 0, 0, 0);
    m_linear = new QRadioButton(QStringLiteral("Linear"), curve);
    m_exponential = new QRadioButton(QStringLiteral("Exponential"), curve);
    curveLayout->addWidget(m_linear);
    curveLayout->addWidget(m_exponential);
    curveLayout->addStretch();
    decayForm->addRow(QStringLiteral("Curve"), curve);
    m_noveltyFloor = makeDoubleSpinBox(0.0, 1.0, decayBox);
    m_ratingFloor = makeDoubleSpinBox(0.0, 1.0, decayBox);
    decayForm->addRow(QStringLiteral("Novelty floor"), m_noveltyFloor);
    decayForm->addRow(QStringLiteral("Rating floor"), m_ratingFloor);
    contentLayout->addWidget(decayBox);
    contentLayout->addStretch();
    scroll->setWidget(content);
    layout->addWidget(scroll, 1);

    auto *historyRow = new QHBoxLayout;
    m_undo = new QPushButton(QStringLiteral("Undo"), this);
    m_redo = new QPushButton(QStringLiteral("Redo"), this);
    auto *reset = new QPushButton(QStringLiteral("Reset to Default"), this);
    historyRow->addWidget(m_undo);
    historyRow->addWidget(m_redo);
    historyRow->addStretch();
    historyRow->addWidget(reset);
    layout->addLayout(historyRow);

    auto *buttons = new QDialogButtonBox(this);
    QPushButton *apply = buttons->addButton(QStringLiteral("Apply"), QDialogButtonBox::ApplyRole);
    QPushButton *revert = buttons->addButton(QStringLiteral("Revert"), QDialogButtonBox::ResetRole);
    QPushButton *close = buttons->addButton(QStringLiteral("Close"), QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);

    m_previewTimer = new QTimer(this);
    m_previewTimer->setSingleShot(true);
    m_previewTimer->setInterval(500);
    connect(m_previewTimer, &QTimer::timeout, this, &RadioCustomizationDialog::flushPreview);
    connect(m_profiles, &QComboBox::currentTextChanged, this, [this](const QString &name) {
        if (m_loading || name == m_core->activeRadioProfile().name) {
            return;
        }
        flushPreview();
        if (m_core->selectRadioProfile(name)) {
            m_revertBaseline = m_core->activeRadioProfile();
            reloadProfile();
        }
    });
    connect(newProfile, &QPushButton::clicked, this, [this]() { createProfile(false); });
    connect(duplicate, &QPushButton::clicked, this, [this]() { createProfile(true); });
    connect(rename, &QPushButton::clicked, this, &RadioCustomizationDialog::renameProfile);
    connect(remove, &QPushButton::clicked, this, [this]() {
        const RadioProfile profile = m_core->activeRadioProfile();
        if (profile.name == QLatin1String("Default")) {
            QMessageBox::information(this, QStringLiteral("Default profile"),
                                     QStringLiteral("The Default profile cannot be deleted."));
            return;
        }
        if (QMessageBox::question(this, QStringLiteral("Delete profile"),
                                  QStringLiteral("Delete the profile \"%1\"?").arg(profile.name))
            == QMessageBox::Yes && m_core->deleteActiveRadioProfile()) {
            m_revertBaseline = m_core->activeRadioProfile();
            reloadProfile();
        }
    });
    connect(reset, &QPushButton::clicked, this, [this]() {
        flushPreview();
        if (m_core->resetActiveRadioProfile()) {
            m_revertBaseline = m_core->activeRadioProfile();
            reloadProfile();
        }
    });
    connect(m_undo, &QPushButton::clicked, this, [this]() {
        flushPreview();
        if (m_core->undoRadioProfile()) {
            m_revertBaseline = m_core->activeRadioProfile();
            reloadProfile();
        }
    });
    connect(m_redo, &QPushButton::clicked, this, [this]() {
        flushPreview();
        if (m_core->redoRadioProfile()) {
            m_revertBaseline = m_core->activeRadioProfile();
            reloadProfile();
        }
    });
    connect(apply, &QPushButton::clicked, this, [this]() {
        flushPreview();
        m_revertBaseline = m_core->activeRadioProfile();
        updateHistoryButtons();
    });
    connect(revert, &QPushButton::clicked, this, [this]() {
        m_previewTimer->stop();
        m_previewDirty = false;
        if (m_core->restoreActiveRadioProfile(m_revertBaseline)) {
            reloadProfile();
        }
    });
    connect(close, &QPushButton::clicked, this, [this]() { flushPreview(); reject(); });
    connect(m_decayStart, qOverload<int>(&QSpinBox::valueChanged), this, [this](int) { previewEdits(); });
    connect(m_linear, &QRadioButton::toggled, this, [this](bool) { previewEdits(); });
    connect(m_noveltyFloor, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double) { previewEdits(); });
    connect(m_ratingFloor, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
            [this](double) { previewEdits(); });

    m_revertBaseline = m_core->activeRadioProfile();
    reloadProfile();
}

void RadioCustomizationDialog::exportProfile()
{
    const RadioProfile profile = m_core->activeRadioProfile();
    const bool tasteBearing = profile.origin != RadioProfile::Origin::Manual;
    QString disclosure = QStringLiteral("Profile: %1\n\n").arg(profile.name)
        + RadioProfileExchange::contentsDescription();
    if (tasteBearing) {
        disclosure += QStringLiteral("\n\nThis profile is learned from listening or has an unknown origin. "
                                     "Its tuning can reveal listening taste. Export it with origin learned?");
    } else {
        disclosure += QStringLiteral("\n\nExport the manual profile \"%1\"?").arg(profile.name);
    }
    if (QMessageBox::question(this, QStringLiteral("Export radio profile"), disclosure,
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export radio profile"),
        QStringLiteral("radio-profile.json"), QStringLiteral("Radio profile JSON (*.json)"), nullptr,
        QFileDialog::DontConfirmOverwrite);
    if (path.isEmpty()) {
        return;
    }
    const QFileInfo file(path);
    const bool replace = file.exists() || file.isSymLink();
    if (replace && QMessageBox::question(this, QStringLiteral("Replace export file"),
        QStringLiteral("Replace the existing file \"%1\"?").arg(file.fileName()),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
        return;
    }
    QString error;
    if (!RadioProfileExchange::write(path, profile, tasteBearing, replace, &error)) {
        QMessageBox::warning(this, QStringLiteral("Export radio profile"), error);
    }
}

void RadioCustomizationDialog::importProfile()
{
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import radio profile"),
        {}, QStringLiteral("Radio profile JSON (*.json)"));
    if (path.isEmpty()) {
        return;
    }
    RadioProfile profile;
    QString error;
    if (!RadioProfileExchange::read(path, &profile, &error)) {
        QMessageBox::warning(this, QStringLiteral("Import radio profile"), error);
        return;
    }
    bool exists = false;
    for (const RadioProfile &entry : m_core->radioProfiles()) {
        exists = exists || entry.name == profile.name;
    }
    QDialog preview(this);
    preview.setWindowTitle(QStringLiteral("Import radio profile preview"));
    preview.resize(540, 620);
    auto *layout = new QVBoxLayout(&preview);
    auto *description = new QLabel(RadioProfileExchange::contentsDescription()
        + QStringLiteral("\n\nThe active profile selection stays the same. Replacing the active profile applies its tuning live."), &preview);
    description->setWordWrap(true);
    layout->addWidget(description);
    if (profile.origin == RadioProfile::Origin::Learned) {
        auto *warning = new QLabel(QStringLiteral("Learned tuning can reveal the sender's listening taste."), &preview);
        warning->setWordWrap(true);
        layout->addWidget(warning);
    }
    QByteArray bytes;
    RadioProfileExchange::encode(profile, true, &bytes, &error);
    auto *contents = new QTextEdit(&preview);
    contents->setReadOnly(true);
    contents->setPlainText(QString::fromUtf8(bytes));
    layout->addWidget(contents, 1);
    auto *replace = new QCheckBox(QStringLiteral("Replace existing profile \"%1\"").arg(profile.name), &preview);
    replace->setVisible(exists);
    layout->addWidget(replace);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &preview);
    QPushButton *importButton = buttons->addButton(QStringLiteral("Import"), QDialogButtonBox::AcceptRole);
    importButton->setEnabled(!exists);
    connect(replace, &QCheckBox::toggled, importButton, &QPushButton::setEnabled);
    connect(buttons, &QDialogButtonBox::accepted, &preview, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &preview, &QDialog::reject);
    layout->addWidget(buttons);
    if (preview.exec() != QDialog::Accepted) {
        return;
    }
    flushPreview();
    if (!m_core->importRadioProfile(profile, replace->isChecked(), &error)) {
        QMessageBox::warning(this, QStringLiteral("Import radio profile"), error);
        return;
    }
    m_revertBaseline = m_core->activeRadioProfile();
    reloadProfile();
}

void RadioCustomizationDialog::reject()
{
    flushPreview();
    QDialog::reject();
}

void RadioCustomizationDialog::closeEvent(QCloseEvent *event)
{
    flushPreview();
    QDialog::closeEvent(event);
}

void RadioCustomizationDialog::reloadProfile()
{
    m_loading = true;
    const RadioProfile profile = m_core->activeRadioProfile();
    m_profiles->clear();
    for (const RadioProfile &candidate : m_core->radioProfiles()) {
        m_profiles->addItem(candidate.name);
    }
    m_profiles->setCurrentText(profile.name);
    for (const TrackScorer::WeightSpec &spec : TrackScorer::weightSpecs()) {
        double value = spec.defaultValue;
        TrackScorer::weightValue(profile.weights, spec.key, &value);
        m_weightControls.value(spec.key)->setValue(value);
    }
    m_decayStart->setValue(profile.sessionDecay.decayStartTrack);
    m_linear->setChecked(profile.sessionDecay.decayCurve < 0.5);
    m_exponential->setChecked(profile.sessionDecay.decayCurve >= 0.5);
    m_noveltyFloor->setValue(profile.sessionDecay.noveltyDecayFloor);
    m_ratingFloor->setValue(profile.sessionDecay.ratingDecayFloor);
    m_loading = false;
    updateHistoryButtons();
}

RadioProfile RadioCustomizationDialog::editedProfile() const
{
    RadioProfile profile = m_core->activeRadioProfile();
    for (const TrackScorer::WeightSpec &spec : TrackScorer::weightSpecs()) {
        TrackScorer::setWeightValue(profile.weights, spec.key, m_weightControls.value(spec.key)->value());
    }
    profile.sessionDecay.decayStartTrack = m_decayStart->value();
    profile.sessionDecay.decayCurve = m_exponential->isChecked() ? 1.0 : 0.0;
    profile.sessionDecay.noveltyDecayFloor = m_noveltyFloor->value();
    profile.sessionDecay.ratingDecayFloor = m_ratingFloor->value();
    return profile;
}

void RadioCustomizationDialog::previewEdits()
{
    if (m_loading) {
        return;
    }
    m_core->previewActiveRadioProfile(editedProfile());
    m_previewDirty = true;
    m_previewTimer->start();
}

void RadioCustomizationDialog::flushPreview()
{
    if (!m_previewDirty) {
        return;
    }
    m_previewTimer->stop();
    m_core->commitActiveRadioProfilePreview();
    m_previewDirty = false;
    updateHistoryButtons();
}

void RadioCustomizationDialog::updateHistoryButtons()
{
    m_undo->setEnabled(m_core->canUndoRadioProfile());
    m_redo->setEnabled(m_core->canRedoRadioProfile());
}

void RadioCustomizationDialog::createProfile(bool duplicateActive)
{
    bool accepted = false;
    const QString name = QInputDialog::getText(this, duplicateActive ? QStringLiteral("Duplicate profile")
                                                                    : QStringLiteral("New profile"),
                                               QStringLiteral("Name:"), QLineEdit::Normal, QString(), &accepted);
    if (accepted && m_core->createRadioProfile(name, duplicateActive)) {
        m_revertBaseline = m_core->activeRadioProfile();
        reloadProfile();
    }
}

void RadioCustomizationDialog::renameProfile()
{
    bool accepted = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("Rename profile"), QStringLiteral("Name:"),
                                               QLineEdit::Normal, m_core->activeRadioProfile().name, &accepted);
    if (accepted && m_core->renameActiveRadioProfile(name)) {
        m_revertBaseline = m_core->activeRadioProfile();
        reloadProfile();
    }
}
