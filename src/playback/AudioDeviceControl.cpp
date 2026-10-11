#include "playback/AudioDeviceControl.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <utility>

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

// pw-dump without waiting for it: the answer arrives through |done| once the
// process exits, fails or times out, unless |context| is destroyed first. The
// process belongs to no caller: destroying a running QProcess waits for it to
// exit, so a caller that goes away only has it killed, and it is released
// once it has exited.
void dumpWithoutBlocking(QObject *context, std::function<void(std::optional<QByteArray>)> done)
{
    const QString program = QStringLiteral("pw-dump");
    if (QStandardPaths::findExecutable(program).isEmpty()) {
        done(std::nullopt);
        return;
    }
    auto *proc = new QProcess;
    auto *timeout = new QTimer(proc);
    timeout->setSingleShot(true);
    const QPointer<QObject> caller(context);
    auto answer = std::make_shared<std::function<void(std::optional<QByteArray>)>>(std::move(done));
    const auto finish = [answer, caller](std::optional<QByteArray> out) {
        const auto callback = std::exchange(*answer, {});
        if (callback && caller)
            callback(std::move(out));
    };
    QObject::connect(proc, &QProcess::finished, proc, [proc, finish](int exitCode, QProcess::ExitStatus status) {
        finish(status == QProcess::NormalExit && exitCode == 0 ? std::optional(proc->readAllStandardOutput())
                                                                : std::nullopt);
        proc->deleteLater();
    });
    QObject::connect(proc, &QProcess::errorOccurred, proc, [proc, finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            finish(std::nullopt);
            proc->deleteLater();
        }
    });
    QObject::connect(timeout, &QTimer::timeout, proc, [proc, finish]() {
        proc->kill();
        finish(std::nullopt);
    });
    if (context != nullptr) {
        QObject::connect(context, &QObject::destroyed, proc, [proc, answer]() {
            *answer = {};
            proc->kill();
        });
    }
    proc->start(program, {});
    timeout->start(kProcessTimeoutMs);
}

Tooling systemTooling()
{
    return {
        [] {
            return !QStandardPaths::findExecutable(QStringLiteral("pw-dump")).isEmpty()
                && !QStandardPaths::findExecutable(QStringLiteral("wpctl")).isEmpty();
        },
        []() -> std::optional<QByteArray> {
            QByteArray out;
            if (!run(QStringLiteral("pw-dump"), {}, &out, nullptr))
                return std::nullopt;
            return out;
        },
        [](int pwId, int profileIndex, QString *error) {
            return run(QStringLiteral("wpctl"),
                       {QStringLiteral("set-profile"), QString::number(pwId), QString::number(profileIndex)},
                       nullptr, error);
        },
        dumpWithoutBlocking,
    };
}

Tooling &tooling()
{
    static Tooling current = systemTooling();
    return current;
}

Snapshot parseSnapshot(const std::optional<QByteArray> &out)
{
    if (!out)
        return {};
    const QJsonDocument doc = QJsonDocument::fromJson(*out);
    return {doc.isArray() ? doc.array() : QJsonArray()};
}

bool setProfile(const DeviceState &dev, int profileIndex, QString *error)
{
    if (dev.pwId < 0 || profileIndex < 0) {
        if (error != nullptr)
            *error = QStringLiteral("Device or profile is unknown");
        return false;
    }
    return tooling().setProfile(dev.pwId, profileIndex, error);
}

struct SinkNode {
    int id = -1;
    QString name;
};

std::optional<SinkNode> findSinkNode(const Snapshot &graph, const QString &hwPath)
{
    if (hwPath.isEmpty())
        return std::nullopt;
    const QJsonArray &arr = graph.objects;

    int deviceId = -1;
    for (const QJsonValue &value : arr) {
        if (auto dev = parseDevice(value.toObject()); dev && dev->hwPath == hwPath) {
            deviceId = dev->pwId;
            break;
        }
    }
    if (deviceId < 0)
        return std::nullopt;

    for (const QJsonValue &value : arr) {
        const QJsonObject obj = value.toObject();
        if (obj.value(QStringLiteral("type")).toString() != QStringLiteral("PipeWire:Interface:Node"))
            continue;
        const QJsonObject props = obj.value(QStringLiteral("info")).toObject()
                                      .value(QStringLiteral("props")).toObject();
        if (props.value(QStringLiteral("media.class")).toString() == QStringLiteral("Audio/Sink")
            && propToInt(props.value(QStringLiteral("device.id")), -1) == deviceId) {
            return SinkNode{propToInt(obj.value(QStringLiteral("id"))),
                            props.value(QStringLiteral("node.name")).toString()};
        }
    }
    return std::nullopt;
}

} // namespace

Tooling setTooling(Tooling replacement)
{
    return std::exchange(tooling(), std::move(replacement));
}

bool toolingAvailable()
{
    return tooling().available();
}

Snapshot snapshot()
{
    return parseSnapshot(tooling().dump());
}

void readSnapshot(QObject *context, std::function<void(const Snapshot &)> done)
{
    const Tooling &current = tooling();
    if (!current.dumpAsync) {
        done(snapshot());
        return;
    }
    const QPointer<QObject> guard(context);
    current.dumpAsync(context, [guard, done = std::move(done)](std::optional<QByteArray> out) {
        if (guard) {
            done(parseSnapshot(out));
        }
    });
}

QVector<DeviceState> enumerate(const Snapshot &graph)
{
    QVector<DeviceState> devices;
    for (const QJsonValue &value : graph.objects) {
        if (auto dev = parseDevice(value.toObject()))
            devices.append(*dev);
    }
    return devices;
}

QVector<DeviceState> enumerate()
{
    return enumerate(snapshot());
}

std::optional<DeviceState> findByHwPath(const QString &hwPath)
{
    if (hwPath.isEmpty())
        return std::nullopt;
    return findSelected(snapshot(), QString(), hwPath);
}

std::optional<DeviceState> findByStableId(const QString &stableId)
{
    if (stableId.isEmpty())
        return std::nullopt;
    return findSelected(snapshot(), stableId, QString());
}

std::optional<DeviceState> findSelected(const QString &stableId, const QString &hwPath)
{
    return stableId.isEmpty() ? findByHwPath(hwPath) : findByStableId(stableId);
}

std::optional<DeviceState> findSelected(const Snapshot &graph, const QString &stableId, const QString &hwPath)
{
    if (stableId.isEmpty() && hwPath.isEmpty())
        return std::nullopt;
    for (const DeviceState &dev : enumerate(graph)) {
        if (stableId.isEmpty() ? dev.hwPath == hwPath : dev.stableId == stableId)
            return dev;
    }
    return std::nullopt;
}

DirectTarget resolveDirect(const QString &stableId, const QString &savedDevice)
{
    DirectTarget target;
    if (stableId.isEmpty()) {
        target.status = savedDevice.isEmpty() ? DirectTarget::Status::Unavailable : DirectTarget::Status::Manual;
        target.hwPath = savedDevice;
        target.description = savedDevice;
        return target;
    }
    if (const auto dev = findByStableId(stableId); dev && !dev->hwPath.isEmpty()) {
        target.status = DirectTarget::Status::Resolved;
        target.hwPath = dev->hwPath;
        target.description = dev->description;
    }
    return target;
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

QString sinkNodeName(const QString &hwPath)
{
    if (hwPath.isEmpty())
        return {};
    const auto sink = findSinkNode(snapshot(), hwPath);
    return sink ? sink->name : QString();
}

int sinkNodeId(const QString &hwPath)
{
    return hwPath.isEmpty() ? -1 : sinkNodeId(snapshot(), hwPath);
}

int sinkNodeId(const Snapshot &graph, const QString &hwPath)
{
    const auto sink = findSinkNode(graph, hwPath);
    return sink ? sink->id : -1;
}

bool sinkNodeReady(const QString &hwPath)
{
    return !hwPath.isEmpty() && sinkNodeReady(snapshot(), hwPath);
}

bool sinkNodeReady(const Snapshot &graph, const QString &hwPath)
{
    const auto sink = findSinkNode(graph, hwPath);
    return sink && !sink->name.isEmpty();
}

} // namespace AudioDeviceControl
