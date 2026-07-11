#include "ui/PlayerBar.h"

#include "ui/AlbumArtFallback.h"
#include "ui/AlbumArtView.h"
#include "ui/PanelBorderStyle.h"
#include "ui/StarRating.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCursor>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QStyleOption>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector>
#include <QWidgetAction>
#include <QEvent>

#include <algorithm>
#include <functional>
#include <limits>

namespace {

// A menu that stays open when a checkable action is clicked, so related
// toggles can be flipped without reopening it each time. Non-checkable
// actions close the menu as usual.
class StayOpenMenu final : public QMenu {
public:
    using QMenu::QMenu;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QAction *action = activeAction();
        if (action != nullptr && action->isEnabled() && action->isCheckable()) {
            action->trigger();
            return;
        }
        QMenu::mouseReleaseEvent(event);
    }
};

class EagerMenuBar final : public QMenuBar {
public:
    using QMenuBar::QMenuBar;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        QAction *action = actionAt(event->pos());
        if (event->button() == Qt::LeftButton && action != nullptr && action == activeAction()) {
            if (QMenu *menu = action->menu(); menu != nullptr && menu->isVisible()) {
                menu->hide();
                setActiveAction(nullptr);
                event->accept();
                return;
            }
        }
        QMenuBar::mousePressEvent(event);
    }
};

class MenuPaddingStyle final : public QProxyStyle {
public:
    MenuPaddingStyle()
        : QProxyStyle()
    {
    }

    int pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr, const QWidget *widget = nullptr) const override
    {
        if (metric == QStyle::PM_MenuButtonIndicator) {
            return QProxyStyle::pixelMetric(metric, option, widget) + 10;
        }
        return QProxyStyle::pixelMetric(metric, option, widget);
    }

    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget = nullptr) const override
    {
        if (element == QStyle::CE_MenuItem) {
            if (const auto *menuItem = qstyleoption_cast<const QStyleOptionMenuItem *>(option);
                menuItem != nullptr && option->state.testFlag(QStyle::State_Selected)) {
                painter->fillRect(option->rect, option->palette.highlight());
                QStyleOptionMenuItem adjusted(*menuItem);
                adjusted.state &= ~QStyle::State_Selected;
                adjusted.palette.setColor(QPalette::Text, option->palette.highlightedText().color());
                adjusted.palette.setColor(QPalette::ButtonText, option->palette.highlightedText().color());
                adjusted.palette.setColor(QPalette::WindowText, option->palette.highlightedText().color());
                QProxyStyle::drawControl(element, &adjusted, painter, widget);
                return;
            }
        }
        QProxyStyle::drawControl(element, option, painter, widget);
    }
};

class RatingStrip final : public QWidget {
public:
    explicit RatingStrip(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setFixedSize(100, 22);
        setMouseTracking(true);
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
        if (rating >= 0) {
            m_rating0To100 = rating;
            m_hoverRating0To100 = StarRating::unset;
            update();
            if (ratingChanged) {
                ratingChanged(rating);
            }
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

        m_slider = new QSlider(Qt::Vertical);
        m_slider->setRange(0, 100);
        m_slider->setValue(100);
        m_slider->setFixedHeight(120);
        m_slider->setFixedWidth(36);
        auto *action = new QWidgetAction(this);
        action->setDefaultWidget(m_slider);
        m_menu.addAction(action);
        m_menu.installEventFilter(this);
        connect(m_slider, &QSlider::valueChanged, this, [this](int value) {
            if (volumeChanged) {
                volumeChanged(value);
            }
        });
    }

    // Reflects an externally-restored volume without re-emitting volumeChanged.
    void setValue(int volume0To100)
    {
        const QSignalBlocker blocker(m_slider);
        m_slider->setValue(std::clamp(volume0To100, 0, 100));
    }

    void setVolumeControlEnabled(bool enabled)
    {
        setEnabled(enabled);
        m_slider->setEnabled(enabled);
        setToolTip(enabled
                       ? QStringLiteral("Volume")
                       : QStringLiteral("Volume disabled by the output profile"));
        if (!enabled) {
            m_menu.close();
        }
    }

    std::function<void(int)> volumeChanged;

protected:
    void enterEvent(QEnterEvent *) override
    {
        if (!isEnabled()) {
            return;
        }
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
    QSlider *m_slider = nullptr;
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

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        QSlider::mouseReleaseEvent(event);
        if (orientation() == Qt::Horizontal && isSliderDown()) {
            setSliderDown(false);
        }
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

// Vectorized from the radio-receiver reference artwork: a boxy receiver with
// a round dial, speaker slats, and an antenna broadcasting waves. Used for
// the Radio shuffle mode so it reads as "radio", not just badged shuffle.
void drawRadioReceiverGlyph(QPainter &painter, const QColor &color)
{
    QPen pen(color, 1.7);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    // Body, and the antenna reaching up to the broadcast point.
    painter.drawRoundedRect(QRectF(3.5, 11.0, 17.0, 10.0), 2.2, 2.2);
    painter.drawLine(QPointF(7.0, 11.0), QPointF(14.6, 5.7));
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QPointF(14.9, 5.4), 1.15, 1.15);

    // Broadcast waves fanning out above the antenna tip.
    painter.setBrush(Qt::NoBrush);
    QPen wave(color, 1.5);
    wave.setCapStyle(Qt::RoundCap);
    painter.setPen(wave);
    for (int i = 0; i < 2; ++i) {
        const qreal radius = 2.6 + i * 2.1;
        const QRectF rect(14.9 - radius, 5.4 - radius, radius * 2, radius * 2);
        painter.drawArc(rect, 40 * 16, 100 * 16);
    }

    // Dial and speaker slats on the face.
    QPen dial(color, 1.5);
    painter.setPen(dial);
    painter.drawEllipse(QPointF(8.7, 16.0), 2.3, 2.3);
    QPen slat(color, 1.3);
    slat.setCapStyle(Qt::RoundCap);
    painter.setPen(slat);
    painter.drawLine(QPointF(13.4, 13.9), QPointF(17.4, 13.9));
    painter.drawLine(QPointF(13.4, 16.0), QPointF(17.4, 16.0));
    painter.drawLine(QPointF(13.4, 18.1), QPointF(17.4, 18.1));
}

QIcon shuffleIcon(const QPalette &palette, ShuffleMode mode)
{
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // Off reads as a neutral glyph; an active mode is accented.
    const QColor color = mode == ShuffleMode::Off
        ? palette.color(QPalette::ButtonText)
        : palette.color(QPalette::Highlight);

    // Radio shuffle swaps the crossing arrows for the receiver glyph — the
    // mode is "let the radio pick", which the plain badge didn't convey.
    if (mode == ShuffleMode::Radio) {
        drawRadioReceiverGlyph(painter, color);
        return QIcon(pixmap);
    }

    QPen pen(color, 1.8);
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

    // Library-wide shuffle adds a small "+" badge: the queue plus the library.
    if (mode == ShuffleMode::Library) {
        QPen badge(color, 1.6);
        badge.setCapStyle(Qt::RoundCap);
        painter.setPen(badge);
        painter.drawLine(QPointF(21, 1.5), QPointF(21, 6.5));
        painter.drawLine(QPointF(18.5, 4), QPointF(23.5, 4));
    }
    return QIcon(pixmap);
}

QIcon repeatIcon(const QPalette &palette, RepeatMode mode)
{
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor color = mode == RepeatMode::Off
        ? palette.color(QPalette::ButtonText)
        : palette.color(QPalette::Highlight);
    QPen pen(color, 1.8);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);

    // A rounded-rectangle loop, broken on the right edge with an arrowhead at
    // the top-right re-entry point — the conventional "repeat" glyph.
    QPainterPath loop;
    loop.moveTo(QPointF(15, 5));
    loop.lineTo(QPointF(8, 5));
    loop.quadTo(QPointF(4, 5), QPointF(4, 9));
    loop.lineTo(QPointF(4, 15));
    loop.quadTo(QPointF(4, 19), QPointF(8, 19));
    loop.lineTo(QPointF(16, 19));
    loop.quadTo(QPointF(20, 19), QPointF(20, 15));
    loop.lineTo(QPointF(20, 12));
    painter.drawPath(loop);
    painter.drawLine(QPointF(15, 5), QPointF(11.5, 2.5));
    painter.drawLine(QPointF(15, 5), QPointF(11.5, 7.5));

    // Repeat-one marks the loop with a "1".
    if (mode == RepeatMode::One) {
        QFont font = painter.font();
        font.setPixelSize(11);
        font.setBold(true);
        painter.setFont(font);
        painter.drawText(QRectF(0, 1, 24, 24), Qt::AlignCenter, QStringLiteral("1"));
    }
    return QIcon(pixmap);
}

// Vectorized from the headphones-over-vinyl reference artwork: a filled
// record (punched label ring, groove arcs, center dot) framed by a headband
// and two ear cups. Replaces the generic wifi-arc glyph the radio-session
// indicator used to show.
QIcon radioIcon(const QPalette &palette)
{
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    // The button is only ever shown while a radio session is active, so the
    // glyph is always drawn in the accent color (no "off" variant needed).
    const QColor color = palette.color(QPalette::Highlight);

    // Headband arcing over the disc into the two ear cups, drawn first so the
    // disc can punch a separating gap into them (the reference art keeps
    // clear space between the record and the headphones).
    const QPointF discCenter(12.0, 14.0);
    QPen band(color, 2.4);
    band.setCapStyle(Qt::FlatCap);
    painter.setPen(band);
    painter.setBrush(Qt::NoBrush);
    const qreal bandRadius = 9.6;
    const QRectF bandRect(12.0 - bandRadius, 13.5 - bandRadius, bandRadius * 2, bandRadius * 2);
    painter.drawArc(bandRect, 20 * 16, 140 * 16);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(QRectF(0.8, 10.5, 4.0, 8.6), 1.8, 1.8);
    painter.drawRoundedRect(QRectF(19.2, 10.5, 4.0, 8.6), 1.8, 1.8);

    // The vinyl disc, with the label ring and groove arcs punched out of it
    // (Clear mode) so they read in whatever sits behind the toolbar. The
    // slightly larger clear disc first carves the gap around the record.
    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    painter.drawEllipse(discCenter, 8.2, 8.2);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.drawEllipse(discCenter, 7.2, 7.2);
    painter.setCompositionMode(QPainter::CompositionMode_Clear);
    painter.drawEllipse(discCenter, 3.0, 3.0);
    QPen groove(color, 1.0);
    groove.setCapStyle(Qt::RoundCap);
    painter.setPen(groove);
    painter.setBrush(Qt::NoBrush);
    const qreal grooveRadius = 5.2;
    const QRectF grooveRect(discCenter.x() - grooveRadius, discCenter.y() - grooveRadius,
                            grooveRadius * 2, grooveRadius * 2);
    painter.drawArc(grooveRect, 115 * 16, 55 * 16);
    painter.drawArc(grooveRect, 295 * 16, 55 * 16);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(discCenter, 1.1, 1.1);
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

bool isThemeChange(QEvent::Type type)
{
    return type == QEvent::PaletteChange
        || type == QEvent::ApplicationPaletteChange
        || type == QEvent::StyleChange;
}

void repolish(QWidget *widget)
{
    if (widget == nullptr || widget->style() == nullptr) {
        return;
    }
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

void styleMenu(QMenu *menu)
{
    if (menu == nullptr) {
        return;
    }
    auto *style = new MenuPaddingStyle;
    style->setParent(menu);
    menu->setStyle(style);
}

} // namespace

PlayerBar::PlayerBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("PlayerBar"));
    setMinimumHeight(82);
    restyleFrame();

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *compactMenu = new QMenu(this);
    auto *mpdMenu = new QMenu(QStringLiteral("MPD"), this);
    QAction *mpdSource = mpdMenu->addAction(QStringLiteral("Configure MPD source…"));
    QAction *mpdImport = mpdMenu->addAction(QStringLiteral("Import MPD library metadata"));

    // Everything in here manages the library (sources, scanning, tags) — no
    // file operations — hence the label.
    auto *fileMenu = new QMenu(QStringLiteral("Library"), this);
    QAction *openLibrary = fileMenu->addAction(QStringLiteral("Open library folder…"));
    QAction *sourceDirectories = fileMenu->addAction(QStringLiteral("Source directories…"));
    QAction *linkRoots = fileMenu->addAction(QStringLiteral("Link roots…"));
    fileMenu->addMenu(mpdMenu); // MPD is a library source, so it lives here
    fileMenu->addSeparator();
    QAction *scanEnabledSources = fileMenu->addAction(QStringLiteral("Scan enabled source directories"));
    QAction *forceRescan = fileMenu->addAction(QStringLiteral("Force full rescan"));
    auto *scanPowerMenu = fileMenu->addMenu(QStringLiteral("Scan power"));
    auto *scanPowerGroup = new QActionGroup(this);
    scanPowerGroup->setExclusive(true);
    const char *scanProfileLabels[3] = {"Background", "Balanced", "Turbo"};
    for (int i = 0; i < 3; ++i) {
        m_scanProfileActions[i] = scanPowerMenu->addAction(QString::fromLatin1(scanProfileLabels[i]));
        m_scanProfileActions[i]->setCheckable(true);
        scanPowerGroup->addAction(m_scanProfileActions[i]);
        connect(m_scanProfileActions[i], &QAction::triggered, this, [this, i]() { emit scanProfileChanged(i); });
    }
    m_scanProfileActions[1]->setChecked(true);  // Balanced default until set from settings
    m_showGuessedPlaceholders = fileMenu->addAction(QStringLiteral("Show guessed metadata while scanning"));
    m_showGuessedPlaceholders->setCheckable(true);
    connect(m_showGuessedPlaceholders, &QAction::toggled, this, &PlayerBar::showGuessedPlaceholdersChanged);
    QAction *removeMissingTracks = fileMenu->addAction(QStringLiteral("Remove missing tracks"));
    fileMenu->addSeparator();
    auto *audioAnalysisMenu = fileMenu->addMenu(QStringLiteral("Audio analysis"));
    m_audioAnalysisRunStatusAction = audioAnalysisMenu->addAction(QStringLiteral("Analysis idle"));
    m_audioAnalysisRunStatusAction->setEnabled(false);
    m_audioAnalysisRunStatusAction->setVisible(false);
    auto *analysisPowerMenu = audioAnalysisMenu->addMenu(QStringLiteral("Analysis power"));
    auto *analysisPowerGroup = new QActionGroup(this);
    analysisPowerGroup->setExclusive(true);
    const char *analysisPowerLabels[3] = {"Background", "Balanced", "Turbo"};
    for (int i = 0; i < 3; ++i) {
        m_analysisPowerActions[i] = analysisPowerMenu->addAction(QString::fromLatin1(analysisPowerLabels[i]));
        m_analysisPowerActions[i]->setCheckable(true);
        analysisPowerGroup->addAction(m_analysisPowerActions[i]);
        connect(m_analysisPowerActions[i], &QAction::triggered, this, [this, i]() { emit analysisPowerChanged(i); });
    }
    m_analysisPowerActions[0]->setChecked(true);
    m_semanticAnalysisEnabledAction = audioAnalysisMenu->addAction(
        QStringLiteral("Generate semantic/audio-similarity features"));
    m_semanticAnalysisEnabledAction->setCheckable(true);
    QAction *semanticProviderSetup = audioAnalysisMenu->addAction(QStringLiteral("Semantic provider setup…"));
    QAction *semanticModelDownload = audioAnalysisMenu->addAction(QStringLiteral("Download semantic model…"));
    audioAnalysisMenu->addSeparator();
    m_analyzeAudioAction = audioAnalysisMenu->addAction(QStringLiteral("Analyze library audio"));
    m_cancelAudioAnalysisAction = audioAnalysisMenu->addAction(QStringLiteral("Stop analysis"));
    m_cancelAudioAnalysisAction->setVisible(false);
    audioAnalysisMenu->addSeparator();
    QAction *analysisStatus = audioAnalysisMenu->addAction(QStringLiteral("Analysis status…"));
    QAction *duplicateCopies = audioAnalysisMenu->addAction(QStringLiteral("Duplicate copies…"));
    auto *ratingTagsMenu = fileMenu->addMenu(QStringLiteral("Rating tags"));
    QAction *syncCurrentTrackRatingTags = ratingTagsMenu->addAction(QStringLiteral("Sync current track rating to file"));
    QAction *syncCurrentArtistRatingTags = ratingTagsMenu->addAction(QStringLiteral("Sync current artist rated tracks"));
    QAction *syncAllSavedRatingTags = ratingTagsMenu->addAction(QStringLiteral("Sync all saved rated tracks"));
    QAction *retryPendingRatingTags = ratingTagsMenu->addAction(QStringLiteral("Retry pending rating writes"));

    auto *playbackMenu = new QMenu(QStringLiteral("Playback"), this);
    QAction *findCurrentTrack = playbackMenu->addAction(QStringLiteral("Find current track in library"));
    QAction *playbackOutput = playbackMenu->addAction(QStringLiteral("Output profile…"));
    m_releaseDeviceAction = playbackMenu->addAction(QStringLiteral("Release device"));
    m_releaseDeviceAction->setVisible(false);
    m_releaseDeviceAction->setToolTip(
        QStringLiteral("Hand the held output device back to PipeWire (switches to shared output)"));
    // Availability depends on live device-hold state (DSD takeover or a bit-perfect
    // profile holding a card), so refresh it each time the menu opens.
    connect(playbackMenu, &QMenu::aboutToShow, this, &PlayerBar::playbackMenuAboutToShow);
    QAction *playbackResume = playbackMenu->addAction(QStringLiteral("Resume behavior…"));
    QAction *libraryShuffleSettings = playbackMenu->addAction(QStringLiteral("Library shuffle…"));

    // Radio gets a top-level menu: session control, the mixes, and every
    // radio setting reachable WITHOUT an active session (the radio indicator
    // button's menu mirrors the session controls but only exists mid-session).
    auto *radioMenu = new QMenu(QStringLiteral("Radio"), this);
    QAction *startRadioCurrent = radioMenu->addAction(QStringLiteral("Start radio from current track"));
    QAction *startArtistRadioCurrent = radioMenu->addAction(QStringLiteral("Start artist radio"));
    m_stopRadioAction = radioMenu->addAction(QStringLiteral("Stop radio"));
    radioMenu->addSeparator();
    QAction *rediscoveryMix = radioMenu->addAction(QStringLiteral("Play Rediscovery mix"));
    QAction *deepCutsMix = radioMenu->addAction(QStringLiteral("Play Deep cuts mix"));
    radioMenu->addSeparator();
    m_radioBarAdventurousAction = radioMenu->addAction(QStringLiteral("Adventurous (this session)"));
    m_radioBarAdventurousAction->setCheckable(true);
    connect(m_radioBarAdventurousAction, &QAction::toggled, this, &PlayerBar::radioAdventurousChanged);
    QAction *radioExplorationBar = radioMenu->addAction(QStringLiteral("Exploration…"));
    connect(radioExplorationBar, &QAction::triggered, this, &PlayerBar::radioExplorationSettingsRequested);
    QAction *radioBatchSizeBar = radioMenu->addAction(QStringLiteral("Radio batch size…"));
    connect(radioBatchSizeBar, &QAction::triggered, this, &PlayerBar::radioBatchSizeSettingsRequested);
    QAction *radioShuffleSettings = radioMenu->addAction(QStringLiteral("Radio shuffle percent…"));
    QAction *scoringWeights = radioMenu->addAction(QStringLiteral("Scoring weights…"));
    radioMenu->addSeparator();
    QAction *genreCuration = radioMenu->addAction(QStringLiteral("Genre curation…"));
    connect(radioMenu, &QMenu::aboutToShow, this, [this]() {
        m_stopRadioAction->setEnabled(m_radioActive);
        m_radioBarAdventurousAction->setEnabled(m_radioActive);
        emit radioMenuAboutToShow();
    });

    auto *historyMenu = new QMenu(QStringLiteral("History"), this);
    QAction *listeningHistory = historyMenu->addAction(QStringLiteral("Listening history…"));
    auto *scrobblersMenu = new StayOpenMenu(QStringLiteral("Scrobblers"), this);
    m_listenBrainzEnabled = scrobblersMenu->addAction(QStringLiteral("ListenBrainz scrobbling"));
    m_listenBrainzEnabled->setCheckable(true);
    QAction *listenBrainzToken = scrobblersMenu->addAction(QStringLiteral("Set ListenBrainz token…"));
    m_clearListenBrainzBacklog = scrobblersMenu->addAction(QStringLiteral("Clear ListenBrainz backlog"));
    m_clearListenBrainzBacklog->setVisible(false);
    scrobblersMenu->addSeparator();
    m_lastFmEnabled = scrobblersMenu->addAction(QStringLiteral("Last.fm scrobbling"));
    m_lastFmEnabled->setCheckable(true);
    QAction *lastFmSettings = scrobblersMenu->addAction(QStringLiteral("Last.fm API settings…"));
    m_clearLastFmBacklog = scrobblersMenu->addAction(QStringLiteral("Clear Last.fm backlog"));
    m_clearLastFmBacklog->setVisible(false);
    scrobblersMenu->addSeparator();
    m_scrobbleOffline = scrobblersMenu->addAction(QStringLiteral("Offline mode (buffer listens locally)"));
    m_scrobbleOffline->setCheckable(true);
    m_scrobbleOffline->setToolTip(QStringLiteral("Keep collecting listening history but send nothing; unchecking uploads the buffered backlog."));
    scrobblersMenu->addSeparator();
    // Backfill status + controls: live progress text (disabled, display-only),
    // the two import triggers, and the one action that stops eager auto-resume.
    m_backfillStatusAction = scrobblersMenu->addAction(QStringLiteral("Backfill idle"));
    m_backfillStatusAction->setEnabled(false);
    m_backfillStatusAction->setVisible(false);
    m_importListenBrainzAction = scrobblersMenu->addAction(QStringLiteral("Import ListenBrainz history"));
    m_syncLastFmAction = scrobblersMenu->addAction(QStringLiteral("Sync Last.fm play counts"));
    m_cancelBackfillAction = scrobblersMenu->addAction(QStringLiteral("Cancel import"));
    m_cancelBackfillAction->setVisible(false);
    connect(scrobblersMenu, &QMenu::aboutToShow, this, &PlayerBar::scrobblersMenuAboutToShow);
    historyMenu->addMenu(scrobblersMenu);

    auto *viewMenu = new QMenu(QStringLiteral("View"), this);
    m_trackInfoPaneVisible = viewMenu->addAction(QStringLiteral("Show track information pane"));
    m_trackInfoPaneVisible->setCheckable(true);
    m_trackInfoPaneVisible->setChecked(true);
    QAction *trackInfoPaneSettings = viewMenu->addAction(QStringLiteral("Track information panel…"));
    QAction *albumArtResolution = viewMenu->addAction(QStringLiteral("Album art resolution…"));
    QAction *playlistMetadataDisplay = viewMenu->addAction(QStringLiteral("Playlist metadata display…"));
    QAction *customizePanelOrder = viewMenu->addAction(QStringLiteral("Panel order…"));
    QAction *resetPanelOrder = viewMenu->addAction(QStringLiteral("Reset panel order to default"));
    viewMenu->addSeparator();
    m_listUnsupportedFiles = viewMenu->addAction(QStringLiteral("List unsupported files in explorer"));
    m_listUnsupportedFiles->setCheckable(true);
    m_listUnsupportedFiles->setVisible(false); // only relevant in file-explorer view
    QAction *resetViewPreferences = viewMenu->addAction(QStringLiteral("Reset view preferences to defaults"));

    auto *queueMenu = new QMenu(QStringLiteral("Queue"), this);
    QAction *showQueue = queueMenu->addAction(QStringLiteral("Show queue"));
    queueMenu->addSeparator();
    QAction *clearPlayNext = queueMenu->addAction(QStringLiteral("Clear play-next priority"));
    QAction *clearQueue = queueMenu->addAction(QStringLiteral("Clear queue"));
    queueMenu->addSeparator();
    QAction *saveQueue = queueMenu->addAction(QStringLiteral("Save current queue as…"));
    QAction *restoreQueue = queueMenu->addAction(QStringLiteral("Restore saved queue…"));
    QAction *mergeQueue = queueMenu->addAction(QStringLiteral("Merge saved queue (play next)…"));
    QAction *savedQueueLimit = queueMenu->addAction(QStringLiteral("Saved queue limits…"));
    m_mergeSavedQueueAction = mergeQueue;
    queueMenu->setToolTipsVisible(true);

    auto *playlistMenu = new QMenu(QStringLiteral("Playlists"), this);
    QAction *showPlaylists = playlistMenu->addAction(QStringLiteral("Show playlists"));
    playlistMenu->addSeparator();
    QAction *newPlaylist = playlistMenu->addAction(QStringLiteral("New playlist…"));
    QAction *addSong = playlistMenu->addAction(QStringLiteral("Add song…"));
    playlistMenu->addSeparator();
    QAction *playPlaylist = playlistMenu->addAction(QStringLiteral("Play playlist"));
    QAction *playNextPlaylist = playlistMenu->addAction(QStringLiteral("Play playlist next"));
    QAction *addPlaylistToQueue = playlistMenu->addAction(QStringLiteral("Add playlist to queue"));
    playlistMenu->addSeparator();
    QAction *movePlaylistItemUp = playlistMenu->addAction(QStringLiteral("Move selected item up"));
    QAction *movePlaylistItemDown = playlistMenu->addAction(QStringLiteral("Move selected item down"));
    playlistMenu->addSeparator();
    QAction *renamePlaylist = playlistMenu->addAction(QStringLiteral("Rename playlist"));
    QAction *exportPlaylist = playlistMenu->addAction(QStringLiteral("Export playlist…"));
    QAction *deletePlaylist = playlistMenu->addAction(QStringLiteral("Delete playlist"));
    m_playlistViewActions = {
        addSong,
        playPlaylist,
        playNextPlaylist,
        addPlaylistToQueue,
        movePlaylistItemUp,
        movePlaylistItemDown,
        renamePlaylist,
        exportPlaylist,
        deletePlaylist,
    };

    auto *settingsMenu = new QMenu(QStringLiteral("Settings"), this);
    QAction *searchRanking = settingsMenu->addAction(QStringLiteral("Search ranking…"));
    QAction *memoryReclaim = settingsMenu->addAction(QStringLiteral("Memory reclaim…"));
    QAction *keybindings = settingsMenu->addAction(QStringLiteral("Keybinds…"));
    m_compactMenu = settingsMenu->addAction(QStringLiteral("Use compact menu"));
    m_compactMenu->setCheckable(true);
    m_alwaysShowTray = settingsMenu->addAction(QStringLiteral("Always show system tray icon"));
    m_alwaysShowTray->setCheckable(true);

    auto *helpMenu = new QMenu(QStringLiteral("Help"), this);
    QAction *aboutApp = helpMenu->addAction(QStringLiteral("About muzaiten…"));
    connect(aboutApp, &QAction::triggered, this, &PlayerBar::aboutRequested);

    const QVector<QMenu *> styledMenus{compactMenu, fileMenu, ratingTagsMenu, scanPowerMenu, viewMenu, queueMenu, playlistMenu, playbackMenu, radioMenu, mpdMenu, historyMenu, scrobblersMenu, settingsMenu, helpMenu};
    for (QMenu *menu : styledMenus) {
        styleMenu(menu);
    }

    compactMenu->addMenu(fileMenu);
    compactMenu->addMenu(viewMenu);
    compactMenu->addMenu(queueMenu);
    compactMenu->addMenu(playlistMenu);
    compactMenu->addMenu(playbackMenu);
    compactMenu->addMenu(radioMenu);
    compactMenu->addMenu(historyMenu);
    compactMenu->addMenu(settingsMenu);
    compactMenu->addMenu(helpMenu);

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

    m_menuBar = new EagerMenuBar(m_menuStrip);
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
    m_menuBar->addMenu(viewMenu);
    m_menuBar->addMenu(queueMenu);
    m_menuBar->addMenu(playlistMenu);
    m_menuBar->addMenu(playbackMenu);
    m_menuBar->addMenu(radioMenu);
    m_menuBar->addMenu(historyMenu);
    m_menuBar->addMenu(settingsMenu);
    m_menuBar->addMenu(helpMenu);
    menuStripLayout->addWidget(m_menuButton);
    menuStripLayout->addWidget(m_menuBar, 1);
    root->addWidget(m_menuStrip);

    auto *controls = new QHBoxLayout;
    controls->setContentsMargins(8, 6, 10, 8);
    controls->setSpacing(10);

    m_albumArt = new AlbumArtView(this);
    m_albumArt->setFixedSize(56, 56);
    m_albumArt->setText(QStringLiteral("Album art"));
    m_albumArt->setVisible(false);
    m_albumArt->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_albumArt, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        if (!m_hasTrack) {
            return;
        }
        QMenu menu(this);
        QAction *findLibrary = menu.addAction(QStringLiteral("Find in library"));
        QAction *findFile = menu.addAction(QStringLiteral("Open containing directory"));
        QAction *selected = menu.exec(m_albumArt->mapToGlobal(pos));
        if (selected == findLibrary) {
            emit currentTrackLibraryRequested();
        } else if (selected == findFile) {
            emit currentTrackFileRequested();
        }
    });
    controls->addWidget(m_albumArt, 0, Qt::AlignVCenter);

    m_previous = iconButton(this, QStyle::SP_MediaSkipBackward, QStringLiteral("Previous"));
    controls->addWidget(m_previous);
    m_playPause = iconButton(this, QStyle::SP_MediaPlay, QStringLiteral("Play"));
    controls->addWidget(m_playPause);
    m_next = iconButton(this, QStyle::SP_MediaSkipForward, QStringLiteral("Next"));
    controls->addWidget(m_next);

    // Two stacked rows that share the same left/right edges: the meta row
    // (title+subtitle | stars) sits above the timeline row (elapsed | bar |
    // duration). The outer margin restores the breathing room the controls had
    // before; the time labels are content-width (no fixed box) so elapsed's left
    // edge meets the title's, duration's right edge meets the stars', and the
    // gap from each label to the bar stays a single, symmetric spacing step.
    auto *progressLayout = new QVBoxLayout;
    progressLayout->setContentsMargins(16, 0, 16, 0);
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
    m_elapsed->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    timeline->addWidget(m_elapsed);

    m_progress = new ClickSeekSlider(Qt::Horizontal, this);
    m_progress->setRange(0, 0);
    m_progress->setEnabled(false);
    timeline->addWidget(m_progress, 1);

    m_duration = new QLabel(QStringLiteral("0:00"), this);
    m_duration->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    timeline->addWidget(m_duration);

    progressLayout->addLayout(timeline);
    controls->addLayout(progressLayout, 1);

    auto *volume = new VolumeButton(this);
    volume->volumeChanged = [this](int value) {
        emit volumeChanged(value);
    };
    controls->addWidget(volume);
    m_volumeButton = volume;

    m_repeat = new QToolButton(this);
    m_repeat->setAutoRaise(true);
    m_repeat->setFixedSize(34, 34);
    m_repeat->setContextMenuPolicy(Qt::CustomContextMenu);
    updateRepeatIcon();
    controls->addWidget(m_repeat);

    m_shuffle = new QToolButton(this);
    m_shuffle->setAutoRaise(true);
    m_shuffle->setFixedSize(34, 34);
    m_shuffle->setContextMenuPolicy(Qt::CustomContextMenu);
    updateShuffleIcon();
    controls->addWidget(m_shuffle);

    m_radio = new QToolButton(this);
    m_radio->setAutoRaise(true);
    m_radio->setFixedSize(34, 34);
    m_radio->setCheckable(true);
    m_radio->setToolTip(QStringLiteral("Radio session active. Click to stop, right-click for options"));
    m_radio->setVisible(false);
    m_radio->setContextMenuPolicy(Qt::CustomContextMenu);
    updateRadioIcon();
    controls->addWidget(m_radio);

    m_radioMenu = new QMenu(this);
    m_radioAdventurousAction = m_radioMenu->addAction(QStringLiteral("Adventurous (this session)"));
    m_radioAdventurousAction->setCheckable(true);
    connect(m_radioAdventurousAction, &QAction::toggled, this, &PlayerBar::radioAdventurousChanged);
    QAction *radioExploration = m_radioMenu->addAction(QStringLiteral("Exploration…"));
    connect(radioExploration, &QAction::triggered, this, &PlayerBar::radioExplorationSettingsRequested);
    QAction *radioBatchSize = m_radioMenu->addAction(QStringLiteral("Radio batch size…"));
    connect(radioBatchSize, &QAction::triggered, this, &PlayerBar::radioBatchSizeSettingsRequested);
    connect(m_radioMenu, &QMenu::aboutToShow, this, &PlayerBar::radioMenuAboutToShow);
    root->addLayout(controls);

    connect(m_repeat, &QToolButton::clicked, this, &PlayerBar::cycleRepeatMode);
    connect(m_repeat, &QWidget::customContextMenuRequested, this, [this]() {
        // Right-click resets to the default (off); a no-op when already off.
        if (m_repeatMode != RepeatMode::Off) {
            setRepeatMode(RepeatMode::Off);
            emit repeatModeChangeRequested(RepeatMode::Off);
        }
    });
    connect(m_shuffle, &QToolButton::clicked, this, &PlayerBar::cycleShuffleMode);
    connect(m_shuffle, &QWidget::customContextMenuRequested, this, [this]() {
        if (m_shuffleMode != ShuffleMode::Off) {
            setShuffleMode(ShuffleMode::Off);
            emit shuffleModeChangeRequested(ShuffleMode::Off);
        }
    });
    connect(m_radio, &QToolButton::clicked, this, &PlayerBar::stopRadioRequested);
    connect(m_radio, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        m_radioMenu->exec(m_radio->mapToGlobal(pos));
    });

    connect(m_previous, &QToolButton::clicked, this, &PlayerBar::previousRequested);
    connect(openLibrary, &QAction::triggered, this, &PlayerBar::openLibraryRequested);
    connect(sourceDirectories, &QAction::triggered, this, &PlayerBar::sourceDirectoriesRequested);
    connect(scanEnabledSources, &QAction::triggered, this, &PlayerBar::scanEnabledSourcesRequested);
    connect(forceRescan, &QAction::triggered, this, &PlayerBar::forceRescanRequested);
    connect(removeMissingTracks, &QAction::triggered, this, &PlayerBar::removeMissingTracksRequested);
    connect(m_analyzeAudioAction, &QAction::triggered, this, &PlayerBar::audioAnalysisStartRequested);
    connect(m_semanticAnalysisEnabledAction, &QAction::toggled,
            this, &PlayerBar::semanticAnalysisEnabledChanged);
    connect(semanticProviderSetup, &QAction::triggered,
            this, &PlayerBar::semanticProviderSetupRequested);
    connect(semanticModelDownload, &QAction::triggered,
            this, &PlayerBar::semanticModelDownloadRequested);
    connect(m_cancelAudioAnalysisAction, &QAction::triggered, this, &PlayerBar::audioAnalysisCancelRequested);
    connect(analysisStatus, &QAction::triggered, this, &PlayerBar::analysisStatusRequested);
    connect(duplicateCopies, &QAction::triggered, this, &PlayerBar::duplicateCopiesRequested);
    connect(syncCurrentTrackRatingTags, &QAction::triggered, this, &PlayerBar::syncCurrentTrackRatingTagsRequested);
    connect(syncCurrentArtistRatingTags, &QAction::triggered, this, &PlayerBar::syncCurrentArtistRatingTagsRequested);
    connect(syncAllSavedRatingTags, &QAction::triggered, this, &PlayerBar::syncAllSavedRatingTagsRequested);
    connect(retryPendingRatingTags, &QAction::triggered, this, &PlayerBar::retryPendingRatingTagsRequested);
    connect(findCurrentTrack, &QAction::triggered, this, &PlayerBar::currentTrackLibraryRequested);
    connect(playbackOutput, &QAction::triggered, this, &PlayerBar::playbackProfileRequested);
    connect(m_releaseDeviceAction, &QAction::triggered, this, &PlayerBar::releaseDeviceRequested);
    connect(playbackResume, &QAction::triggered, this, &PlayerBar::playbackResumeRequested);
    connect(libraryShuffleSettings, &QAction::triggered, this, &PlayerBar::libraryShuffleSettingsRequested);
    connect(radioShuffleSettings, &QAction::triggered, this, &PlayerBar::radioShuffleSettingsRequested);
    connect(scoringWeights, &QAction::triggered, this, &PlayerBar::scoringWeightsRequested);
    connect(genreCuration, &QAction::triggered, this, &PlayerBar::genreCurationRequested);
    connect(rediscoveryMix, &QAction::triggered, this, &PlayerBar::rediscoveryMixRequested);
    connect(deepCutsMix, &QAction::triggered, this, &PlayerBar::deepCutsMixRequested);
    connect(startRadioCurrent, &QAction::triggered, this, &PlayerBar::startRadioFromCurrentRequested);
    connect(startArtistRadioCurrent, &QAction::triggered, this, &PlayerBar::startArtistRadioFromCurrentRequested);
    connect(m_stopRadioAction, &QAction::triggered, this, &PlayerBar::stopRadioRequested);
    connect(linkRoots, &QAction::triggered, this, &PlayerBar::linkRootsRequested);
    connect(mpdSource, &QAction::triggered, this, &PlayerBar::mpdSourceRequested);
    connect(mpdImport, &QAction::triggered, this, &PlayerBar::mpdImportRequested);
    connect(listeningHistory, &QAction::triggered, this, &PlayerBar::listeningHistoryRequested);
    connect(m_compactMenu, &QAction::toggled, this, &PlayerBar::compactMenuChanged);
    connect(m_alwaysShowTray, &QAction::toggled, this, &PlayerBar::alwaysShowTrayChanged);
    connect(m_listUnsupportedFiles, &QAction::toggled, this, &PlayerBar::listUnsupportedFilesChanged);
    connect(m_trackInfoPaneVisible, &QAction::toggled, this, &PlayerBar::trackInfoPaneVisibleChanged);
    connect(trackInfoPaneSettings, &QAction::triggered, this, &PlayerBar::trackInfoPaneSettingsRequested);
    connect(albumArtResolution, &QAction::triggered, this, &PlayerBar::albumArtResolutionRequested);
    connect(playlistMetadataDisplay, &QAction::triggered, this, &PlayerBar::playlistMetadataDisplayRequested);
    connect(searchRanking, &QAction::triggered, this, &PlayerBar::searchRankingRequested);
    connect(memoryReclaim, &QAction::triggered, this, &PlayerBar::memoryReclaimRequested);
    connect(keybindings, &QAction::triggered, this, &PlayerBar::keybindingsRequested);
    connect(resetPanelOrder, &QAction::triggered, this, &PlayerBar::resetPanelOrderRequested);
    connect(resetViewPreferences, &QAction::triggered, this, &PlayerBar::resetViewPreferencesRequested);
    connect(customizePanelOrder, &QAction::triggered, this, &PlayerBar::panelOrderRequested);
    connect(showQueue, &QAction::triggered, this, &PlayerBar::queueViewRequested);
    connect(clearQueue, &QAction::triggered, this, &PlayerBar::queueClearRequested);
    connect(clearPlayNext, &QAction::triggered, this, &PlayerBar::queueClearPlayNextPriorityRequested);
    connect(saveQueue, &QAction::triggered, this, &PlayerBar::queueSaveAsRequested);
    connect(restoreQueue, &QAction::triggered, this, &PlayerBar::queueRestorePreviousRequested);
    connect(mergeQueue, &QAction::triggered, this, &PlayerBar::queueMergeSavedRequested);
    connect(savedQueueLimit, &QAction::triggered, this, &PlayerBar::queueSavedLimitRequested);
    connect(showPlaylists, &QAction::triggered, this, &PlayerBar::playlistViewRequested);
    connect(newPlaylist, &QAction::triggered, this, &PlayerBar::playlistNewRequested);
    connect(addSong, &QAction::triggered, this, &PlayerBar::playlistAddSongRequested);
    connect(playPlaylist, &QAction::triggered, this, &PlayerBar::playlistPlayRequested);
    connect(playNextPlaylist, &QAction::triggered, this, &PlayerBar::playlistPlayNextRequested);
    connect(addPlaylistToQueue, &QAction::triggered, this, &PlayerBar::playlistAddToQueueRequested);
    connect(renamePlaylist, &QAction::triggered, this, &PlayerBar::playlistRenameRequested);
    connect(exportPlaylist, &QAction::triggered, this, &PlayerBar::playlistExportRequested);
    connect(deletePlaylist, &QAction::triggered, this, &PlayerBar::playlistDeleteRequested);
    connect(movePlaylistItemUp, &QAction::triggered, this, &PlayerBar::playlistMoveItemUpRequested);
    connect(movePlaylistItemDown, &QAction::triggered, this, &PlayerBar::playlistMoveItemDownRequested);
    connect(m_listenBrainzEnabled, &QAction::toggled, this, &PlayerBar::listenBrainzEnabledChanged);
    connect(listenBrainzToken, &QAction::triggered, this, &PlayerBar::listenBrainzTokenRequested);
    connect(m_clearListenBrainzBacklog, &QAction::triggered, this, &PlayerBar::listenBrainzBacklogClearRequested);
    connect(m_lastFmEnabled, &QAction::toggled, this, &PlayerBar::lastFmEnabledChanged);
    connect(lastFmSettings, &QAction::triggered, this, &PlayerBar::lastFmSettingsRequested);
    connect(m_clearLastFmBacklog, &QAction::triggered, this, &PlayerBar::lastFmBacklogClearRequested);
    connect(m_scrobbleOffline, &QAction::toggled, this, &PlayerBar::scrobbleOfflineChanged);
    connect(m_importListenBrainzAction, &QAction::triggered, this, [this]() {
        emit backfillStartRequested(QStringLiteral("listenbrainz"));
    });
    connect(m_syncLastFmAction, &QAction::triggered, this, [this]() {
        emit backfillStartRequested(QStringLiteral("lastfm"));
    });
    connect(m_cancelBackfillAction, &QAction::triggered, this, &PlayerBar::backfillCancelRequested);
    connect(m_playPause, &QToolButton::clicked, this, &PlayerBar::playPauseRequested);
    connect(m_next, &QToolButton::clicked, this, &PlayerBar::nextRequested);
    connect(m_progress, &QSlider::sliderMoved, this, [this](int value) {
        emit seekRequested(value);
    });

    setCompactMenu(false);
    setPlaylistViewActionsActive(false);
}

void PlayerBar::setReleaseDeviceVisible(bool visible)
{
    if (m_releaseDeviceAction != nullptr) {
        m_releaseDeviceAction->setVisible(visible);
        m_releaseDeviceAction->setEnabled(visible);
    }
}

void PlayerBar::setScrobbleBacklogCounts(int lastFmPending, int listenBrainzPending)
{
    if (m_clearLastFmBacklog != nullptr) {
        m_clearLastFmBacklog->setVisible(lastFmPending > 0);
        m_clearLastFmBacklog->setText(lastFmPending > 0
                                          ? QStringLiteral("Clear Last.fm backlog (%1)").arg(lastFmPending)
                                          : QStringLiteral("Clear Last.fm backlog"));
    }
    if (m_clearListenBrainzBacklog != nullptr) {
        m_clearListenBrainzBacklog->setVisible(listenBrainzPending > 0);
        m_clearListenBrainzBacklog->setText(listenBrainzPending > 0
                                                ? QStringLiteral("Clear ListenBrainz backlog (%1)").arg(listenBrainzPending)
                                                : QStringLiteral("Clear ListenBrainz backlog"));
    }
}

void PlayerBar::setBackfillStatus(bool running, const QString &statusText, bool lbResumable)
{
    if (m_backfillStatusAction != nullptr) {
        m_backfillStatusAction->setVisible(running || !statusText.isEmpty());
        m_backfillStatusAction->setText(statusText.isEmpty() ? QStringLiteral("Backfill idle") : statusText);
    }
    if (m_cancelBackfillAction != nullptr) {
        m_cancelBackfillAction->setVisible(running);
    }
    if (m_importListenBrainzAction != nullptr) {
        m_importListenBrainzAction->setEnabled(!running);
        m_importListenBrainzAction->setText(lbResumable
                                                ? QStringLiteral("Resume ListenBrainz history import")
                                                : QStringLiteral("Import ListenBrainz history"));
    }
    if (m_syncLastFmAction != nullptr) {
        m_syncLastFmAction->setEnabled(!running);
    }
}

void PlayerBar::setAudioAnalysisRunStatus(bool running, const QString &statusText)
{
    if (m_audioAnalysisRunStatusAction != nullptr) {
        m_audioAnalysisRunStatusAction->setVisible(running || !statusText.isEmpty());
        m_audioAnalysisRunStatusAction->setText(statusText.isEmpty() ? QStringLiteral("Analysis idle") : statusText);
    }
    if (m_analyzeAudioAction != nullptr) {
        m_analyzeAudioAction->setEnabled(!running);
    }
    if (m_cancelAudioAnalysisAction != nullptr) {
        m_cancelAudioAnalysisAction->setVisible(running);
        m_cancelAudioAnalysisAction->setEnabled(running);
    }
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
    if (!m_hasTrack) {
        m_trackStartGuardActive = false;
        m_lastProgressPositionMs = 0;
        m_lastProgressDurationMs = -1;
    }
}

void PlayerBar::setListenBrainzEnabled(bool enabled)
{
    if (m_listenBrainzEnabled == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_listenBrainzEnabled);
    m_listenBrainzEnabled->setChecked(enabled);
}

void PlayerBar::setScrobbleOffline(bool offline)
{
    if (m_scrobbleOffline == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_scrobbleOffline);
    m_scrobbleOffline->setChecked(offline);
}

void PlayerBar::setLastFmEnabled(bool enabled)
{
    if (m_lastFmEnabled == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_lastFmEnabled);
    m_lastFmEnabled->setChecked(enabled);
}

void PlayerBar::setTrackInfoPaneVisible(bool visible)
{
    if (m_trackInfoPaneVisible == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_trackInfoPaneVisible);
    m_trackInfoPaneVisible->setChecked(visible);
}

void PlayerBar::setListUnsupportedFiles(bool show)
{
    if (m_listUnsupportedFiles == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_listUnsupportedFiles);
    m_listUnsupportedFiles->setChecked(show);
}

void PlayerBar::setScanProfile(int profile)
{
    if (profile < 0 || profile > 2 || m_scanProfileActions[profile] == nullptr) {
        return;
    }
    // setChecked emits toggled, not triggered (which is what scanProfileChanged is
    // wired to), so this reflects the stored value without re-emitting.
    m_scanProfileActions[profile]->setChecked(true);
}

void PlayerBar::setAnalysisPower(int power)
{
    if (power < 0 || power > 2 || m_analysisPowerActions[power] == nullptr) {
        return;
    }
    m_analysisPowerActions[power]->setChecked(true);
}

void PlayerBar::setSemanticAnalysisEnabled(bool enabled)
{
    if (m_semanticAnalysisEnabledAction == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_semanticAnalysisEnabledAction);
    m_semanticAnalysisEnabledAction->setChecked(enabled);
}

void PlayerBar::setShowGuessedPlaceholders(bool show)
{
    if (m_showGuessedPlaceholders == nullptr) {
        return;
    }
    const QSignalBlocker blocker(m_showGuessedPlaceholders);
    m_showGuessedPlaceholders->setChecked(show);
}

void PlayerBar::setExplorerOptionsVisible(bool visible)
{
    if (m_listUnsupportedFiles != nullptr) {
        m_listUnsupportedFiles->setVisible(visible);
    }
}

void PlayerBar::setQueueViewLayoutActive(bool active)
{
    if (m_albumArt != nullptr) {
        m_albumArt->setVisible(active);
    }
}

void PlayerBar::setPlaylistViewActionsActive(bool active)
{
    for (QAction *action : m_playlistViewActions) {
        if (action != nullptr) {
            action->setEnabled(active);
        }
    }
}

void PlayerBar::setMergeSavedQueueEnabled(bool enabled)
{
    if (m_mergeSavedQueueAction == nullptr) {
        return;
    }
    m_mergeSavedQueueAction->setEnabled(enabled);
    m_mergeSavedQueueAction->setToolTip(enabled
        ? QString()
        : QStringLiteral("Unavailable while the queue is mirroring a playlist. "
                         "Use the saved queue's “Play next” from the Playlists screen instead."));
}

void PlayerBar::setAlbumArt(const QString &imagePath)
{
    if (m_albumArt == nullptr) {
        return;
    }
    const bool valid = !imagePath.isEmpty() && QFileInfo::exists(imagePath);
    const QString effectivePath = valid ? imagePath : AlbumArtFallback::resourcePath(palette());
    m_usingArtFallback = !valid;

    if (effectivePath.isEmpty()) {
        m_albumArt->setPixmap({});
        m_albumArt->setText(QStringLiteral("Album art"));
        return;
    }

    m_albumArt->setText({});
    m_albumArt->setSourcePath(effectivePath);
}

void PlayerBar::setAlbumArt(const QImage &image)
{
    if (image.isNull()) {
        setAlbumArt(QString());
        return;
    }
    m_usingArtFallback = false;
    if (m_albumArt != nullptr) {
        m_albumArt->setText({});
        m_albumArt->setSourceImage(image);
    }
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

void PlayerBar::setAlwaysShowTray(bool enabled)
{
    if (m_alwaysShowTray != nullptr) {
        const QSignalBlocker blocker(m_alwaysShowTray);
        m_alwaysShowTray->setChecked(enabled);
    }
}

void PlayerBar::setPlaying(bool playing)
{
    m_isPlaying = playing;
    updateTransportIcons();
}

void PlayerBar::setVolume(int volume0To100)
{
    if (m_volumeButton != nullptr) {
        static_cast<VolumeButton *>(m_volumeButton)->setValue(volume0To100);
    }
}

void PlayerBar::setVolumeControlEnabled(bool enabled)
{
    if (m_volumeButton != nullptr) {
        static_cast<VolumeButton *>(m_volumeButton)->setVolumeControlEnabled(enabled);
    }
}

void PlayerBar::setPosition(qint64 positionMs, qint64 durationMs)
{
    const qint64 safeDuration = std::max<qint64>(0, durationMs);
    const qint64 rawPosition = std::max<qint64>(0, positionMs);
    qint64 safePosition = std::clamp<qint64>(rawPosition, 0, safeDuration);

    if (safePosition == 0 && m_hasTrack && safeDuration > 0
        && (m_lastProgressPositionMs > 1000 || m_lastProgressDurationMs != safeDuration)) {
        m_trackStartGuardActive = true;
        m_trackStartGuardTimer.restart();
        if (m_progress->isSliderDown()) {
            m_progress->setSliderDown(false);
        }
    }

    if (shouldHoldTransitionPosition(rawPosition, safeDuration)) {
        safePosition = std::clamp<qint64>(m_lastProgressPositionMs, 0, safeDuration);
    }

    m_elapsed->setText(formatTime(safePosition));
    m_duration->setText(formatTime(safeDuration));
    m_progress->setEnabled(m_hasTrack && safeDuration > 0);
    m_progress->setRange(0, static_cast<int>(std::min<qint64>(safeDuration, std::numeric_limits<int>::max())));
    if (!m_progress->isSliderDown()) {
        m_progress->setValue(static_cast<int>(std::min<qint64>(safePosition, std::numeric_limits<int>::max())));
    }
    m_lastProgressPositionMs = safePosition;
    m_lastProgressDurationMs = safeDuration;
}

bool PlayerBar::shouldHoldTransitionPosition(qint64 positionMs, qint64 durationMs)
{
    if (!m_trackStartGuardActive) {
        return false;
    }
    constexpr qint64 plausibleStartPositionMs = 2000;
    constexpr qint64 maxGuardMs = 1500;
    if (durationMs <= 0 || positionMs <= plausibleStartPositionMs || m_trackStartGuardTimer.elapsed() > maxGuardMs) {
        m_trackStartGuardActive = false;
        return false;
    }
    return true;
}

void PlayerBar::scheduleThemeRefresh()
{
    if (m_themeRefreshPending) {
        return;
    }

    m_themeRefreshPending = true;
    QTimer::singleShot(0, this, [this]() {
        refreshTheme();
        m_themeRefreshPending = false;
    });
}

void PlayerBar::refreshTheme()
{
    restyleFrame(true);
    restyleMenuBar(true);
    updateTransportIcons();
    if (m_menuButton != nullptr) {
        m_menuButton->setIcon(menuHamburgerIcon(palette()));
    }
    updateShuffleIcon();
    updateRepeatIcon();
    updateRadioIcon();
    if (m_usingArtFallback) {
        setAlbumArt(QString());
    }

    repolish(this);
    for (QWidget *widget : findChildren<QWidget *>()) {
        repolish(widget);
    }
}

void PlayerBar::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (isThemeChange(event->type())) {
        scheduleThemeRefresh();
    }
}

void PlayerBar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
}

void PlayerBar::restyleFrame(bool force)
{
    if (m_restylingFrame) {
        return;
    }

    const QString style = panelBorderStyleSheet(
        QStringLiteral("QWidget#PlayerBar"), panelBottomBorder(), this, QStringLiteral(" background: palette(window);"));
    if (!force && styleSheet() == style) {
        return;
    }

    m_restylingFrame = true;
    if (force && styleSheet() == style) {
        setStyleSheet(QString());
    }
    setStyleSheet(style);
    m_restylingFrame = false;
}

void PlayerBar::restyleMenuBar(bool force)
{
    if (m_menuBar == nullptr) {
        return;
    }
    if (m_restylingMenuBar) {
        return;
    }

    const QString style = QStringLiteral(
        "QMenuBar {"
        "  margin: 0;"
        "  padding: 0;"
        "  background: palette(window);"
        "  border-top: 1px solid %1;"
        "  border-bottom: 0;"
        "}"
        "QMenuBar::item {"
        "  margin: 0;"
        "  padding: 0 10px;"
        "  border: 0;"
        "  background: transparent;"
        "}"
        "QMenuBar::item:selected {"
        "  background: palette(highlight);"
        "  color: palette(highlighted-text);"
        "}"
        "QMenuBar::item:pressed {"
        "  background: palette(highlight);"
        "  color: palette(highlighted-text);"
        "}"
        "QToolButton {"
        "  margin: 0;"
        "  padding: 0;"
        "  border: 0;"
        "  background: transparent;"
        "}"
        "QToolButton:hover {"
        "  background: palette(highlight);"
        "  color: palette(highlighted-text);"
        "}"
        "QToolButton:pressed {"
        "  background: palette(highlight);"
        "}").arg(panelSeparatorColorCss(m_menuBar));
    if (!force && m_menuBar->styleSheet() == style) {
        return;
    }

    m_restylingMenuBar = true;
    if (force && m_menuBar->styleSheet() == style) {
        m_menuBar->setStyleSheet(QString());
    }
    m_menuBar->setStyleSheet(style);
    m_restylingMenuBar = false;
}

void PlayerBar::updateTransportIcons()
{
    if (m_previous != nullptr) {
        m_previous->setIcon(m_previous->style()->standardIcon(QStyle::SP_MediaSkipBackward, nullptr, m_previous));
    }
    if (m_playPause != nullptr) {
        m_playPause->setIcon(m_playPause->style()->standardIcon(
            m_isPlaying ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay, nullptr, m_playPause));
        m_playPause->setToolTip(m_isPlaying ? QStringLiteral("Pause") : QStringLiteral("Play"));
    }
    if (m_next != nullptr) {
        m_next->setIcon(m_next->style()->standardIcon(QStyle::SP_MediaSkipForward, nullptr, m_next));
    }
    if (m_volumeButton != nullptr) {
        m_volumeButton->setIcon(m_volumeButton->style()->standardIcon(QStyle::SP_MediaVolume, nullptr, m_volumeButton));
    }
}

void PlayerBar::updateShuffleIcon()
{
    if (m_shuffle == nullptr) {
        return;
    }
    m_shuffle->setIcon(shuffleIcon(palette(), m_shuffleMode));
    switch (m_shuffleMode) {
    case ShuffleMode::Off:
        m_shuffle->setToolTip(QStringLiteral("Shuffle: off"));
        break;
    case ShuffleMode::Queue:
        m_shuffle->setToolTip(QStringLiteral("Shuffle: queue"));
        break;
    case ShuffleMode::Library:
        m_shuffle->setToolTip(QStringLiteral("Shuffle: library-wide"));
        break;
    case ShuffleMode::Radio:
        m_shuffle->setToolTip(QStringLiteral("Shuffle: radio"));
        break;
    }
}

void PlayerBar::updateRepeatIcon()
{
    if (m_repeat == nullptr) {
        return;
    }
    m_repeat->setIcon(repeatIcon(palette(), m_repeatMode));
    switch (m_repeatMode) {
    case RepeatMode::Off:
        m_repeat->setToolTip(QStringLiteral("Repeat: off"));
        break;
    case RepeatMode::All:
        m_repeat->setToolTip(QStringLiteral("Repeat: queue"));
        break;
    case RepeatMode::One:
        m_repeat->setToolTip(QStringLiteral("Repeat: one"));
        break;
    }
}

void PlayerBar::updateRadioIcon()
{
    if (m_radio == nullptr) {
        return;
    }
    m_radio->setIcon(radioIcon(palette()));
}

void PlayerBar::setRadioActive(bool active)
{
    m_radioActive = active;
    if (m_radio == nullptr) {
        return;
    }
    m_radio->setVisible(active);
    m_radio->setChecked(active);
}

void PlayerBar::setRadioAdventurous(bool on)
{
    // Both the indicator-button menu and the menu-bar Radio menu carry this
    // action; each menu refreshes through here on aboutToShow, which also
    // keeps the two mirrors consistent after either one is toggled.
    if (m_radioAdventurousAction != nullptr) {
        const QSignalBlocker blocker(m_radioAdventurousAction);
        m_radioAdventurousAction->setChecked(on);
    }
    if (m_radioBarAdventurousAction != nullptr) {
        const QSignalBlocker blocker(m_radioBarAdventurousAction);
        m_radioBarAdventurousAction->setChecked(on);
    }
}

void PlayerBar::setRepeatMode(RepeatMode mode)
{
    m_repeatMode = mode;
    updateRepeatIcon();
}

void PlayerBar::setShuffleMode(ShuffleMode mode)
{
    m_shuffleMode = mode;
    updateShuffleIcon();
}

void PlayerBar::cycleRepeatMode()
{
    const RepeatMode next = m_repeatMode == RepeatMode::Off ? RepeatMode::All
        : m_repeatMode == RepeatMode::All                  ? RepeatMode::One
                                                           : RepeatMode::Off;
    setRepeatMode(next);
    emit repeatModeChangeRequested(next);
}

void PlayerBar::cycleShuffleMode()
{
    const ShuffleMode next = m_shuffleMode == ShuffleMode::Off ? ShuffleMode::Queue
        : m_shuffleMode == ShuffleMode::Queue                 ? ShuffleMode::Library
        : m_shuffleMode == ShuffleMode::Library               ? ShuffleMode::Radio
                                                              : ShuffleMode::Off;
    setShuffleMode(next);
    emit shuffleModeChangeRequested(next);
}
