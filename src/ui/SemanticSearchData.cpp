#include "ui/SemanticSearchData.h"

#include "core/Rating.h"

#include <algorithm>
#include <cmath>

namespace SemanticSearchData {
namespace {

double cosine(const QVector<float> &left, const QVector<float> &right)
{
    double dot = 0.0;
    double leftNorm = 0.0;
    double rightNorm = 0.0;
    for (qsizetype i = 0; i < left.size(); ++i) {
        const double a = left.at(i);
        const double b = right.at(i);
        if (!std::isfinite(a) || !std::isfinite(b)) {
            return std::numeric_limits<double>::quiet_NaN();
        }
        dot += a * b;
        leftNorm += a * a;
        rightNorm += b * b;
    }
    if (!(leftNorm > 0.0) || !(rightNorm > 0.0)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return dot / (std::sqrt(leftNorm) * std::sqrt(rightNorm));
}

} // namespace

QVector<GroupScore> rankEmbeddings(const QVector<float> &queryVector,
                                   const QHash<qint64, QVector<float>> &embeddings,
                                   int limit)
{
    QVector<GroupScore> scores;
    if (queryVector.isEmpty() || limit <= 0) {
        return scores;
    }
    scores.reserve(embeddings.size());
    for (auto it = embeddings.constBegin(); it != embeddings.constEnd(); ++it) {
        if (it.value().size() != queryVector.size()) {
            continue;
        }
        const double score = cosine(queryVector, it.value());
        if (std::isfinite(score)) {
            scores.push_back({it.key(), score});
        }
    }
    std::sort(scores.begin(), scores.end(), [](const GroupScore &left, const GroupScore &right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        return left.groupId < right.groupId;
    });
    if (scores.size() > limit) {
        scores.resize(limit);
    }
    return scores;
}

QString formatQuality(const Track &track)
{
    QStringList parts;
    if (!track.codec.isEmpty()) {
        parts << track.codec.toUpper();
    }
    if (track.bitDepth > 0 && track.sampleRateHz > 0) {
        parts << QStringLiteral("%1bit/%2kHz")
                     .arg(track.bitDepth)
                     .arg(track.sampleRateHz / 1000.0, 0, 'g', 4);
    } else if (track.bitrateKbps > 0) {
        parts << QStringLiteral("%1kbps").arg(track.bitrateKbps);
    }
    return parts.join(QLatin1Char(' '));
}

QString starText(int rating0To100)
{
    if (rating0To100 == Rating::unset || rating0To100 < 0) {
        return {};
    }
    const int stars = std::clamp((rating0To100 + 10) / 20, 0, 5);
    QString text;
    for (int i = 0; i < 5; ++i) {
        text += i < stars ? QChar(0x2605) : QChar(0x2606);
    }
    return text;
}

QString yearText(const QString &date)
{
    if (date.size() < 4) {
        return {};
    }
    const QString year = date.left(4);
    for (const QChar c : year) {
        if (!c.isDigit()) {
            return {};
        }
    }
    return year;
}

} // namespace SemanticSearchData
