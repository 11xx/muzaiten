#include "ui/PlayerBar.h"

#include "ui/StarRating.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace {

class RatingStrip final : public QWidget {
public:
    explicit RatingStrip(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setFixedSize(100, 22);
    }

    void setRating(int rating0To100)
    {
        m_rating0To100 = rating0To100;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        StarRating::paint(&painter, StarRating::ratingRect(rect(), 18), m_rating0To100, StarRating::unset, palette(), 18);
    }

private:
    int m_rating0To100 = StarRating::unset;
};

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
    setMinimumHeight(72);
    setMaximumHeight(82);

    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(10);

    auto *previous = iconButton(this, QStyle::SP_MediaSkipBackward, QStringLiteral("Previous"));
    root->addWidget(previous);
    m_playPause = iconButton(this, QStyle::SP_MediaPlay, QStringLiteral("Play"));
    root->addWidget(m_playPause);
    auto *next = iconButton(this, QStyle::SP_MediaSkipForward, QStringLiteral("Next"));
    root->addWidget(next);

    auto *progressLayout = new QVBoxLayout;
    progressLayout->setContentsMargins(4, 0, 4, 0);
    progressLayout->setSpacing(4);

    auto *metaLayout = new QHBoxLayout;
    metaLayout->setContentsMargins(0, 0, 0, 0);
    metaLayout->setSpacing(10);

    auto *textLayout = new QVBoxLayout;
    textLayout->setContentsMargins(0, 0, 0, 0);
    textLayout->setSpacing(0);
    m_title = new QLabel(QStringLiteral("No track playing"), this);
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    m_title->setTextInteractionFlags(Qt::NoTextInteraction);
    m_title->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    textLayout->addWidget(m_title);

    m_subtitle = new QLabel(this);
    m_subtitle->setTextInteractionFlags(Qt::NoTextInteraction);
    m_subtitle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_subtitle->setStyleSheet(QStringLiteral("color: palette(mid);"));
    textLayout->addWidget(m_subtitle);
    metaLayout->addLayout(textLayout, 1);

    auto *rating = new RatingStrip(this);
    m_rating = rating;
    metaLayout->addWidget(m_rating, 0, Qt::AlignVCenter);
    progressLayout->addLayout(metaLayout);

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

    auto *volume = new QSlider(Qt::Horizontal, this);
    volume->setRange(0, 100);
    volume->setValue(100);
    volume->setFixedWidth(100);
    root->addWidget(volume);

    auto *single = iconButton(this, QStyle::SP_BrowserReload, QStringLiteral("Single mode"));
    single->setCheckable(true);
    root->addWidget(single);

    auto *shuffle = iconButton(this, QStyle::SP_FileDialogDetailedView, QStringLiteral("Shuffle"));
    shuffle->setCheckable(true);
    root->addWidget(shuffle);

    connect(previous, &QToolButton::clicked, this, &PlayerBar::previousRequested);
    connect(m_playPause, &QToolButton::clicked, this, &PlayerBar::playPauseRequested);
    connect(next, &QToolButton::clicked, this, &PlayerBar::nextRequested);
    connect(volume, &QSlider::valueChanged, this, &PlayerBar::volumeChanged);
    connect(m_progress, &QSlider::sliderMoved, this, [this](int value) {
        emit seekRequested(value);
    });
}

void PlayerBar::setTrackText(const QString &text)
{
    setTrackInfo(text, {}, StarRating::unset);
}

void PlayerBar::setTrackInfo(const QString &title, const QString &subtitle, int rating0To100)
{
    m_hasTrack = !title.trimmed().isEmpty();
    m_title->setText(m_hasTrack ? title : QStringLiteral("No track playing"));
    m_subtitle->setText(subtitle);
    static_cast<RatingStrip *>(m_rating)->setRating(rating0To100);
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
