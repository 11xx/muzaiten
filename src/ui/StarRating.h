#pragma once

#include <QPoint>
#include <QRect>

class QPainter;
class QPalette;

namespace StarRating {
constexpr int unset = -1;

QRect ratingRect(const QRect &cellRect, int starSize, int starCount = 5);
int ratingFromPosition(const QRect &ratingRect, QPoint pos, int starCount = 5);
void paint(QPainter *painter, const QRect &rect, int rating0To100, int hoverRating0To100, const QPalette &palette, int starSize);
}
