#include "reco/ReasonText.h"

#include <QStringList>

#include <algorithm>
#include <cmath>

namespace {

// Below this magnitude, the scorer contribution reads as noise in a tooltip.
constexpr double kExplainThreshold = 0.15;
constexpr int kMaxPositivePhrases = 3;
constexpr int kMaxNegativePhrases = 2;

QString phraseFor(const QString &name)
{
    if (name == QStringLiteral("genre")) {
        return QStringLiteral("matches the session's mood");
    }
    if (name == QStringLiteral("era")) {
        return QStringLiteral("from a similar era");
    }
    if (name == QStringLiteral("tempo")) {
        return QStringLiteral("matches the pace");
    }
    if (name == QStringLiteral("energy")) {
        return QStringLiteral("similar energy");
    }
    if (name == QStringLiteral("audio")) {
        return QStringLiteral("sounds similar");
    }
    if (name == QStringLiteral("rating")) {
        return QStringLiteral("you rate it highly");
    }
    if (name == QStringLiteral("history")) {
        return QStringLiteral("a longtime favorite");
    }
    if (name == QStringLiteral("novelty")) {
        return QStringLiteral("a fresh discovery");
    }
    if (name == QStringLiteral("recency")) {
        return QStringLiteral("heard recently");
    }
    if (name == QStringLiteral("skips")) {
        return QStringLiteral("often skipped early");
    }
    if (name == QStringLiteral("same-artist")) {
        return QStringLiteral("artist just played");
    }
    return name;
}

QList<TrackScorer::Component> sortedByMagnitude(QList<TrackScorer::Component> components)
{
    std::sort(components.begin(), components.end(), [](const auto &left, const auto &right) {
        return std::abs(left.value) > std::abs(right.value);
    });
    return components;
}

} // namespace

namespace ReasonText {

QString sentence(const QList<TrackScorer::Component> &components)
{
    QStringList positiveParts;
    QStringList negativeParts;

    for (const TrackScorer::Component &component : sortedByMagnitude(components)) {
        if (std::abs(component.value) < kExplainThreshold) {
            continue;
        }
        if (component.value > 0.0) {
            if (positiveParts.size() < kMaxPositivePhrases) {
                positiveParts.push_back(phraseFor(component.name));
            }
        } else if (negativeParts.size() < kMaxNegativePhrases) {
            negativeParts.push_back(phraseFor(component.name));
        }
    }

    if (positiveParts.isEmpty() && negativeParts.isEmpty()) {
        return {};
    }
    if (positiveParts.isEmpty()) {
        return QStringLiteral("Radio pick (held back): ") + negativeParts.join(QStringLiteral(" · "));
    }

    QString result = QStringLiteral("Radio pick: ") + positiveParts.join(QStringLiteral(" · "));
    if (!negativeParts.isEmpty()) {
        result += QStringLiteral(" (held back: ") + negativeParts.join(QStringLiteral(" · "))
            + QStringLiteral(")");
    }
    return result;
}

QString breakdown(const QList<TrackScorer::Component> &components)
{
    QStringList parts;
    const QList<TrackScorer::Component> sorted = sortedByMagnitude(components);
    parts.reserve(sorted.size());
    for (const TrackScorer::Component &component : sorted) {
        const double rounded = std::round(component.value * 10.0) / 10.0;
        parts.push_back(QStringLiteral("%1 %2%3")
                            .arg(component.name,
                                 rounded >= 0.0 ? QStringLiteral("+") : QStringLiteral("-"))
                            .arg(std::abs(rounded), 0, 'f', 1));
    }
    return parts.join(QStringLiteral(" · "));
}

} // namespace ReasonText
