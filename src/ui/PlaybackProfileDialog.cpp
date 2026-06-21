#include "ui/PlaybackProfileDialog.h"

#include "playback/AudioDeviceControl.h"

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
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

    // --- The two modes, both visible side by side ---------------------------
    // Each is a checkable group box: ticking one unchecks the other (so its
    // contents grey out), and exactly one is always active. This shows the full
    // configuration of both modes at a glance instead of swapping a single form.
    auto *modesRow = new QHBoxLayout;

    m_sharedGroup = new QGroupBox(QStringLiteral("Shared mode"), this);
    m_sharedGroup->setCheckable(true);
    m_sharedGroup->setToolTip(
        QStringLiteral("Mix with other apps through PipeWire/PulseAudio."));
    auto *sharedForm = new QFormLayout(m_sharedGroup);

    m_sink = new QComboBox(m_sharedGroup);
    m_sink->addItem(QStringLiteral("Auto"),       QStringLiteral("auto"));
    m_sink->addItem(QStringLiteral("PipeWire"),   QStringLiteral("pipewire"));
    m_sink->addItem(QStringLiteral("PulseAudio"), QStringLiteral("pulse"));
    m_sink->addItem(QStringLiteral("ALSA"),       QStringLiteral("alsa"));
    sharedForm->addRow(QStringLiteral("Sink"), m_sink);

    m_softwareVolume = new QCheckBox(QStringLiteral("App controls volume"), m_sharedGroup);
    m_softwareVolume->setToolTip(
        QStringLiteral("When off, the volume slider has no effect and the DAC/hardware controls level."));
    sharedForm->addRow(QString(), m_softwareVolume);

    m_allowResample = new QCheckBox(QStringLiteral("Allow resampling"), m_sharedGroup);
    m_allowResample->setToolTip(
        QStringLiteral("Allow GStreamer to convert sample rates. Normally off — PipeWire handles rate "
                       "negotiation. For DSD, off keeps native bit-perfect output; on decodes DSD to PCM."));
    sharedForm->addRow(QString(), m_allowResample);

    m_releaseSinkOnPause = new QCheckBox(QStringLiteral("Release output device on pause"), m_sharedGroup);
    m_releaseSinkOnPause->setToolTip(
        QStringLiteral("Free the audio device immediately on pause so other apps can use it. "
                       "Resume seeks back to the paused position."));
    sharedForm->addRow(QString(), m_releaseSinkOnPause);

    modesRow->addWidget(m_sharedGroup, 1);

    m_bitPerfectGroup = new QGroupBox(QStringLiteral("Bit-perfect mode"), this);
    m_bitPerfectGroup->setCheckable(true);
    m_bitPerfectGroup->setToolTip(
        QStringLiteral("Open a chosen ALSA device directly for unmodified, exclusive output."));
    auto *bpForm = new QFormLayout(m_bitPerfectGroup);

    m_deviceCombo = new QComboBox(m_bitPerfectGroup);
    m_deviceCombo->setEditable(true);
    m_deviceCombo->setInsertPolicy(QComboBox::NoInsert);
    m_deviceCombo->setPlaceholderText(QStringLiteral("hw:0"));
    populateDevices();
    bpForm->addRow(QStringLiteral("Device"), m_deviceCombo);

    // A card PipeWire holds cannot be opened directly, so offer a one-click
    // takeover (and release) here rather than reaching for an external script.
    m_deviceStatus = new QLabel(m_bitPerfectGroup);
    m_deviceStatus->setWordWrap(true);
    m_deviceAction = new QPushButton(m_bitPerfectGroup);
    m_deviceAction->setAutoDefault(false);
    auto *deviceStatusRow = new QHBoxLayout;
    deviceStatusRow->setContentsMargins(0, 0, 0, 0);
    deviceStatusRow->addWidget(m_deviceStatus, 1);
    deviceStatusRow->addWidget(m_deviceAction, 0);
    bpForm->addRow(QString(), deviceStatusRow);

    connect(m_deviceCombo, &QComboBox::currentTextChanged, this, [this]() {
        refreshDeviceStatus();
    });
    connect(m_deviceAction, &QPushButton::clicked, this, [this]() {
        toggleSelectedDeviceTakeover();
    });

    modesRow->addWidget(m_bitPerfectGroup, 1);
    layout->addLayout(modesRow);

    // Seed the initial active mode before wiring exclusivity, so the toggled
    // handlers only fire on real user/setProfile changes.
    m_sharedGroup->setChecked(true);
    m_bitPerfectGroup->setChecked(false);
    connect(m_sharedGroup, &QGroupBox::toggled, this, [this](bool on) {
        if (on) {
            QSignalBlocker block(m_bitPerfectGroup);
            m_bitPerfectGroup->setChecked(false);
        } else if (!m_bitPerfectGroup->isChecked()) {
            QSignalBlocker block(m_sharedGroup);  // never leave both off
            m_sharedGroup->setChecked(true);
        }
    });
    connect(m_bitPerfectGroup, &QGroupBox::toggled, this, [this](bool on) {
        if (on) {
            QSignalBlocker block(m_sharedGroup);
            m_sharedGroup->setChecked(false);
            refreshDeviceStatus();
        } else if (!m_sharedGroup->isChecked()) {
            QSignalBlocker block(m_bitPerfectGroup);
            m_bitPerfectGroup->setChecked(true);
        }
    });

    // --- Cross-mode options (apply regardless of the active mode) ------------
    auto *bottomForm = new QFormLayout;

    // Auto-release applies to any exclusively-held card (bit-perfect device or a
    // DSD takeover in shared mode), so it lives outside the mode groups and stays
    // enabled in both. Off by default — a takeover is deliberate.
    m_autoReleaseDevice = new QCheckBox(QStringLiteral("Auto-release a taken-over device after"), this);
    m_autoReleaseDevice->setToolTip(
        QStringLiteral("When muzaiten holds a card exclusively (bit-perfect, or a DSD device takeover), "
                       "hand it back to PipeWire automatically after playback stays paused or stopped for "
                       "this long. Off by default; otherwise it stays held until you release it via "
                       "Playback → Release device."));
    m_autoReleaseTimeout = new QSpinBox(this);
    m_autoReleaseTimeout->setRange(1, 600);
    m_autoReleaseTimeout->setSuffix(QStringLiteral(" s"));
    m_autoReleaseTimeout->setValue(15);
    m_autoReleaseTimeout->setEnabled(false);
    connect(m_autoReleaseDevice, &QCheckBox::toggled, m_autoReleaseTimeout, &QWidget::setEnabled);
    auto *autoReleaseRow = new QHBoxLayout;
    autoReleaseRow->setContentsMargins(0, 0, 0, 0);
    autoReleaseRow->addWidget(m_autoReleaseDevice);
    autoReleaseRow->addWidget(m_autoReleaseTimeout);
    autoReleaseRow->addStretch(1);
    bottomForm->addRow(QString(), autoReleaseRow);

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
    bottomForm->addRow(QStringLiteral("Disk read-ahead"), readAheadRow);

    layout->addLayout(bottomForm);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    layout->activate();
    setMinimumSize(sizeHint());
}

bool PlaybackProfileDialog::bitPerfectActive() const
{
    return m_bitPerfectGroup->isChecked();
}

void PlaybackProfileDialog::setModeActive(bool bitPerfect)
{
    // Check the desired group; its toggled handler unchecks (greys out) the other
    // and refreshes device status when bit-perfect becomes active.
    if (bitPerfect) {
        m_bitPerfectGroup->setChecked(true);
    } else {
        m_sharedGroup->setChecked(true);
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
    if (!bitPerfectActive())
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
    // Seed the shared sub-form from the remembered shared selections so they are
    // restored even when the active mode is bit-perfect (where they were pinned).
    // Empty remembered sink defaults to "Auto", never "ALSA".
    const QString sharedSink = profile.sharedSink.isEmpty()
        ? QStringLiteral("auto") : profile.sharedSink;
    const int sinkIndex = m_sink->findData(sharedSink);
    m_sink->setCurrentIndex(sinkIndex >= 0 ? sinkIndex : 0);
    m_softwareVolume->setChecked(profile.sharedSoftwareVolume);
    m_allowResample->setChecked(profile.sharedAllowResample);
    m_releaseSinkOnPause->setChecked(profile.sharedReleaseSinkOnPause);

    // Bit-perfect device: prefer the live hw:N for the remembered stable id (the
    // stored index may have drifted), falling back to the stored hw:N / text.
    QString device = profile.device;
    if (!profile.deviceId.isEmpty()) {
        if (const auto dev = AudioDeviceControl::findByStableId(profile.deviceId);
            dev && !dev->hwPath.isEmpty()) {
            device = dev->hwPath;
        }
    }
    if (!device.isEmpty()) {
        const int devIndex = m_deviceCombo->findData(device);
        if (devIndex >= 0)
            m_deviceCombo->setCurrentIndex(devIndex);
        else
            m_deviceCombo->setCurrentText(device);
    }

    m_readAheadMb->setValue(std::clamp(profile.readAheadMb, 0, 1024));

    m_autoReleaseDevice->setChecked(profile.autoReleaseExclusiveDevice);
    m_autoReleaseTimeout->setValue(std::clamp(profile.autoReleaseTimeoutSec, 1, 600));
    m_autoReleaseTimeout->setEnabled(profile.autoReleaseExclusiveDevice);

    setModeActive(profile.mode == QStringLiteral("bit-perfect"));
}

PlaybackProfile PlaybackProfileDialog::profile() const
{
    PlaybackProfile p;
    p.backend = QStringLiteral("gstreamer");
    p.mode    = bitPerfectActive() ? QStringLiteral("bit-perfect") : QStringLiteral("shared");
    p.replayGain = false;

    // Capture both sub-forms unconditionally so each mode's selection survives
    // while the other is active. The hidden widgets keep the values seeded in
    // setProfile, so reading them here is correct in either mode.
    p.sharedSink                = m_sink->currentData().toString();
    p.sharedSoftwareVolume      = m_softwareVolume->isChecked();
    p.sharedAllowResample       = m_allowResample->isChecked();
    p.sharedReleaseSinkOnPause  = m_releaseSinkOnPause->isChecked();

    // Prefer the item data (hw:N); fall back to the edited text for manual entries.
    const QString devData = m_deviceCombo->currentData().toString();
    const QString device = devData.isEmpty() ? m_deviceCombo->currentText().trimmed() : devData;
    // Persist the device's stable id (independent of the volatile hw:N) so it can
    // be re-resolved later regardless of which mode is active now.
    if (!device.isEmpty()) {
        if (const auto dev = AudioDeviceControl::findByHwPath(device);
            dev && !dev->stableId.isEmpty()) {
            p.deviceId = dev->stableId;
        }
    }

    if (p.mode == QStringLiteral("bit-perfect")) {
        p.sink   = QStringLiteral("alsa");
        p.device = device;
        p.softwareVolume = false;
        p.allowResample  = false;
        p.releaseSinkOnPause = true; // mandatory in bit-perfect
    } else {
        p.sink           = p.sharedSink;
        p.device.clear();
        p.softwareVolume     = p.sharedSoftwareVolume;
        p.allowResample      = p.sharedAllowResample;
        p.releaseSinkOnPause = p.sharedReleaseSinkOnPause;
    }
    p.readAheadMb = m_readAheadMb->value();
    p.autoReleaseExclusiveDevice = m_autoReleaseDevice->isChecked();
    p.autoReleaseTimeoutSec = m_autoReleaseTimeout->value();

    return p;
}
