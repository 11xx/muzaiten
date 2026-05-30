#include "ui/NeighborColumnResizer.h"

#include <QHeaderView>
#include <QMouseEvent>

#include <algorithm>

namespace {
constexpr int handleGrip = 6; // px tolerance around a section boundary
}

NeighborColumnResizer *NeighborColumnResizer::install(QHeaderView *header,
                                                      std::function<int(int)> minWidthFor)
{
    return new NeighborColumnResizer(header, std::move(minWidthFor));
}

NeighborColumnResizer::NeighborColumnResizer(QHeaderView *header, std::function<int(int)> minWidthFor)
    : QObject(header)
    , m_header(header)
    , m_minWidthFor(std::move(minWidthFor))
{
    // Interactive resize mouse events are delivered to the header's viewport, so
    // filter both to reliably detect when a drag begins on a handle.
    m_header->installEventFilter(this);
    if (m_header->viewport() != nullptr) {
        m_header->viewport()->installEventFilter(this);
    }
    connect(m_header, &QHeaderView::sectionResized, this, &NeighborColumnResizer::onSectionResized);
}

int NeighborColumnResizer::minWidthFor(int logicalIndex) const
{
    const int provided = m_minWidthFor ? m_minWidthFor(logicalIndex) : m_header->minimumSectionSize();
    return std::max(provided, m_header->minimumSectionSize());
}

int NeighborColumnResizer::nextVisibleToRight(int logicalIndex) const
{
    const int visual = m_header->visualIndex(logicalIndex);
    for (int v = visual + 1; v < m_header->count(); ++v) {
        const int logical = m_header->logicalIndex(v);
        if (!m_header->isSectionHidden(logical)) {
            return logical;
        }
    }
    return -1;
}

bool NeighborColumnResizer::isOnHandle(int x) const
{
    for (int v = 0; v < m_header->count(); ++v) {
        const int logical = m_header->logicalIndex(v);
        if (m_header->isSectionHidden(logical)) {
            continue;
        }
        const int edge = m_header->sectionViewportPosition(logical) + m_header->sectionSize(logical);
        if (std::abs(x - edge) <= handleGrip) {
            return true;
        }
    }
    return false;
}

bool NeighborColumnResizer::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_header || watched == m_header->viewport()) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                // Only treat the drag as a resize when it begins on a handle, so
                // programmatic size changes (no mouse) never trigger a trade.
                m_userResizing = isOnHandle(mouse->position().toPoint().x());
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            m_userResizing = false;
        }
    }
    return QObject::eventFilter(watched, event);
}

void NeighborColumnResizer::onSectionResized(int logicalIndex, int oldSize, int newSize)
{
    if (m_adjusting || !m_userResizing) {
        return;
    }

    const int rightLogical = nextVisibleToRight(logicalIndex);

    m_adjusting = true;
    if (rightLogical < 0) {
        // Last visible column: its right edge is the table edge, so there is no
        // neighbour to trade with. Keep the total constant by reverting.
        m_header->resizeSection(logicalIndex, oldSize);
        m_adjusting = false;
        return;
    }

    const int pairTotal = oldSize + m_header->sectionSize(rightLogical);
    const int resizedMin = minWidthFor(logicalIndex);
    const int rightMin = minWidthFor(rightLogical);
    const int clamped = std::clamp(newSize, resizedMin, pairTotal - rightMin);
    if (clamped != newSize) {
        m_header->resizeSection(logicalIndex, clamped);
    }
    m_header->resizeSection(rightLogical, pairTotal - clamped);
    m_adjusting = false;

    emit columnResized();
}
