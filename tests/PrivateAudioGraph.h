#pragma once

// A private PipeWire graph for routing tests: its own pipewire, pipewire-pulse
// and policy-only WirePlumber, with sockets, configuration and state in a
// temporary directory. It holds only the null sinks a test creates, never a
// hardware device. The calling process's environment is pointed at it, so
// GStreamer's pulsesink and every pw/pactl helper reach this graph alone.

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <csignal>
#include <memory>
#include <vector>

#include <signal.h>

class PrivateAudioGraph {
public:
    PrivateAudioGraph() = default;
    PrivateAudioGraph(const PrivateAudioGraph &) = delete;
    PrivateAudioGraph &operator=(const PrivateAudioGraph &) = delete;

    ~PrivateAudioGraph()
    {
        for (auto it = m_processes.rbegin(); it != m_processes.rend(); ++it) {
            QProcess *process = it->get();
            process->terminate();
            if (!process->waitForFinished(3000)) {
                process->kill();
                process->waitForFinished(3000);
            }
        }
    }

    // Start the graph. Returns false with *reason set when the tooling is
    // missing (the caller skips) or when isolation cannot be confirmed.
    bool start(QString *reason)
    {
        for (const char *tool : {"pipewire", "pipewire-pulse", "wireplumber", "pactl", "parec",
                                 "pw-dump", "pw-metadata"}) {
            if (QStandardPaths::findExecutable(QString::fromLatin1(tool)).isEmpty()) {
                *reason = QStringLiteral("%1 is not installed").arg(QString::fromLatin1(tool));
                return false;
            }
        }
        if (!m_root.isValid()) {
            *reason = QStringLiteral("no temporary directory");
            return false;
        }
        const QString config = m_root.filePath(QStringLiteral("config"));
        QDir().mkpath(config);
        for (const QString &name : {QStringLiteral("pipewire.conf"), QStringLiteral("pipewire-pulse.conf"),
                                    QStringLiteral("client.conf")}) {
            if (!copyConfig(QStringLiteral("pipewire/") + name, config + QLatin1Char('/') + name)) {
                *reason = QStringLiteral("PipeWire configuration %1 not found").arg(name);
                return false;
            }
        }
        if (!copyConfig(QStringLiteral("wireplumber/wireplumber.conf"), config + QStringLiteral("/wireplumber.conf"))) {
            *reason = QStringLiteral("WirePlumber configuration not found");
            return false;
        }

        const QString runtime = m_root.path();
        for (const char *name : {"PIPEWIRE_REMOTE", "PIPEWIRE_RUNTIME_DIR", "PULSE_SERVER", "PULSE_RUNTIME_PATH",
                                 "PULSE_COOKIE", "PULSE_CLIENTCONFIG"}) {
            qunsetenv(name);
        }
        qputenv("XDG_RUNTIME_DIR", runtime.toUtf8());
        qputenv("PIPEWIRE_RUNTIME_DIR", runtime.toUtf8());
        qputenv("PIPEWIRE_REMOTE", "pipewire-0");
        qputenv("PIPEWIRE_CONFIG_DIR", config.toUtf8());
        qputenv("WIREPLUMBER_CONFIG_DIR", config.toUtf8());
        qputenv("XDG_CONFIG_HOME", config.toUtf8());
        qputenv("XDG_STATE_HOME", (runtime + QStringLiteral("/state")).toUtf8());
        qputenv("XDG_CACHE_HOME", (runtime + QStringLiteral("/cache")).toUtf8());
        qputenv("PULSE_RUNTIME_PATH", (runtime + QStringLiteral("/pulse")).toUtf8());
        qputenv("PULSE_SERVER", (QStringLiteral("unix:") + runtime + QStringLiteral("/pulse/native")).toUtf8());
        qputenv("DBUS_SESSION_BUS_ADDRESS", (QStringLiteral("unix:path=") + runtime + QStringLiteral("/no-bus")).toUtf8());

        launch(QStringLiteral("pipewire"), {QStringLiteral("-c"), QStringLiteral("pipewire.conf")});
        if (!waitFor([&] { return QFile::exists(runtime + QStringLiteral("/pipewire-0")); })) {
            *reason = QStringLiteral("private pipewire did not start");
            return false;
        }
        launch(QStringLiteral("wireplumber"), {QStringLiteral("-p"), QStringLiteral("policy"),
                                               QStringLiteral("-c"), QStringLiteral("wireplumber.conf")});
        launch(QStringLiteral("pipewire-pulse"), {QStringLiteral("-c"), QStringLiteral("pipewire-pulse.conf")});
        if (!waitFor([&] { return QFile::exists(runtime + QStringLiteral("/pulse/native")); })) {
            *reason = QStringLiteral("private pipewire-pulse did not start");
            return false;
        }

        // Isolation must hold before anything is created or moved.
        const QString info = run({QStringLiteral("pactl"), QStringLiteral("info")});
        if (!info.contains(runtime)) {
            *reason = QStringLiteral("pactl does not reach the private server");
            return false;
        }
        if (hasHardwareDevice()) {
            *reason = QStringLiteral("the private graph unexpectedly holds a hardware device");
            return false;
        }
        return true;
    }

    QString run(const QStringList &command, bool *ok = nullptr) const
    {
        QProcess process;
        process.start(command.first(), command.mid(1));
        const bool finished = process.waitForFinished(8000);
        if (ok != nullptr) {
            *ok = finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
        }
        return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    }

    // Create a silent null sink named |name|; returns its module id.
    QString addSink(const QString &name, const QString &description = QString())
    {
        QStringList command{QStringLiteral("pactl"), QStringLiteral("load-module"),
                            QStringLiteral("module-null-sink"), QStringLiteral("sink_name=") + name};
        if (!description.isEmpty()) {
            command << QStringLiteral("sink_properties=device.description=\"%1\"").arg(description);
        }
        const QString module = run(command);
        waitFor([&] { return sinkNames().contains(name); });
        return module;
    }

    void removeModule(const QString &module)
    {
        run({QStringLiteral("pactl"), QStringLiteral("unload-module"), module});
    }

    void setDefaultSink(const QString &name)
    {
        run({QStringLiteral("pactl"), QStringLiteral("set-default-sink"), name});
    }

    QStringList sinkNames() const
    {
        QStringList names;
        for (const QJsonValue &sink : listJson(QStringLiteral("sinks"))) {
            names << sink.toObject().value(QStringLiteral("name")).toString();
        }
        return names;
    }

    // Playback streams as {index, effective sink name} pairs. pipewire-pulse
    // derives the sink from the stream's actual links.
    QVector<QPair<int, QString>> streams() const
    {
        QHash<int, QString> sinkByIndex;
        for (const QJsonValue &sink : listJson(QStringLiteral("sinks"))) {
            const QJsonObject object = sink.toObject();
            sinkByIndex.insert(object.value(QStringLiteral("index")).toInt(),
                               object.value(QStringLiteral("name")).toString());
        }
        QVector<QPair<int, QString>> result;
        for (const QJsonValue &input : listJson(QStringLiteral("sink-inputs"))) {
            const QJsonObject object = input.toObject();
            result.append({object.value(QStringLiteral("index")).toInt(),
                           sinkByIndex.value(object.value(QStringLiteral("sink")).toInt())});
        }
        return result;
    }

    // Effective sink of the only playback stream, or empty when there is none.
    QString route() const
    {
        const auto all = streams();
        return all.size() == 1 ? all.first().second : QString();
    }

    int streamIndex() const
    {
        const auto all = streams();
        return all.size() == 1 ? all.first().first : -1;
    }

    // The target the only playback stream requested when it was created (its
    // node's target.object property), as distinct from where it is linked.
    QString requestedTarget() const
    {
        const QJsonArray objects = QJsonDocument::fromJson(run({QStringLiteral("pw-dump")}).toUtf8()).array();
        for (const QJsonValue &value : objects) {
            const QJsonObject props = value.toObject().value(QStringLiteral("info")).toObject()
                                          .value(QStringLiteral("props")).toObject();
            if (props.value(QStringLiteral("media.class")).toString() == QStringLiteral("Stream/Output/Audio")) {
                return props.value(QStringLiteral("target.object")).toString();
            }
        }
        return {};
    }

    // Move the only playback stream the way Wiremix does: clear the stream's
    // target.object metadata, then write the sink's node id as target.node.
    void mixerMove(const QString &sink)
    {
        const QJsonArray objects = QJsonDocument::fromJson(run({QStringLiteral("pw-dump")}).toUtf8()).array();
        int streamId = -1;
        int sinkId = -1;
        for (const QJsonValue &value : objects) {
            const QJsonObject object = value.toObject();
            const QJsonObject props = object.value(QStringLiteral("info")).toObject()
                                          .value(QStringLiteral("props")).toObject();
            if (props.value(QStringLiteral("media.class")).toString() == QStringLiteral("Stream/Output/Audio")) {
                streamId = object.value(QStringLiteral("id")).toInt();
            } else if (props.value(QStringLiteral("node.name")).toString() == sink
                       && props.value(QStringLiteral("media.class")).toString() == QStringLiteral("Audio/Sink")) {
                sinkId = object.value(QStringLiteral("id")).toInt();
            }
        }
        run({QStringLiteral("pw-metadata"), QStringLiteral("-n"), QStringLiteral("default"), QStringLiteral("-d"),
             QString::number(streamId), QStringLiteral("target.object")});
        run({QStringLiteral("pw-metadata"), QStringLiteral("-n"), QStringLiteral("default"),
             QString::number(streamId), QStringLiteral("target.node"), QString::number(sinkId),
             QStringLiteral("Spa:Id")});
    }

    // Move the only playback stream through the PulseAudio protocol.
    void pulseMove(const QString &sink)
    {
        run({QStringLiteral("pactl"), QStringLiteral("move-sink-input"), QString::number(streamIndex()), sink});
    }

    // Whether audible (non-zero) samples reach |sink| within |ms|, read from
    // its monitor.
    bool carriesAudio(const QString &sink, int ms = 800) const
    {
        QProcess capture;
        // A short latency makes parec deliver within the window; SIGINT lets
        // it flush what it captured.
        capture.start(QStringLiteral("parec"), {QStringLiteral("--raw"), QStringLiteral("--format=s16le"),
                                                QStringLiteral("--channels=1"), QStringLiteral("--rate=8000"),
                                                QStringLiteral("--latency-msec=50"),
                                                QStringLiteral("--device=") + sink + QStringLiteral(".monitor")});
        if (!capture.waitForStarted(3000)) {
            return false;
        }
        QThread::msleep(static_cast<unsigned long>(ms));
        ::kill(static_cast<pid_t>(capture.processId()), SIGINT);
        if (!capture.waitForFinished(3000)) {
            capture.kill();
            capture.waitForFinished(3000);
        }
        const QByteArray data = capture.readAllStandardOutput();
        int loud = 0;
        for (qsizetype i = 0; i + 1 < data.size(); i += 2) {
            const auto sample = static_cast<qint16>(static_cast<quint8>(data[i])
                                                    | (static_cast<quint8>(data[i + 1]) << 8));
            if (sample > 1000 || sample < -1000) {
                ++loud;
            }
        }
        return loud > 100;
    }

    template<typename Predicate>
    static bool waitFor(Predicate predicate, int ms = 8000)
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < ms) {
            if (predicate()) {
                return true;
            }
            QThread::msleep(50);
        }
        return predicate();
    }

private:
    static bool copyConfig(const QString &relative, const QString &destination)
    {
        for (const QString &base : {QStringLiteral("/usr/share/"), QStringLiteral("/usr/local/share/"),
                                    QStringLiteral("/etc/")}) {
            if (QFile::exists(base + relative)) {
                return QFile::copy(base + relative, destination);
            }
        }
        return false;
    }

    void launch(const QString &program, const QStringList &arguments)
    {
        auto process = std::make_unique<QProcess>();
        process->setStandardOutputFile(m_root.filePath(program + QStringLiteral(".log")));
        process->setStandardErrorFile(m_root.filePath(program + QStringLiteral(".log")), QIODevice::Append);
        process->start(program, arguments);
        process->waitForStarted(3000);
        m_processes.push_back(std::move(process));
    }

    QJsonArray listJson(const QString &kind) const
    {
        return QJsonDocument::fromJson(
                   run({QStringLiteral("pactl"), QStringLiteral("-f"), QStringLiteral("json"),
                        QStringLiteral("list"), kind}).toUtf8())
            .array();
    }

    bool hasHardwareDevice() const
    {
        const QJsonArray objects = QJsonDocument::fromJson(run({QStringLiteral("pw-dump")}).toUtf8()).array();
        if (objects.isEmpty()) {
            return true;  // unreadable: do not assume isolation
        }
        for (const QJsonValue &value : objects) {
            const QString api = value.toObject().value(QStringLiteral("info")).toObject()
                                    .value(QStringLiteral("props")).toObject()
                                    .value(QStringLiteral("device.api")).toString();
            if (api == QStringLiteral("alsa") || api == QStringLiteral("bluez5")
                || api == QStringLiteral("v4l2") || api == QStringLiteral("libcamera")) {
                return true;
            }
        }
        return false;
    }

    QTemporaryDir m_root{QDir::tempPath() + QStringLiteral("/mz-graph-XXXXXX")};
    std::vector<std::unique_ptr<QProcess>> m_processes;
};
