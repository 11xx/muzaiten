#include "ui/OverlayScrollBar.h"

#include <QAbstractScrollArea>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>

OverlayScrollBar::OverlayScrollBar(QAbstractScrollArea *area, QWidget *parent)
    : QWidget(parent != nullptr ? parent : area)
    , m_area(area)
    , m_scrollBar(area->verticalScrollBar())
{
    // The widget is purely visual; all mouse interaction is handled via the
    // viewport event filter so the widget never intercepts viewport events.
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);

    // Hide the native vertical scrollbar so the viewport uses the full width.
    area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // Receive mouse and resize events from the viewport.
    area->viewport()->setMouseTracking(true);
    area->viewport()->installEventFilter(this);

    reposition();
    raise();

    connect(m_scrollBar, &QScrollBar::valueChanged, this, QOverload<>::of(&QWidget::update));
    connect(m_scrollBar, &QScrollBar::rangeChanged, this, [this](int, int) { update(); });
}

void OverlayScrollBar::install(QAbstractScrollArea *area)
{
    // Parented to the scroll area (not its viewport): a viewport child would be
    // blitted along with the content on every scroll, making the handle flicker.
    new OverlayScrollBar(area);
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

void OverlayScrollBar::reposition()
{
    // Position over the viewport's right edge, in the scroll area's coordinates.
    const QRect vr = m_area->viewport()->geometry();
    setGeometry(vr.x() + vr.width() - ghostZone, vr.y(), ghostZone, vr.height());
    raise();
}

int OverlayScrollBar::viewportHeight() const
{
    return m_area->viewport()->height();
}

bool OverlayScrollBar::isScrollable() const
{
    return m_scrollBar->minimum() < m_scrollBar->maximum();
}

// Returns the handle rect in widget coordinates.
QRect OverlayScrollBar::handleRect() const
{
    const int vh = viewportHeight();
    const int minimum = m_scrollBar->minimum();
    const int maximum = m_scrollBar->maximum();
    const int total = maximum - minimum;
    const int pageStep = m_scrollBar->pageStep();

    if (total <= 0 || pageStep <= 0) {
        return QRect(ghostZone - handleWidth, 0, handleWidth, vh);
    }

    const int fullRange = total + pageStep;
    const int hh = std::max(handleMinLen, vh * pageStep / fullRange);
    const int track = vh - hh;
    const int hy = track <= 0 ? 0 : track * (m_scrollBar->value() - minimum) / total;
    return QRect(ghostZone - handleWidth, hy, handleWidth, hh);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void OverlayScrollBar::paintEvent(QPaintEvent *)
{
    // Keep geometry synced to the live viewport before drawing, so painted
    // bounds never lag behind a resize that arrived out of order.
    reposition();

    // Nothing to show when the content fits the viewport, even while hovered.
    if (!isScrollable()) {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRect hr = handleRect();

    if (m_hovered || m_dragging) {
        // Bright rounded handle while the ghost zone is active
        QColor color = palette().color(QPalette::Highlight);
        color.setAlpha(m_dragging ? 220 : 170);
        painter.setBrush(color);
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(hr, 4, 4);
    } else {
        // Faint hairline at the handle position (idle indicator)
        QColor color = palette().color(QPalette::Mid);
        color.setAlpha(70);
        painter.fillRect(QRect(ghostZone - 2, hr.top(), 2, hr.height()), color);
    }
}

void OverlayScrollBar::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        update();
    }
    QWidget::changeEvent(event);
}

// ---------------------------------------------------------------------------
// Hover state
// ---------------------------------------------------------------------------

void OverlayScrollBar::setHovered(bool hovered)
{
    if (m_hovered == hovered) {
        return;
    }
    m_hovered = hovered;
    update();
}

// ---------------------------------------------------------------------------
// Viewport event filter
// ---------------------------------------------------------------------------

bool OverlayScrollBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_area->viewport()) {
        return QWidget::eventFilter(watched, event);
    }

    switch (event->type()) {
    case QEvent::Resize:
        reposition();
        break;

    case QEvent::MouseButtonPress: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (!m_dragging && m_hovered && isScrollable() && mouse->button() == Qt::LeftButton) {
            // Convert viewport-local cursor pos to overlay-local coords. The
            // overlay's left edge sits at (viewportWidth - ghostZone) and its
            // top aligns with the viewport top.
            const QPoint local(mouse->pos().x() - (m_area->viewport()->width() - ghostZone),
                               mouse->pos().y());
            if (handleRect().contains(local)) {
                m_dragging = true;
                m_dragStartPos = mouse->pos().y();
                m_dragStartValue = m_scrollBar->value();
                m_dragTrack = viewportHeight() - handleRect().height();
                update();
                return true; // eat: don't pass click to content beneath the handle
            }
        }
        break;
    }

    case QEvent::MouseButtonRelease: {
        if (m_dragging) {
            m_dragging = false;
            update();
            return true;
        }
        break;
    }

    case QEvent::MouseMove: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (m_dragging) {
            if (!(mouse->buttons() & Qt::LeftButton)) {
                // Button released somewhere we never saw the release; stop.
                m_dragging = false;
                update();
                break;
            }
            const int delta = mouse->pos().y() - m_dragStartPos;
            const int total = m_scrollBar->maximum() - m_scrollBar->minimum();
            if (m_dragTrack > 0 && total > 0) {
                m_scrollBar->setValue(std::clamp(
                    m_dragStartValue + delta * total / m_dragTrack,
                    m_scrollBar->minimum(),
                    m_scrollBar->maximum()));
            }
            update();
            return true; // eat during drag so content doesn't get spurious hovers
        }
        const bool inGhostZone = isScrollable()
            && mouse->pos().x() >= m_area->viewport()->width() - ghostZone;
        if (inGhostZone != m_hovered) {
            setHovered(inGhostZone);
        }
        break;
    }

    case QEvent::Leave:
        // Don't abort an in-progress drag: like a real scrollbar, dragging
        // continues (via the press's implicit mouse grab) until the button is
        // released, even if the cursor strays off the viewport. Only the idle
        // hover indicator is cleared here.
        setHovered(false);
        break;

    default:
        break;
    }

    return QWidget::eventFilter(watched, event);
}
