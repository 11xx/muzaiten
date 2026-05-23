#include "ui/PlayerBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace {

QToolButton *iconButton(QWidget *parent, QStyle::StandardPixmap icon, const QString &tooltip)
{
    auto *button = new QToolButton(parent);
    button->setIcon(parent->style()->standardIcon(icon));
    button->setToolTip(tooltip);
    button->setAutoRaise(true);
    button->setFixedSize(34, 34);
    return button;
}

QString formatTime(qint64 milliseconds)
{
    if (milliseconds <= 0) {
        return QStringLiteral("0:00");
    }

    const qint64 seconds = milliseconds / 1000;
    const qint64 minutes = seconds / 60;
    const qint64 hours = minutes / 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes % 60, 2, 10, QLatin1Char('0'))
            .arg(seconds % 60, 2, 10, QLatin1Char('0'));
    }

    return QStringLiteral("%1:%2")
        .arg(minutes)
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

PlayerBar::PlayerBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("PlayerBar"));
    setMinimumHeight(74);
    setMaximumHeight(86);

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(10);

    root->addWidget(iconButton(this, QStyle::SP_MediaSkipBackward, QStringLiteral("Previous")));
    m_playPause = iconButton(this, QStyle::SP_MediaPlay, QStringLiteral("Play"));
    root->addWidget(m_playPause);
    root->addWidget(iconButton(this, QStyle::SP_MediaSkipForward, QStringLiteral("Next")));

    auto *progressLayout = new QVBoxLayout;
    progressLayout->setContentsMargins(4, 0, 4, 0);
    progressLayout->setSpacing(4);

    m_nowPlaying = new QLabel(QStringLiteral("No track playing"), this);
    m_nowPlaying->setTextInteractionFlags(Qt::NoTextInteraction);
    m_nowPlaying->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    progressLayout->addWidget(m_nowPlaying);

    auto *timeline = new QHBoxLayout;
    timeline->setContentsMargins(0, 0, 0, 0);
    timeline->setSpacing(8);

    m_elapsed = new QLabel(QStringLiteral("0:00"), this);
    m_elapsed->setMinimumWidth(42);
    m_elapsed->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    timeline->addWidget(m_elapsed);

    m_progress = new QSlider(Qt::Horizontal, this);
    m_progress->setRange(0, 0);
    m_progress->setEnabled(false);
    timeline->addWidget(m_progress, 1);

    m_duration = new QLabel(QStringLiteral("0:00"), this);
    m_duration->setMinimumWidth(42);
    m_duration->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    timeline->addWidget(m_duration);

    progressLayout->addLayout(timeline);
    root->addLayout(progressLayout, 1);

    auto *stop = iconButton(this, QStyle::SP_MediaStop, QStringLiteral("Stop"));
    root->addWidget(stop);

    connect(m_playPause, &QToolButton::clicked, this, &PlayerBar::playPauseRequested);
    connect(stop, &QToolButton::clicked, this, &PlayerBar::stopRequested);
    connect(m_progress, &QSlider::sliderMoved, this, [this](int value) {
        emit seekRequested(value);
    });
}

void PlayerBar::setTrackText(const QString &text)
{
    m_hasTrack = !text.trimmed().isEmpty();
    m_nowPlaying->setText(m_hasTrack ? text : QStringLiteral("No track playing"));
    m_progress->setEnabled(m_hasTrack);
}

void PlayerBar::setPlaying(bool playing)
{
    m_playPause->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    m_playPause->setToolTip(playing ? QStringLiteral("Pause") : QStringLiteral("Play"));
}

void PlayerBar::setPosition(qint64 positionMs, qint64 durationMs)
{
    const qint64 safeDuration = std::max<qint64>(0, durationMs);
    const qint64 safePosition = std::clamp<qint64>(positionMs, 0, safeDuration);

    m_elapsed->setText(formatTime(safePosition));
    m_duration->setText(formatTime(safeDuration));
    m_progress->setEnabled(m_hasTrack && safeDuration > 0);
    m_progress->setRange(0, static_cast<int>(std::min<qint64>(safeDuration, std::numeric_limits<int>::max())));
    if (!m_progress->isSliderDown()) {
        m_progress->setValue(static_cast<int>(std::min<qint64>(safePosition, std::numeric_limits<int>::max())));
    }
}
