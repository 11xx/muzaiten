#pragma once

#include <QString>

struct PlaybackProfile {
    QString id = QStringLiteral("shared-default");
    QString name = QStringLiteral("Shared output");
    QString mode = QStringLiteral("shared");
    QString backend = QStringLiteral("qt");
    QString sink = QStringLiteral("auto");
    QString device;
    bool softwareVolume = true;
    bool replayGain = false;
    bool allowResample = true;
};
