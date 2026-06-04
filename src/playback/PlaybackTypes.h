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
    // Megabytes of the playing file to keep warm in the OS page cache ahead of
    // the playhead, via posix_fadvise(POSIX_FADV_WILLNEED).  0 = off (the
    // kernel's own read-ahead still applies).  Smooths reads from slow or
    // network mounts without duplicating the file into app memory; the page
    // cache is reclaimable RAM, and warmed pages also serve soft-pause resume.
    int readAheadMb = 0;
};
