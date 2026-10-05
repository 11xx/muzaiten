#include "playback/PlaybackProfileJson.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

PlaybackProfile playbackProfileFromJson(const QString &json)
{
    PlaybackProfile profile;
    if (json.isEmpty()) {
        return profile;
    }

    const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
    profile.id = root.value(QStringLiteral("id")).toString(profile.id);
    profile.name = root.value(QStringLiteral("name")).toString(profile.name);
    profile.backend = root.value(QStringLiteral("backend")).toString(profile.backend);
    const QString mode = root.value(QStringLiteral("mode")).toString(profile.mode);
    profile.mode = mode == QStringLiteral("exclusive") ? QStringLiteral("bit-perfect") : mode;
    profile.sink = root.value(QStringLiteral("sink")).toString(profile.sink);
    profile.device = root.value(QStringLiteral("device")).toString(profile.device);
    profile.softwareVolume = root.value(QStringLiteral("softwareVolume")).toBool(profile.softwareVolume);
    profile.replayGain = root.value(QStringLiteral("replayGain")).toBool(profile.replayGain);
    profile.allowResample = root.value(QStringLiteral("allowResample")).toBool(profile.allowResample);
    profile.releaseSinkOnPause = root.value(QStringLiteral("releaseSinkOnPause")).toBool(profile.releaseSinkOnPause);
    if (root.contains(QStringLiteral("readAheadMb"))) {
        profile.readAheadMb = std::clamp(
            root.value(QStringLiteral("readAheadMb")).toInt(profile.readAheadMb), 0, 1024);
    } else {
        // Migrate the legacy "preload into RAM" percentage: any non-zero value
        // meant "whole file in RAM", which maps to read-ahead enabled.
        const int legacyPreload = root.value(QStringLiteral("preloadPercent")).toInt(0);
        profile.readAheadMb = legacyPreload > 0 ? 32 : 0;
    }

    // Cross-mode memory. Fall back to the active fields so profiles saved before
    // these were introduced still seed sensible per-mode defaults.
    const bool sharedActive = profile.mode != QStringLiteral("bit-perfect");
    profile.sharedSink = root.value(QStringLiteral("sharedSink"))
        .toString(sharedActive ? profile.sink : QString());
    profile.sharedSoftwareVolume = root.value(QStringLiteral("sharedSoftwareVolume"))
        .toBool(sharedActive ? profile.softwareVolume : true);
    profile.sharedAllowResample = root.value(QStringLiteral("sharedAllowResample"))
        .toBool(sharedActive ? profile.allowResample : false);
    profile.sharedReleaseSinkOnPause = root.value(QStringLiteral("sharedReleaseSinkOnPause"))
        .toBool(sharedActive ? profile.releaseSinkOnPause : true);
    profile.deviceId = root.value(QStringLiteral("deviceId")).toString(profile.deviceId);
    profile.deviceLabel = root.value(QStringLiteral("deviceLabel")).toString();
    profile.sharedEndpoint = root.value(QStringLiteral("sharedEndpoint")).toString();
    profile.sharedEndpointLabel = root.value(QStringLiteral("sharedEndpointLabel")).toString();
    profile.sharedEndpointFallback = root.value(QStringLiteral("sharedEndpointFallback")).toBool(false);
    profile.autoReleaseExclusiveDevice = root.value(QStringLiteral("autoReleaseExclusiveDevice"))
        .toBool(profile.autoReleaseExclusiveDevice);
    profile.autoReleaseTimeoutSec = std::clamp(
        root.value(QStringLiteral("autoReleaseTimeoutSec")).toInt(profile.autoReleaseTimeoutSec), 1, 600);
    return profile;
}

QString playbackProfileToJson(const PlaybackProfile &profile)
{
    QJsonObject root;
    root.insert(QStringLiteral("id"), profile.id);
    root.insert(QStringLiteral("name"), profile.name);
    root.insert(QStringLiteral("backend"), profile.backend);
    root.insert(QStringLiteral("mode"), profile.mode);
    root.insert(QStringLiteral("sink"), profile.sink);
    root.insert(QStringLiteral("device"), profile.device);
    root.insert(QStringLiteral("softwareVolume"), profile.softwareVolume);
    root.insert(QStringLiteral("replayGain"), profile.replayGain);
    root.insert(QStringLiteral("allowResample"), profile.allowResample);
    root.insert(QStringLiteral("releaseSinkOnPause"), profile.releaseSinkOnPause);
    root.insert(QStringLiteral("readAheadMb"), profile.readAheadMb);
    root.insert(QStringLiteral("sharedSink"), profile.sharedSink);
    root.insert(QStringLiteral("sharedSoftwareVolume"), profile.sharedSoftwareVolume);
    root.insert(QStringLiteral("sharedAllowResample"), profile.sharedAllowResample);
    root.insert(QStringLiteral("sharedReleaseSinkOnPause"), profile.sharedReleaseSinkOnPause);
    root.insert(QStringLiteral("deviceId"), profile.deviceId);
    root.insert(QStringLiteral("deviceLabel"), profile.deviceLabel);
    root.insert(QStringLiteral("sharedEndpoint"), profile.sharedEndpoint);
    root.insert(QStringLiteral("sharedEndpointLabel"), profile.sharedEndpointLabel);
    root.insert(QStringLiteral("sharedEndpointFallback"), profile.sharedEndpointFallback);
    root.insert(QStringLiteral("autoReleaseExclusiveDevice"), profile.autoReleaseExclusiveDevice);
    root.insert(QStringLiteral("autoReleaseTimeoutSec"), profile.autoReleaseTimeoutSec);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}
