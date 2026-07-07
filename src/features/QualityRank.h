#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace QualityRank {

struct Copy {
    QString path;
    QString codec;
    int bitDepth = 0;
    int sampleRateHz = 0;
    int bitrateKbps = 0;
    QStringList mediaTags;
};

bool isVinylTagged(const QStringList &mediaTags);
int score(const Copy &copy);
QString bestPath(const QVector<Copy> &copies, const QString &pinnedPath = {});

} // namespace QualityRank
