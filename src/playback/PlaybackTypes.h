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

    // When muzaiten takes a card away from PipeWire (bit-perfect, or a DSD
    // takeover), optionally hand it back automatically after playback goes idle
    // (pause / end of queue) for this many seconds. Off by default: a takeover
    // is deliberate, so the device stays ours until released explicitly (the
    // Playback → Release device action) unless the user opts into auto-release.
    bool autoReleaseExclusiveDevice = false;
    int autoReleaseTimeoutSec = 15;

    // --- Cross-mode memory ---------------------------------------------------
    // The active fields above describe whichever mode is currently selected; in
    // bit-perfect mode `sink` is pinned to "alsa" and the shared toggles are
    // forced, so the user's shared-mode choices would otherwise be lost on the
    // round trip. These shadow fields preserve each mode's selection across mode
    // switches and restarts. Not consumed by the backend — purely UI memory.
    QString sharedSink;                  // remembered shared sink; empty -> "auto"
    bool sharedSoftwareVolume = true;
    bool sharedAllowResample = false;
    bool sharedReleaseSinkOnPause = true;
    // Stable identity of the bit-perfect device (PipeWire `device.name`, e.g.
    // "alsa_card.usb-..."), used to re-resolve the volatile `device` (hw:N) on
    // load — the index drifts across reboots / device launch order.
    QString deviceId;

    // Return a copy forced to shared mode with every shared active field
    // restored from the cross-mode shadow memory above. Use this whenever an
    // exclusively-held card is handed back outside the profile dialog (the
    // Release device action / idle auto-release): flipping only `mode` would
    // strand bit-perfect's pinned sink ("alsa") and hw `device` behind
    // mode="shared", a "ghost" that still opens the card directly instead of
    // routing through the shared graph. Mirrors the shared branch of
    // PlaybackProfileDialog::profile().
    PlaybackProfile toSharedMode() const
    {
        PlaybackProfile p = *this;
        p.mode = QStringLiteral("shared");
        p.sink = sharedSink.isEmpty() ? QStringLiteral("auto") : sharedSink;
        p.device.clear();
        p.softwareVolume = sharedSoftwareVolume;
        p.allowResample = sharedAllowResample;
        p.releaseSinkOnPause = sharedReleaseSinkOnPause;
        return p;
    }
};
