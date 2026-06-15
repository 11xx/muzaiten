#include "ui/PlaybackProfileDialog.h"

#include "playback/AudioDeviceControl.h"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <optional>

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
    populateDevices();
    m_form->addRow(QStringLiteral("Device"), m_deviceCombo);

    // Takeover status + action (bit-perfect mode only). A card PipeWire holds
    // cannot be opened directly, so offer a one-click takeover (and release)
    // here rather than making the user reach for an external script.
    m_deviceStatus = new QLabel(this);
    m_deviceStatus->setWordWrap(true);
    m_deviceAction = new QPushButton(this);
    m_deviceAction->setAutoDefault(false);
    auto *deviceStatusRow = new QHBoxLayout;
    deviceStatusRow->setContentsMargins(0, 0, 0, 0);
    deviceStatusRow->addWidget(m_deviceStatus, 1);
    deviceStatusRow->addWidget(m_deviceAction, 0);
    m_form->addRow(QString(), deviceStatusRow);

    connect(m_deviceCombo, &QComboBox::currentTextChanged, this, [this]() {
        refreshDeviceStatus();
    });
    connect(m_deviceAction, &QPushButton::clicked, this, [this]() {
        toggleSelectedDeviceTakeover();
    });

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

    // Release on pause (always on in bit-perfect; configurable in shared)
    m_releaseSinkOnPause = new QCheckBox(QStringLiteral("Release output device on pause"), this);
    m_releaseSinkOnPause->setToolTip(
        QStringLiteral("Free the audio device immediately on pause so other apps can use it. "
                       "Resume seeks back to the paused position. "
                       "Always enabled in bit-perfect mode."));
    m_form->addRow(QString(), m_releaseSinkOnPause);

    // Disk read-ahead: warm the page cache ahead of the playhead.
    const QString readAheadHelp =
        QStringLiteral("Keeps roughly this much of the upcoming track warm in the OS page "
                       "cache (RAM) ahead of the playhead, so reads from slow disks or "
                       "network mounts don't stutter. Uses reclaimable cache, not extra app "
                       "memory; the system's own read-ahead still applies when Off.");
    m_readAheadMb = new QSpinBox(this);
    m_readAheadMb->setRange(0, 1024);
    m_readAheadMb->setSingleStep(16);
    m_readAheadMb->setSuffix(QStringLiteral(" MB"));
    m_readAheadMb->setSpecialValueText(QStringLiteral("Off"));
    m_readAheadMb->setToolTip(readAheadHelp);

    auto *readAheadInfo = new QToolButton(this);
    readAheadInfo->setAutoRaise(true);
    readAheadInfo->setIcon(style()->standardIcon(QStyle::SP_MessageBoxInformation));
    readAheadInfo->setToolTip(readAheadHelp);
    readAheadInfo->setFocusPolicy(Qt::NoFocus);
    readAheadInfo->setAccessibleName(QStringLiteral("Disk read-ahead help"));

    auto *readAheadRow = new QHBoxLayout;
    readAheadRow->setContentsMargins(0, 0, 0, 0);
    readAheadRow->addWidget(m_readAheadMb);
    readAheadRow->addWidget(readAheadInfo);
    readAheadRow->addStretch(1);
    m_form->addRow(QStringLiteral("Disk read-ahead"), readAheadRow);

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
    // The takeover status line only makes sense for a direct device.
    m_deviceStatus->setVisible(bitPerfect);
    m_deviceAction->setVisible(bitPerfect);
    // In bit-perfect mode the device is always released on pause (exclusive ALSA
    // access blocks other apps otherwise); lock the checkbox to reflect that.
    m_releaseSinkOnPause->setEnabled(!bitPerfect);
    if (bitPerfect) {
        m_releaseSinkOnPause->setChecked(true);
        refreshDeviceStatus();
    }
}

void PlaybackProfileDialog::populateDevices()
{
    // Preserve the current selection (hw:N or typed text) across a rebuild.
    const QString selected = m_deviceCombo->currentData().toString().isEmpty()
        ? m_deviceCombo->currentText().trimmed()
        : m_deviceCombo->currentData().toString();

    // Map hw:N -> PipeWire hold state so we can flag busy cards in the list.
    QHash<QString, bool> held;
    for (const AudioDeviceControl::DeviceState &dev : AudioDeviceControl::enumerate())
        held.insert(dev.hwPath, dev.heldByPipeWire());

    const QSignalBlocker blocker(m_deviceCombo);
    m_deviceCombo->clear();
    for (const auto &[label, hw] : enumerateAlsaCards()) {
        const bool busy = held.value(hw, false);
        m_deviceCombo->addItem(busy ? QStringLiteral("%1 — busy").arg(label) : label, hw);
        if (busy) {
            const int row = m_deviceCombo->count() - 1;
            m_deviceCombo->setItemData(row, QColor(Qt::red), Qt::ForegroundRole);
            m_deviceCombo->setItemData(
                row,
                QStringLiteral("Held by PipeWire — take over to play bit-perfect to it"),
                Qt::ToolTipRole);
        }
    }

    if (!selected.isEmpty()) {
        const int devIndex = m_deviceCombo->findData(selected);
        if (devIndex >= 0)
            m_deviceCombo->setCurrentIndex(devIndex);
        else
            m_deviceCombo->setCurrentText(selected);
    }
}

void PlaybackProfileDialog::refreshDeviceStatus()
{
    if (m_mode->currentData().toString() != QStringLiteral("bit-perfect"))
        return;

    if (!AudioDeviceControl::toolingAvailable()) {
        m_deviceStatus->clear();
        m_deviceAction->setVisible(false);
        return;
    }

    const QString hw = m_deviceCombo->currentData().toString().isEmpty()
        ? m_deviceCombo->currentText().trimmed()
        : m_deviceCombo->currentData().toString();
    const std::optional<AudioDeviceControl::DeviceState> dev =
        AudioDeviceControl::findByHwPath(hw);

    if (!dev) {
        // Either a manual hw: string with no matching PipeWire card, or nothing
        // selected — nothing to take over.
        m_deviceStatus->setText(QStringLiteral("PipeWire does not manage this device."));
        m_deviceStatus->setStyleSheet(QString());
        m_deviceAction->setVisible(false);
        return;
    }

    m_deviceAction->setVisible(true);
    if (dev->heldByPipeWire()) {
        m_deviceStatus->setText(
            QStringLiteral("⚠ Busy — PipeWire is holding this device; direct playback will fail."));
        m_deviceStatus->setStyleSheet(QStringLiteral("color: #c0392b;"));
        m_deviceAction->setText(QStringLiteral("Take over"));
        m_deviceAction->setToolTip(
            QStringLiteral("Free %1 from PipeWire so muzaiten can open it directly.")
                .arg(dev->description));
    } else {
        m_deviceStatus->setText(QStringLiteral("✓ Free for direct (bit-perfect) playback."));
        m_deviceStatus->setStyleSheet(QStringLiteral("color: #27ae60;"));
        m_deviceAction->setText(QStringLiteral("Release"));
        m_deviceAction->setToolTip(
            QStringLiteral("Hand %1 back to PipeWire for shared use by other apps.")
                .arg(dev->description));
    }
}

void PlaybackProfileDialog::toggleSelectedDeviceTakeover()
{
    const QString hw = m_deviceCombo->currentData().toString().isEmpty()
        ? m_deviceCombo->currentText().trimmed()
        : m_deviceCombo->currentData().toString();
    const std::optional<AudioDeviceControl::DeviceState> dev =
        AudioDeviceControl::findByHwPath(hw);
    if (!dev)
        return;

    m_deviceAction->setEnabled(false);
    QString error;
    const bool ok = dev->heldByPipeWire()
        ? AudioDeviceControl::takeOver(*dev, &error)
        : AudioDeviceControl::release(*dev, /*restoreProfileIndex=*/-1, &error);
    m_deviceAction->setEnabled(true);

    if (!ok && !error.isEmpty()) {
        m_deviceStatus->setText(error);
        m_deviceStatus->setStyleSheet(QStringLiteral("color: #c0392b;"));
        return;
    }

    // Re-read the live state so the list colours and status reflect the change.
    populateDevices();
    refreshDeviceStatus();
}

void PlaybackProfileDialog::setProfile(const PlaybackProfile &profile)
{
    const int modeIndex = m_mode->findData(profile.mode);
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
    m_releaseSinkOnPause->setChecked(profile.releaseSinkOnPause);
    m_readAheadMb->setValue(std::clamp(profile.readAheadMb, 0, 1024));

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
        p.releaseSinkOnPause = true; // mandatory in bit-perfect
    } else {
        p.sink           = m_sink->currentData().toString();
        p.device.clear();
        p.softwareVolume     = m_softwareVolume->isChecked();
        p.allowResample      = m_allowResample->isChecked();
        p.releaseSinkOnPause = m_releaseSinkOnPause->isChecked();
    }
    p.readAheadMb = m_readAheadMb->value();

    return p;
}
