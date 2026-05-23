#include "core/Rating.h"

#include <algorithm>

int Rating::normalized0To100(int value)
{
    if (value < 0) {
        return unset;
    }

    const int clamped = std::clamp(value, 0, 100);
    return ((clamped + 5) / 10) * 10;
}

QString Rating::displayText(int value0To100)
{
    const int normalized = normalized0To100(value0To100);
    if (normalized < 0) {
        return {};
    }

    const int halfSteps = normalized / 10;
    const int wholeStars = halfSteps / 2;
    const bool hasHalf = (halfSteps % 2) == 1;

    QString text;
    text.reserve(6);
    for (int i = 0; i < wholeStars; ++i) {
        text += QChar(0x2605);
    }
    if (hasHalf) {
        text += QStringLiteral("1/2");
    }
    return text;
}

bool Rating::isValidStoredValue(int value)
{
    return value == unset || (value >= 0 && value <= 100 && value % 10 == 0);
}

