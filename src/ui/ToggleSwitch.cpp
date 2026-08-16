#include "ui/ToggleSwitch.h"

#include <QPainter>
#include <QVariantAnimation>

namespace {

constexpr int kTrackWidth = 33;
constexpr int kTrackHeight = 18;
constexpr qreal kKnobInset = 2.5;

}   // namespace

ToggleSwitch::ToggleSwitch(QWidget *parent)
    : QAbstractButton(parent)
    , m_slide(new QVariantAnimation(this))
{
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    m_slide->setDuration(110);
    connect(m_slide, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_position = value.toReal();
        update();
    });
    connect(this, &QAbstractButton::toggled, this, [this](bool on) {
        m_slide->stop();
        // State set before the switch is on screen is where it starts, not
        // something it slides into: animating that would show every switch
        // turning itself on as the window opens.
        if (!isVisible()) {
            m_position = on ? 1.0 : 0.0;
            update();
            return;
        }
        m_slide->setStartValue(m_position);
        m_slide->setEndValue(on ? 1.0 : 0.0);
        m_slide->start();
    });
}

QSize ToggleSwitch::sizeHint() const
{
    return {kTrackWidth + 5, kTrackHeight + 2};
}

void ToggleSwitch::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF track(1.5, (height() - kTrackHeight) / 2.0, kTrackWidth, kTrackHeight);
    const qreal radius = track.height() / 2;
    const QPalette::ColorGroup group = isEnabled() ? QPalette::Active : QPalette::Disabled;
    const QColor off = palette().color(group, QPalette::Button);
    const QColor on = palette().color(group, QPalette::Highlight);
    const auto blend = [this](qreal from, qreal to) { return static_cast<float>(from + (to - from) * m_position); };

    QColor outline = palette().color(group, QPalette::WindowText);
    outline.setAlphaF(static_cast<float>(0.45 - 0.35 * m_position));
    painter.setPen(QPen(outline, 1));
    painter.setBrush(QColor::fromRgbF(blend(off.redF(), on.redF()), blend(off.greenF(), on.greenF()),
                                      blend(off.blueF(), on.blueF())));
    painter.drawRoundedRect(track, radius, radius);

    if (hasFocus()) {
        painter.setPen(QPen(palette().color(group, QPalette::Highlight), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(track.adjusted(-1.5, -1.5, 1.5, 1.5), radius + 1, radius + 1);
    }

    const qreal travel = track.width() - track.height();
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette().color(group, QPalette::BrightText));
    painter.drawEllipse(QPointF(track.left() + radius + travel * m_position, track.center().y()),
                        radius - kKnobInset, radius - kKnobInset);
}
