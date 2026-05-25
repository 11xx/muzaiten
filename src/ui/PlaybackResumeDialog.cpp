#include "ui/PlaybackResumeDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QVBoxLayout>

PlaybackResumeDialog::PlaybackResumeDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Playback resume"));
    resize(460, 170);

    auto *layout = new QVBoxLayout(this);
    auto *description = new QLabel(QStringLiteral(
        "Persist the current playback position and restore the last playback state on restart."));
    description->setWordWrap(true);
    layout->addWidget(description);

    m_savePosition = new QCheckBox(QStringLiteral("Save current track position"), this);
    m_savePosition->setChecked(true);
    layout->addWidget(m_savePosition);

    m_restorePlaybackState = new QCheckBox(QStringLiteral("Restore playback state on startup"), this);
    m_restorePlaybackState->setChecked(true);
    layout->addWidget(m_restorePlaybackState);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void PlaybackResumeDialog::setSavePositionEnabled(bool enabled)
{
    m_savePosition->setChecked(enabled);
}

bool PlaybackResumeDialog::savePositionEnabled() const
{
    return m_savePosition->isChecked();
}

void PlaybackResumeDialog::setRestorePlaybackStateEnabled(bool enabled)
{
    m_restorePlaybackState->setChecked(enabled);
}

bool PlaybackResumeDialog::restorePlaybackStateEnabled() const
{
    return m_restorePlaybackState->isChecked();
}
