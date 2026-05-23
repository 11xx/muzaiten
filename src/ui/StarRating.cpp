#include "ui/StarRating.h"

#include "core/Rating.h"

#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <algorithm>
#include <cmath>

namespace {

QPainterPath starPath(const QRectF &rect)
{
    const QPointF center = rect.center();
    const double outerRadius = std::min(rect.width(), rect.height()) / 2.0;
    const double innerRadius = outerRadius * 0.48;
    QPainterPath path;

    for (int i = 0; i < 10; ++i) {
        const double angle = (-90.0 + i * 36.0) * M_PI / 180.0;
        const double radius = (i % 2 == 0) ? outerRadius : innerRadius;
        const QPointF point(center.x() + std::cos(angle) * radius, center.y() + std::sin(angle) * radius);
        if (i == 0) {
            path.moveTo(point);
        } else {
            path.lineTo(point);
        }
    }
    path.closeSubpath();
    return path;
}

} // namespace

namespace StarRating {

QRect ratingRect(const QRect &cellRect, int starSize, int starCount)
{
    const int totalWidth = starSize * starCount;
    return {cellRect.left() + 6, cellRect.top() + (cellRect.height() - starSize) / 2, totalWidth, starSize};
}

int ratingFromPosition(const QRect &rect, QPoint pos, int starCount)
{
    if (!rect.contains(pos) || rect.width() <= 0 || starCount <= 0) {
        return unset;
    }

    const int halfSteps = starCount * 2;
    const double stepWidth = static_cast<double>(rect.width()) / static_cast<double>(halfSteps);
    const int step = std::clamp(static_cast<int>((pos.x() - rect.left()) / stepWidth), 0, halfSteps - 1);
    return (step + 1) * 10;
}

void paint(QPainter *painter, const QRect &rect, int rating0To100, int hoverRating0To100, const QPalette &palette, int starSize)
{
    const int activeRating = hoverRating0To100 >= 0 ? hoverRating0To100 : rating0To100;
    const int normalized = activeRating >= 0 ? Rating::normalized0To100(activeRating) : unset;
    const QColor fillColor = palette.color(QPalette::Highlight);
    const QColor emptyColor = palette.color(QPalette::Disabled, QPalette::Text);
    const QPen outlinePen(emptyColor, 1.0);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const int starCount = 5;
    for (int i = 0; i < starCount; ++i) {
        const QRectF starRect(rect.left() + i * starSize + 2, rect.top() + 2, starSize - 4, starSize - 4);
        const QPainterPath path = starPath(starRect);

        painter->setPen(outlinePen);
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(path);

        if (normalized <= i * 20) {
            continue;
        }

        const bool half = normalized < (i + 1) * 20;
        painter->save();
        painter->setClipRect(half ? QRectF(starRect.left(), starRect.top(), starRect.width() / 2.0, starRect.height()) : starRect);
        painter->setPen(Qt::NoPen);
        painter->setBrush(fillColor);
        painter->drawPath(path);
        painter->restore();

        painter->setPen(QPen(fillColor, 1.0));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(path);
    }

    painter->restore();
}

} // namespace StarRating
