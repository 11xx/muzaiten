#include "ui/PlayerBar.h"

#include "ui/StarRating.h"

#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QEvent>

#include <algorithm>
#include <functional>
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

    int rating() const
    {
        return m_rating0To100;
    }

    std::function<void(int)> ratingChanged;

protected:
    void mouseMoveEvent(QMouseEvent *event) override
    {
        m_hoverRating0To100 = StarRating::ratingFromPosition(StarRating::ratingRect(rect(), 18), event->pos());
        update();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            return;
        }

        const int rating = StarRating::ratingFromPosition(StarRating::ratingRect(rect(), 18), event->pos());
        if (rating >= 0 && ratingChanged) {
            ratingChanged(rating);
        }
    }

    void leaveEvent(QEvent *) override
    {
        m_hoverRating0To100 = StarRating::unset;
        update();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        StarRating::paint(&painter, StarRating::ratingRect(rect(), 18), m_rating0To100, m_hoverRating0To100, palette(), 18);
    }

private:
    int m_rating0To100 = StarRating::unset;
    int m_hoverRating0To100 = StarRating::unset;
};

class VolumeButton final : public QToolButton {
public:
    explicit VolumeButton(QWidget *parent = nullptr)
        : QToolButton(parent)
    {
        setIcon(parent->style()->standardIcon(QStyle::SP_MediaVolume));
        setToolTip(QStringLiteral("Volume"));
        setAutoRaise(true);
        setFixedSize(34, 34);

        auto *slider = new QSlider(Qt::Vertical);
        slider->setRange(0, 100);
        slider->setValue(100);
        slider->setFixedHeight(120);
        slider->setFixedWidth(36);
        auto *action = new QWidgetAction(this);
        action->setDefaultWidget(slider);
        m_menu.addAction(action);
        m_menu.installEventFilter(this);
        connect(slider, &QSlider::valueChanged, this, [this](int value) {
            if (volumeChanged) {
                volumeChanged(value);
            }
        });
    }

    std::function<void(int)> volumeChanged;

protected:
    void enterEvent(QEnterEvent *) override
    {
        const QPoint at = mapToGlobal(QPoint(0, height()));
        m_menu.popup(at);
    }

    void leaveEvent(QEvent *) override
    {
        closeIfPointerOutsideSoon();
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == &m_menu && event->type() == QEvent::Leave) {
            closeIfPointerOutsideSoon();
        }
        return QToolButton::eventFilter(watched, event);
    }

private:
    void closeIfPointerOutsideSoon()
    {
        QTimer::singleShot(150, this, [this]() {
            const QPoint globalPos = QCursor::pos();
            if (!rect().contains(mapFromGlobal(globalPos)) && !m_menu.geometry().contains(globalPos)) {
                m_menu.close();
            }
        });
    }

    QMenu m_menu;
};

class ClickSeekSlider final : public QSlider {
public:
    explicit ClickSeekSlider(Qt::Orientation orientation, QWidget *parent = nullptr)
        : QSlider(orientation, parent)
    {
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && orientation() == Qt::Horizontal && minimum() < maximum()) {
            const int clickX = std::clamp(static_cast<int>(event->position().x()), 0, std::max(1, width()));
            const int newValue = QStyle::sliderValueFromPosition(minimum(),
                                                                 maximum(),
                                                                 clickX,
                                                                 std::max(1, width()));
            setSliderPosition(newValue);
            setValue(newValue);
            setSliderDown(true);
            emit sliderPressed();
            emit sliderMoved(newValue);
            emit actionTriggered(QAbstractSlider::SliderMove);
        }
        QSlider::mousePressEvent(event);
    }
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

QToolButton *menuButton(QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setToolTip(QStringLiteral("Menu"));
    button->setAutoRaise(true);
    return button;
}

QIcon menuHamburgerIcon(const QPalette &palette)
{
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(palette.color(QPalette::ButtonText), 2.0);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);

    painter.drawLine(QPointF(5, 7), QPointF(19, 7));
    painter.drawLine(QPointF(5, 12), QPointF(19, 12));
    painter.drawLine(QPointF(5, 17), QPointF(19, 17));
    return QIcon(pixmap);
}

QIcon shuffleIcon(const QPalette &palette)
{
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(palette.color(QPalette::ButtonText), 1.8);
    painter.setPen(pen);

    QPainterPath topPath;
    topPath.moveTo(QPointF(4, 7));
    topPath.lineTo(QPointF(9, 7));
    topPath.cubicTo(QPointF(13, 7), QPointF(14, 17), QPointF(19, 17));
    painter.drawPath(topPath);
    painter.drawLine(QPointF(16, 14), QPointF(20, 17));
    painter.drawLine(QPointF(16, 20), QPointF(20, 17));

    QPainterPath bottomPath;
    bottomPath.moveTo(QPointF(4, 17));
    bottomPath.lineTo(QPointF(9, 17));
    bottomPath.cubicTo(QPointF(13, 17), QPointF(14, 7), QPointF(19, 7));
    painter.drawPath(bottomPath);
    painter.drawLine(QPointF(16, 4), QPointF(20, 7));
    painter.drawLine(QPointF(16, 10), QPointF(20, 7));

    return QIcon(pixmap);
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
    setMinimumHeight(82);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *compactMenu = new QMenu(this);
    auto *fileMenu = new QMenu(QStringLiteral("File"), this);
    QAction *openLibrary = fileMenu->addAction(QStringLiteral("Open library folder..."));
    QAction *sourceDirectories = fileMenu->addAction(QStringLiteral("Source directories..."));
    QAction *scanEnabledSources = fileMenu->addAction(QStringLiteral("Scan enabled source directories"));
    QAction *linkRoots = fileMenu->addAction(QStringLiteral("Link roots..."));
    auto *ratingTagsMenu = fileMenu->addMenu(QStringLiteral("Rating tags"));
    QAction *syncCurrentTrackRatingTags = ratingTagsMenu->addAction(QStringLiteral("Sync current track rating to file"));
    QAction *syncCurrentArtistRatingTags = ratingTagsMenu->addAction(QStringLiteral("Sync current artist rated tracks"));
    QAction *syncAllSavedRatingTags = ratingTagsMenu->addAction(QStringLiteral("Sync all saved rated tracks"));
    QAction *retryPendingRatingTags = ratingTagsMenu->addAction(QStringLiteral("Retry pending rating writes"));

    auto *playbackMenu = new QMenu(QStringLiteral("Playback"), this);
    QAction *playbackOutput = playbackMenu->addAction(QStringLiteral("Output profile..."));
    QAction *playbackResume = playbackMenu->addAction(QStringLiteral("Resume behavior..."));

    auto *mpdMenu = new QMenu(QStringLiteral("MPD"), this);
    QAction *mpdSource = mpdMenu->addAction(QStringLiteral("Configure MPD source..."));
    QAction *mpdImport = mpdMenu->addAction(QStringLiteral("Import MPD library metadata"));

    auto *scrobblersMenu = new QMenu(QStringLiteral("Scrobblers"), this);
    m_listenBrainzEnabled = scrobblersMenu->addAction(QStringLiteral("ListenBrainz scrobbling"));
    m_listenBrainzEnabled->setCheckable(true);
    QAction *listenBrainzToken = scrobblersMenu->addAction(QStringLiteral("Set ListenBrainz token..."));

    auto *settingsMenu = new QMenu(QStringLiteral("Settings"), this);
    m_trackInfoPaneVisible = settingsMenu->addAction(QStringLiteral("Show track information pane"));
    m_trackInfoPaneVisible->setCheckable(true);
    m_trackInfoPaneVisible->setChecked(true);
    QAction *trackInfoPaneSettings = settingsMenu->addAction(QStringLiteral("Track information panel..."));
    m_compactMenu = settingsMenu->addAction(QStringLiteral("Use compact menu"));
    m_compactMenu->setCheckable(true);

    compactMenu->addMenu(fileMenu);
    compactMenu->addMenu(playbackMenu);
    compactMenu->addMenu(mpdMenu);
    compactMenu->addMenu(scrobblersMenu);
    compactMenu->addMenu(settingsMenu);

    m_menuStrip = new QWidget(this);
    m_menuStrip->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *menuStripLayout = new QHBoxLayout(m_menuStrip);
    menuStripLayout->setContentsMargins(0, 0, 0, 0);
    menuStripLayout->setSpacing(0);
    menuStripLayout->setAlignment(Qt::AlignLeft);

    m_menuButton = menuButton(m_menuStrip);
    connect(m_menuButton, &QToolButton::clicked, this, [this, compactMenu]() {
        compactMenu->popup(m_menuButton->mapToGlobal(QPoint(0, m_menuButton->height())));
    });

    m_menuBar = new QMenuBar(m_menuStrip);
    m_menuBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    const int menuStripHeight = m_menuBar->fontMetrics().height() + 2;
    m_menuStrip->setFixedHeight(menuStripHeight);
    m_menuButton->setFixedSize(30, menuStripHeight);
    m_menuButton->setIconSize(QSize(18, 18));
    m_menuButton->setIcon(menuHamburgerIcon(palette()));
    m_menuBar->setFixedHeight(menuStripHeight);
    m_menuBar->setContentsMargins(0, 0, 0, 0);
    restyleMenuBar();
    m_menuBar->addMenu(fileMenu);
    m_menuBar->addMenu(playbackMenu);
    m_menuBar->addMenu(mpdMenu);
    m_menuBar->addMenu(scrobblersMenu);
    m_menuBar->addMenu(settingsMenu);
    menuStripLayout->addWidget(m_menuButton);
    menuStripLayout->addWidget(m_menuBar, 1);
    root->addWidget(m_menuStrip);

    auto *controls = new QHBoxLayout;
    controls->setContentsMargins(8, 6, 10, 8);
    controls->setSpacing(10);

    auto *previous = iconButton(this, QStyle::SP_MediaSkipBackward, QStringLiteral("Previous"));
    controls->addWidget(previous);
    m_playPause = iconButton(this, QStyle::SP_MediaPlay, QStringLiteral("Play"));
    controls->addWidget(m_playPause);
    auto *next = iconButton(this, QStyle::SP_MediaSkipForward, QStringLiteral("Next"));
    controls->addWidget(next);

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
    textLayout->addWidget(m_subtitle);
    metaLayout->addLayout(textLayout, 1);

    auto *rating = new RatingStrip(this);
    rating->ratingChanged = [this](int value) {
        emit currentTrackRatingChanged(value);
    };
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

    m_progress = new ClickSeekSlider(Qt::Horizontal, this);
    m_progress->setRange(0, 0);
    m_progress->setEnabled(false);
    timeline->addWidget(m_progress, 1);

    m_duration = new QLabel(QStringLiteral("0:00"), this);
    m_duration->setMinimumWidth(42);
    m_duration->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    timeline->addWidget(m_duration);

    progressLayout->addLayout(timeline);
    controls->addLayout(progressLayout, 1);

    auto *volume = new VolumeButton(this);
    volume->volumeChanged = [this](int value) {
        emit volumeChanged(value);
    };
    controls->addWidget(volume);

    auto *single = iconButton(this, QStyle::SP_BrowserReload, QStringLiteral("Single mode"));
    single->setCheckable(true);
    controls->addWidget(single);

    m_shuffle = new QToolButton(this);
    m_shuffle->setToolTip(QStringLiteral("Shuffle"));
    m_shuffle->setAutoRaise(true);
    m_shuffle->setFixedSize(34, 34);
    m_shuffle->setCheckable(true);
    updateShuffleIcon();
    controls->addWidget(m_shuffle);
    root->addLayout(controls);

    connect(previous, &QToolButton::clicked, this, &PlayerBar::previousRequested);
    connect(openLibrary, &QAction::triggered, this, &PlayerBar::openLibraryRequested);
    connect(sourceDirectories, &QAction::triggered, this, &PlayerBar::sourceDirectoriesRequested);
    connect(scanEnabledSources, &QAction::triggered, this, &PlayerBar::scanEnabledSourcesRequested);
    connect(syncCurrentTrackRatingTags, &QAction::triggered, this, &PlayerBar::syncCurrentTrackRatingTagsRequested);
    connect(syncCurrentArtistRatingTags, &QAction::triggered, this, &PlayerBar::syncCurrentArtistRatingTagsRequested);
    connect(syncAllSavedRatingTags, &QAction::triggered, this, &PlayerBar::syncAllSavedRatingTagsRequested);
    connect(retryPendingRatingTags, &QAction::triggered, this, &PlayerBar::retryPendingRatingTagsRequested);
    connect(playbackOutput, &QAction::triggered, this, &PlayerBar::playbackProfileRequested);
    connect(playbackResume, &QAction::triggered, this, &PlayerBar::playbackResumeRequested);
    connect(linkRoots, &QAction::triggered, this, &PlayerBar::linkRootsRequested);
    connect(mpdSource, &QAction::triggered, this, &PlayerBar::mpdSourceRequested);
    connect(mpdImport, &QAction::triggered, this, &PlayerBar::mpdImportRequested);
    connect(m_compactMenu, &QAction::toggled, this, &PlayerBar::compactMenuChanged);
    connect(m_trackInfoPaneVisible, &QAction::toggled, this, &PlayerBar::trackInfoPaneVisibleChanged);
    connect(trackInfoPaneSettings, &QAction::triggered, this, &PlayerBar::trackInfoPaneSettingsRequested);
    connect(m_listenBrainzEnabled, &QAction::toggled, this, &PlayerBar::listenBrainzEnabledChanged);
    connect(listenBrainzToken, &QAction::triggered, this, &PlayerBar::listenBrainzTokenRequested);
    connect(m_playPause, &QToolButton::clicked, this, &PlayerBar::playPauseRequested);
    connect(next, &QToolButton::clicked, this, &PlayerBar::nextRequested);
    connect(m_progress, &QSlider::sliderMoved, this, [this](int value) {
        emit seekRequested(value);
    });

    setCompactMenu(false);
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

void PlayerBar::setListenBrainzEnabled(bool enabled)
{
    if (m_listenBrainzEnabled == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_listenBrainzEnabled);
    m_listenBrainzEnabled->setChecked(enabled);
}

void PlayerBar::setTrackInfoPaneVisible(bool visible)
{
    if (m_trackInfoPaneVisible == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_trackInfoPaneVisible);
    m_trackInfoPaneVisible->setChecked(visible);
}

void PlayerBar::setCompactMenu(bool compact)
{
    if (m_compactMenu != nullptr) {
        const QSignalBlocker blocker(m_compactMenu);
        m_compactMenu->setChecked(compact);
    }
    if (m_menuButton != nullptr) {
        m_menuButton->setVisible(compact);
    }
    if (m_menuBar != nullptr) {
        m_menuBar->setVisible(!compact);
    }
    setMinimumHeight(82);
    updateGeometry();
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

void PlayerBar::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::StyleChange) {
        restyleMenuBar();
        if (m_menuButton != nullptr) {
            m_menuButton->setIcon(menuHamburgerIcon(palette()));
        }
        updateShuffleIcon();
    }
}

void PlayerBar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
}

void PlayerBar::restyleMenuBar()
{
    if (m_menuBar == nullptr) {
        return;
    }
    m_menuBar->setStyleSheet(QStringLiteral(
        "QMenuBar {"
        "  margin: 0;"
        "  padding: 0;"
        "  background: palette(window);"
        "  border-top: 1px solid palette(mid);"
        "  border-bottom: 1px solid palette(mid);"
        "}"
        "QMenuBar::item {"
        "  margin: 0;"
        "  padding: 0 10px;"
        "  border-right: 1px solid palette(mid);"
        "  background: transparent;"
        "}"
        "QMenuBar::item:selected {"
        "  background: palette(highlight);"
        "  color: palette(highlighted-text);"
        "}"
        "QMenuBar::item:pressed {"
        "  background: palette(dark);"
        "  color: palette(highlighted-text);"
        "}"
        "QToolButton {"
        "  margin: 0;"
        "  padding: 0;"
        "  border: 0;"
        "  border-right: 1px solid palette(mid);"
        "  background: transparent;"
        "}"
        "QToolButton:hover {"
        "  background: palette(highlight);"
        "  color: palette(highlighted-text);"
        "}"
        "QToolButton:pressed {"
        "  background: palette(dark);"
        "}"));
}

void PlayerBar::updateShuffleIcon()
{
    if (m_shuffle != nullptr) {
        m_shuffle->setIcon(shuffleIcon(palette()));
    }
}
