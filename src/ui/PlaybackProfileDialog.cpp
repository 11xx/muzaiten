#include "ui/PlaybackProfileDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace {

// Parse /proc/asound/cards and return {display label, hw:N} pairs.
QVector<QPair<QString, QString>> enumerateAlsaCards()
{
    QVector<QPair<QString, QString>> cards;
    QFile f(QStringLiteral("/proc/asound/cards"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return cards;

    // Lines of interest look like:
    //  " 0 [Pro            ]: USB-Audio - FiiO K5 Pro"
    static const QRegularExpression re(
        QStringLiteral(R"(^\s*(\d+)\s+\[.*?\]\s*:\s*\S+\s+-\s+(.+)$)"));

    const QString text = QString::fromLocal8Bit(f.readAll());
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        const QRegularExpressionMatch m = re.match(line);
        if (!m.hasMatch())
            continue;
        const QString index = m.captured(1).trimmed();
        const QString name  = m.captured(2).trimmed();
        const QString hw    = QStringLiteral("hw:%1").arg(index);
        cards.append({QStringLiteral("%1 (%2)").arg(name, hw), hw});
    }
    return cards;
}

} // namespace

PlaybackProfileDialog::PlaybackProfileDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Playback output"));

    auto *layout = new QVBoxLayout(this);
    m_form = new QFormLayout;

    // Mode
    m_mode = new QComboBox(this);
    m_mode->addItem(QStringLiteral("Shared"),     QStringLiteral("shared"));
    m_mode->addItem(QStringLiteral("Bit-perfect"), QStringLiteral("bit-perfect"));
    m_form->addRow(QStringLiteral("Mode"), m_mode);

    // Sink (shared mode only)
    m_sink = new QComboBox(this);
    m_sink->addItem(QStringLiteral("Auto"), QStringLiteral("auto"));
    m_sink->addItem(QStringLiteral("PipeWire"),   QStringLiteral("pipewire"));
    m_sink->addItem(QStringLiteral("PulseAudio"), QStringLiteral("pulse"));
    m_sink->addItem(QStringLiteral("ALSA"),       QStringLiteral("alsa"));
    m_form->addRow(QStringLiteral("Sink"), m_sink);

    // Device combo (bit-perfect mode only)
    m_deviceCombo = new QComboBox(this);
    m_deviceCombo->setEditable(true);
    m_deviceCombo->setInsertPolicy(QComboBox::NoInsert);
    m_deviceCombo->setPlaceholderText(QStringLiteral("hw:0"));
    for (const auto &[label, hw] : enumerateAlsaCards())
        m_deviceCombo->addItem(label, hw);
    m_form->addRow(QStringLiteral("Device"), m_deviceCombo);

    // Software volume (shared mode only)
    m_softwareVolume = new QCheckBox(QStringLiteral("App controls volume"), this);
    m_softwareVolume->setToolTip(
        QStringLiteral("When off, the volume slider has no effect and the DAC/hardware controls level."));
    m_form->addRow(QString(), m_softwareVolume);

    // Resampling (shared mode only)
    m_allowResample = new QCheckBox(QStringLiteral("Allow resampling"), this);
    m_allowResample->setToolTip(
        QStringLiteral("Allow GStreamer to convert sample rates. Normally off — PipeWire handles rate negotiation."));
    m_form->addRow(QString(), m_allowResample);

    connect(m_mode, &QComboBox::currentIndexChanged, this, [this]() {
        updateModeVisibility();
    });

    layout->addLayout(m_form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    // Pin the minimum size to the fully-expanded (shared) state so switching
    // modes never shrinks the dialog.
    layout->activate();
    setMinimumSize(sizeHint());

    updateModeVisibility();
}

void PlaybackProfileDialog::updateModeVisibility()
{
    const bool bitPerfect = m_mode->currentData().toString() == QStringLiteral("bit-perfect");
    m_form->setRowVisible(m_sink,          !bitPerfect);
    m_form->setRowVisible(m_deviceCombo,    bitPerfect);
    m_form->setRowVisible(m_softwareVolume, !bitPerfect);
    m_form->setRowVisible(m_allowResample,  !bitPerfect);
}

void PlaybackProfileDialog::setProfile(const PlaybackProfile &profile)
{
    // Migrate legacy "exclusive" key to "bit-perfect".
    const QString mode = profile.mode == QStringLiteral("exclusive")
                             ? QStringLiteral("bit-perfect")
                             : profile.mode;

    const int modeIndex = m_mode->findData(mode);
    m_mode->setCurrentIndex(modeIndex >= 0 ? modeIndex : 0);

    const int sinkIndex = m_sink->findData(profile.sink);
    m_sink->setCurrentIndex(sinkIndex >= 0 ? sinkIndex : 0);

    // Populate device combo: try to match by data (hw:N), fall back to typed text.
    if (!profile.device.isEmpty()) {
        const int devIndex = m_deviceCombo->findData(profile.device);
        if (devIndex >= 0) {
            m_deviceCombo->setCurrentIndex(devIndex);
        } else {
            m_deviceCombo->setCurrentText(profile.device);
        }
    }

    m_softwareVolume->setChecked(profile.softwareVolume);
    m_allowResample->setChecked(profile.allowResample);

    updateModeVisibility();
}

PlaybackProfile PlaybackProfileDialog::profile() const
{
    PlaybackProfile p;
    p.backend = QStringLiteral("gstreamer");
    p.mode    = m_mode->currentData().toString();
    p.replayGain = false;

    if (p.mode == QStringLiteral("bit-perfect")) {
        p.sink   = QStringLiteral("alsa");
        // Prefer the item data (hw:N); fall back to the edited text for manual entries.
        const QString devData = m_deviceCombo->currentData().toString();
        p.device = devData.isEmpty() ? m_deviceCombo->currentText().trimmed() : devData;
        p.softwareVolume = false;
        p.allowResample  = false;
    } else {
        p.sink           = m_sink->currentData().toString();
        p.device.clear();
        p.softwareVolume = m_softwareVolume->isChecked();
        p.allowResample  = m_allowResample->isChecked();
    }

    return p;
}
