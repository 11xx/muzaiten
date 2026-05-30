#pragma once

#include <QString>

struct PlaybackProfile {
    QString id = QStringLiteral("shared-default");
    QString name = QStringLiteral("Shared output");
    QString mode = QStringLiteral("shared");
    QString backend = QStringLiteral("gstreamer");
    QString sink = QStringLiteral("auto");
    QString device;
    bool softwareVolume = true;
    bool replayGain = false;
    bool allowResample = false;
    // Release the audio sink immediately on pause so the output device is
    // freed for other apps.  Always true in bit-perfect mode (exclusive ALSA
    // access would block every other app while merely paused); configurable
    // in shared mode (default on).
    bool releaseSinkOnPause = true;
};
