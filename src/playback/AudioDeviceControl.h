#pragma once

#include <QString>
#include <QVector>

#include <optional>

// Manual "takeover" of a sound card from the system audio server so a
// bit-perfect / direct ALSA stream can open it exclusively.
//
// Background: when PipeWire holds an ALSA card, opening the same `hw:N` device
// directly for bit-perfect output fails with an "internal data stream error" —
// the kernel device is busy. The fix mirrors the long-standing `wptoggle`
// script: switch the card's PipeWire *profile* to "Off", which unbinds it from
// the graph and frees the device. PipeWire/WirePlumber remembers the card and
// re-routes automatically once a normal profile is restored, so we only need to
// implement the takeover (Off) and release (restore) halves here.
//
// PipeWire-only for now (via `pw-dump` + `wpctl`); PulseAudio / ALSA-direct
// behaviour can be added behind the same interface later.
namespace AudioDeviceControl {

struct DeviceState {
    int pwId = -1;                  // PipeWire global object id (for `wpctl set-profile`)
    int cardIndex = -1;             // ALSA card number (the N in hw:N)
    QString hwPath;                 // "hw:N" — volatile, drifts across reboots
    QString stableId;              // PipeWire `device.name`, stable across reboots
    QString description;            // human label, e.g. "FiiO K5 Pro"
    int currentProfileIndex = -1;
    QString currentProfileName;     // "off" once released
    int offProfileIndex = -1;       // index of the "Off" profile (usually 0)
    int bestAudioProfileIndex = -1; // highest-priority available non-off profile

    // True while PipeWire still owns the card — a direct hw: stream will be
    // refused until it is taken over. Unknown ("off" profile not found) reads as
    // not-held so we never nag about a card we can't actually free.
    bool heldByPipeWire() const
    {
        return offProfileIndex >= 0 && currentProfileIndex != offProfileIndex;
    }
};

// Whether the PipeWire tooling (pw-dump + wpctl) is present on this system.
bool toolingAvailable();

// Snapshot of every ALSA card PipeWire currently knows about.
QVector<DeviceState> enumerate();

// Look up the card backing a "hw:N" device string (as stored in a bit-perfect
// PlaybackProfile). Returns nullopt if no PipeWire card maps to it.
std::optional<DeviceState> findByHwPath(const QString &hwPath);

// Look up a card by its stable id (PipeWire `device.name`). Used to recover the
// current hw:N for a remembered device after the index has drifted.
std::optional<DeviceState> findByStableId(const QString &stableId);

// Release `dev` from PipeWire (switch to the Off profile) so a direct ALSA
// stream can open it. `error` is filled on failure.
bool takeOver(const DeviceState &dev, QString *error = nullptr);

// Hand `dev` back to PipeWire. Restores `restoreProfileIndex` when >= 0 (e.g.
// the profile that was active before takeover); otherwise the card's best
// available audio profile.
bool release(const DeviceState &dev, int restoreProfileIndex = -1, QString *error = nullptr);

// Return the PipeWire node.name for the card's Audio/Sink, or an empty string
// until that node exists. The name is also the PipeWire Pulse-server sink name,
// so a shared GStreamer sink can target this physical card explicitly instead
// of racing the current default sink after release().
QString sinkNodeName(const QString &hwPath);

// Whether PipeWire has (re)created an Audio/Sink node for the card behind
// "hw:N". After release() the profile flips instantly but PipeWire needs a
// couple of seconds to rebuild the sink node and re-link any loopbacks — so
// this is the real "the card is usable again" signal to wait on before routing
// shared playback back to it. False if the card/tooling is absent.
bool sinkNodeReady(const QString &hwPath);

} // namespace AudioDeviceControl
