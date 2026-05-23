#include "ui/PlayerBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

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
    root->addWidget(iconButton(this, QStyle::SP_MediaPlay, QStringLiteral("Play")));
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

    root->addWidget(iconButton(this, QStyle::SP_MediaStop, QStringLiteral("Stop")));
}

