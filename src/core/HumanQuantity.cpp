#include "core/HumanQuantity.h"

#include <QChar>
#include <QStringList>

namespace humanquantity {

QString formatDuration(qint64 ms)
{
    if (ms <= 0) {
        return {};
    }
    const qint64 totalSeconds = ms / 1000;
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QLatin1Char('0'));
}

qint64 parseDuration(const QString &text)
{
    qint64 total = 0;
    const QStringList parts = text.split(QLatin1Char(':'));
    for (const QString &part : parts) {
        total = total * 60 + part.trimmed().toLongLong();
    }
    return total * 1000;
}

QString formatSize(qint64 bytes)
{
    if (bytes <= 0) {
        return {};
    }
    double value = static_cast<double>(bytes);
    const QStringList units = {QStringLiteral("B"), QStringLiteral("KB"), QStringLiteral("MB"), QStringLiteral("GB")};
    int unit = 0;
    while (value >= 1024.0 && unit < units.size() - 1) {
        value /= 1024.0;
        ++unit;
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', unit == 0 ? 0 : 1).arg(units.at(unit));
}

} // namespace humanquantity
