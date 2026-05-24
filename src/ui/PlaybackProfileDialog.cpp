#include "ui/PlaybackProfileDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QVBoxLayout>

PlaybackProfileDialog::PlaybackProfileDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Playback output"));

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;

    m_mode = new QComboBox(this);
    m_mode->addItem(QStringLiteral("Shared"), QStringLiteral("shared"));
    m_mode->addItem(QStringLiteral("Exclusive"), QStringLiteral("exclusive"));
    form->addRow(QStringLiteral("Mode"), m_mode);

    m_sink = new QComboBox(this);
    m_sink->addItem(QStringLiteral("Auto"), QStringLiteral("auto"));
    m_sink->addItem(QStringLiteral("PipeWire"), QStringLiteral("pipewire"));
    m_sink->addItem(QStringLiteral("PulseAudio"), QStringLiteral("pulse"));
    m_sink->addItem(QStringLiteral("ALSA"), QStringLiteral("alsa"));
    form->addRow(QStringLiteral("Sink"), m_sink);

    m_device = new QLineEdit(this);
    m_device->setPlaceholderText(QStringLiteral("hw:CARD=Pro"));
    form->addRow(QStringLiteral("Device"), m_device);

    m_softwareVolume = new QCheckBox(QStringLiteral("Software volume"), this);
    form->addRow(QString(), m_softwareVolume);

    m_allowResample = new QCheckBox(QStringLiteral("Allow resampling"), this);
    form->addRow(QString(), m_allowResample);

    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void PlaybackProfileDialog::setProfile(const PlaybackProfile &profile)
{
    const int modeIndex = m_mode->findData(profile.mode);
    m_mode->setCurrentIndex(modeIndex >= 0 ? modeIndex : 0);
    const int sinkIndex = m_sink->findData(profile.sink);
    m_sink->setCurrentIndex(sinkIndex >= 0 ? sinkIndex : 0);
    m_device->setText(profile.device);
    m_softwareVolume->setChecked(profile.softwareVolume);
    m_allowResample->setChecked(profile.allowResample);
}

PlaybackProfile PlaybackProfileDialog::profile() const
{
    PlaybackProfile profile;
    profile.id = QStringLiteral("custom");
    profile.name = QStringLiteral("Custom output");
    profile.backend = QStringLiteral("gstreamer");
    profile.mode = m_mode->currentData().toString();
    profile.sink = m_sink->currentData().toString();
    profile.device = m_device->text().trimmed();
    profile.softwareVolume = m_softwareVolume->isChecked();
    profile.allowResample = m_allowResample->isChecked();
    profile.replayGain = false;
    return profile;
}
