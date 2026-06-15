#include "playback/AudioDeviceControl.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QStandardPaths>

namespace AudioDeviceControl {

namespace {

constexpr int kProcessTimeoutMs = 3000;

// pw-dump stores numeric props as JSON strings ("3"); accept either form.
int propToInt(const QJsonValue &value, int fallback = -1)
{
    if (value.isDouble())
        return static_cast<int>(value.toDouble());
    if (value.isString()) {
        bool ok = false;
        const int n = value.toString().toInt(&ok);
        return ok ? n : fallback;
    }
    return fallback;
}

// Run a short-lived helper and capture stdout. Returns false (with *error set)
// on missing binary, timeout, or non-zero exit.
bool run(const QString &program, const QStringList &args, QByteArray *out, QString *error)
{
    if (QStandardPaths::findExecutable(program).isEmpty()) {
        if (error != nullptr)
            *error = QStringLiteral("%1 is not installed").arg(program);
        return false;
    }

    QProcess proc;
    proc.start(program, args);
    if (!proc.waitForFinished(kProcessTimeoutMs)) {
        proc.kill();
        proc.waitForFinished(500);
        if (error != nullptr)
            *error = QStringLiteral("%1 timed out").arg(program);
        return false;
    }
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        if (error != nullptr) {
            const QString stderrText = QString::fromLocal8Bit(proc.readAllStandardError()).trimmed();
            *error = stderrText.isEmpty()
                ? QStringLiteral("%1 failed").arg(program)
                : stderrText;
        }
        return false;
    }
    if (out != nullptr)
        *out = proc.readAllStandardOutput();
    return true;
}

// Parse one pw-dump Device object into a DeviceState. Returns nullopt for
// non-ALSA devices or entries without a hw: path.
std::optional<DeviceState> parseDevice(const QJsonObject &obj)
{
    if (obj.value(QStringLiteral("type")).toString() != QStringLiteral("PipeWire:Interface:Device"))
        return std::nullopt;

    const QJsonObject info  = obj.value(QStringLiteral("info")).toObject();
    const QJsonObject props = info.value(QStringLiteral("props")).toObject();
    if (props.value(QStringLiteral("device.api")).toString() != QStringLiteral("alsa"))
        return std::nullopt;

    const QString hwPath = props.value(QStringLiteral("api.alsa.path")).toString();
    if (hwPath.isEmpty())
        return std::nullopt;

    DeviceState dev;
    dev.pwId        = propToInt(obj.value(QStringLiteral("id")));
    dev.cardIndex   = propToInt(props.value(QStringLiteral("api.alsa.card")));
    dev.hwPath      = hwPath;
    dev.stableId    = props.value(QStringLiteral("device.name")).toString();
    dev.description = props.value(QStringLiteral("device.description")).toString();
    if (dev.description.isEmpty())
        dev.description = props.value(QStringLiteral("device.nick")).toString();
    if (dev.description.isEmpty())
        dev.description = hwPath;

    const QJsonObject params = info.value(QStringLiteral("params")).toObject();

    // The active profile is reported as a single-element Profile array.
    const QJsonArray profile = params.value(QStringLiteral("Profile")).toArray();
    if (!profile.isEmpty()) {
        const QJsonObject cur = profile.first().toObject();
        dev.currentProfileIndex = propToInt(cur.value(QStringLiteral("index")));
        dev.currentProfileName  = cur.value(QStringLiteral("name")).toString();
    }

    // EnumProfile lists every selectable profile; pick out "off" and the
    // highest-priority available audio profile for restore.
    int bestPriority = -1;
    for (const QJsonValue &value : params.value(QStringLiteral("EnumProfile")).toArray()) {
        const QJsonObject p = value.toObject();
        const int index    = propToInt(p.value(QStringLiteral("index")));
        const QString name = p.value(QStringLiteral("name")).toString();
        if (name == QStringLiteral("off")) {
            dev.offProfileIndex = index;
            continue;
        }
        // "available" is the string "yes"/"no"/"unknown"; treat anything but a
        // hard "no" as a restore candidate.
        if (p.value(QStringLiteral("available")).toString() == QStringLiteral("no"))
            continue;
        const int priority = propToInt(p.value(QStringLiteral("priority")), 0);
        if (priority > bestPriority) {
            bestPriority = priority;
            dev.bestAudioProfileIndex = index;
        }
    }

    return dev;
}

bool setProfile(const DeviceState &dev, int profileIndex, QString *error)
{
    if (dev.pwId < 0 || profileIndex < 0) {
        if (error != nullptr)
            *error = QStringLiteral("Device or profile is unknown");
        return false;
    }
    return run(QStringLiteral("wpctl"),
               {QStringLiteral("set-profile"),
                QString::number(dev.pwId),
                QString::number(profileIndex)},
               nullptr, error);
}

} // namespace

bool toolingAvailable()
{
    return !QStandardPaths::findExecutable(QStringLiteral("pw-dump")).isEmpty()
        && !QStandardPaths::findExecutable(QStringLiteral("wpctl")).isEmpty();
}

QVector<DeviceState> enumerate()
{
    QVector<DeviceState> devices;
    QByteArray out;
    if (!run(QStringLiteral("pw-dump"), {}, &out, nullptr))
        return devices;

    const QJsonDocument doc = QJsonDocument::fromJson(out);
    if (!doc.isArray())
        return devices;

    for (const QJsonValue &value : doc.array()) {
        if (auto dev = parseDevice(value.toObject()))
            devices.append(*dev);
    }
    return devices;
}

std::optional<DeviceState> findByHwPath(const QString &hwPath)
{
    if (hwPath.isEmpty())
        return std::nullopt;
    for (const DeviceState &dev : enumerate()) {
        if (dev.hwPath == hwPath)
            return dev;
    }
    return std::nullopt;
}

std::optional<DeviceState> findByStableId(const QString &stableId)
{
    if (stableId.isEmpty())
        return std::nullopt;
    for (const DeviceState &dev : enumerate()) {
        if (dev.stableId == stableId)
            return dev;
    }
    return std::nullopt;
}

bool takeOver(const DeviceState &dev, QString *error)
{
    const int off = dev.offProfileIndex >= 0 ? dev.offProfileIndex : 0;
    return setProfile(dev, off, error);
}

bool release(const DeviceState &dev, int restoreProfileIndex, QString *error)
{
    int target = restoreProfileIndex;
    if (target < 0)
        target = dev.bestAudioProfileIndex;
    if (target < 0) {
        if (error != nullptr)
            *error = QStringLiteral("No audio profile to restore for %1").arg(dev.description);
        return false;
    }
    return setProfile(dev, target, error);
}

} // namespace AudioDeviceControl
