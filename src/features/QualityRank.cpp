#include "features/QualityRank.h"

#include "core/Track.h"

#include <algorithm>

namespace {

int codecBase(const QString &codec)
{
    const QString c = codec.toLower();
    if (isDsdCodec(c)) {
        return 120000;
    }
    if (c == QStringLiteral("flac") || c == QStringLiteral("alac") || c == QStringLiteral("wav")
        || c == QStringLiteral("aiff") || c == QStringLiteral("aif") || c == QStringLiteral("ape")
        || c == QStringLiteral("wv")) {
        return 60000;
    }
    if (c == QStringLiteral("opus") || c == QStringLiteral("aac") || c == QStringLiteral("m4a")
        || c == QStringLiteral("ogg")) {
        return 30000;
    }
    if (c == QStringLiteral("mp3")) {
        return 20000;
    }
    return 10000;
}

} // namespace

namespace QualityRank {

bool isVinylTagged(const QStringList &mediaTags)
{
    for (const QString &tag : mediaTags) {
        if (tag.contains(QStringLiteral("vinyl"), Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

int score(const Copy &copy)
{
    int value = codecBase(copy.codec);
    value += std::max(0, copy.bitDepth) * 1000;
    value += std::max(0, copy.sampleRateHz) / 100;
    value += std::max(0, copy.bitrateKbps);
    if (isDsdCodec(copy.codec)) {
        value += 50000;
    }
    if (isVinylTagged(copy.mediaTags)) {
        value -= 50000;
    }
    return value;
}

QString bestPath(const QVector<Copy> &copies, const QString &pinnedPath)
{
    if (!pinnedPath.isEmpty()) {
        for (const Copy &copy : copies) {
            if (copy.path == pinnedPath) {
                return pinnedPath;
            }
        }
    }

    QString best;
    int bestScore = 0;
    bool haveBest = false;
    for (const Copy &copy : copies) {
        if (copy.path.isEmpty()) {
            continue;
        }
        const int copyScore = score(copy);
        if (!haveBest || copyScore > bestScore || (copyScore == bestScore && copy.path < best)) {
            haveBest = true;
            bestScore = copyScore;
            best = copy.path;
        }
    }
    return best;
}

} // namespace QualityRank
