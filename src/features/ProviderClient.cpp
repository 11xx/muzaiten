#include "features/ProviderClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QUuid>

namespace FeatureProvider {
namespace {

struct Candidate {
    QString path;
    QString source;
};

QString executableAt(const QString &value)
{
    if (value.isEmpty()) {
        return {};
    }
    const QFileInfo info(value);
    if (info.isAbsolute() || value.contains(QDir::separator())) {
        return info.exists() && info.isExecutable() ? info.absoluteFilePath() : QString();
    }
    return QStandardPaths::findExecutable(value);
}

QList<Candidate> candidates(const QString &explicitPath, const QString &savedPath)
{
    QList<Candidate> values;
    const auto add = [&](const QString &path, const QString &source) {
        if (!path.trimmed().isEmpty()) {
            values.push_back({path.trimmed(), source});
        }
    };
    add(explicitPath, QStringLiteral("cli"));
    add(qEnvironmentVariable("MUZAITEN_FEATURES_CLAP"), QStringLiteral("environment"));
    add(savedPath, QStringLiteral("saved-setting"));
    add(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("muzaiten-features-clap")),
        QStringLiteral("sibling"));

    const QString uvToolBin = qEnvironmentVariable("UV_TOOL_BIN_DIR");
    if (!uvToolBin.isEmpty()) {
        add(QDir(uvToolBin).filePath(QStringLiteral("muzaiten-features-clap")),
            QStringLiteral("UV_TOOL_BIN_DIR"));
    }
    if (const QString uv = QStandardPaths::findExecutable(QStringLiteral("uv")); !uv.isEmpty()) {
        QProcess process;
        process.start(uv, {QStringLiteral("tool"), QStringLiteral("dir"), QStringLiteral("--bin")});
        if (process.waitForFinished(2'000) && process.exitCode() == 0) {
            add(QDir(QString::fromUtf8(process.readAllStandardOutput()).trimmed())
                    .filePath(QStringLiteral("muzaiten-features-clap")),
                QStringLiteral("uv-tool-bin"));
        }
    }
    const QString xdgData = qEnvironmentVariable("XDG_DATA_HOME",
                                                  QDir::homePath() + QStringLiteral("/.local/share"));
    add(QDir(xdgData).filePath(QStringLiteral("../bin/muzaiten-features-clap")),
        QStringLiteral("xdg-user-bin"));
    add(QStringLiteral("muzaiten-features-clap"), QStringLiteral("PATH"));
    return values;
}

void relayLine(const QByteArray &line)
{
    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);
    out << QString::fromUtf8(line) << '\n';
    out.flush();
}

} // namespace

Invocation invoke(const QString &path,
                  const QString &operation,
                  const QJsonObject &parameters,
                  bool relayJsonl,
                  const std::function<bool()> &canceled,
                  int operationTimeoutMs)
{
    Invocation invocation;
    QProcess process;
    process.setProgram(path);
    process.start();
    if (!process.waitForStarted(operationTimeoutMs)) {
        invocation.exitCode = 3;
        invocation.errorCode = QStringLiteral("component_missing");
        invocation.errorMessage = process.errorString();
        return invocation;
    }

    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject request{
        {QStringLiteral("protocol_version"), 1},
        {QStringLiteral("request_id"), requestId},
        {QStringLiteral("operation"), operation},
        {QStringLiteral("parameters"), parameters},
    };
    process.write(QJsonDocument(request).toJson(QJsonDocument::Compact));
    process.closeWriteChannel();

    QByteArray buffered;
    bool terminationRequested = false;
    QElapsedTimer elapsed;
    elapsed.start();
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(100);
        buffered += process.readAllStandardOutput();
        for (qsizetype newline = buffered.indexOf('\n'); newline >= 0;
             newline = buffered.indexOf('\n')) {
            const QByteArray line = buffered.left(newline).trimmed();
            buffered.remove(0, newline + 1);
            if (line.isEmpty()) {
                continue;
            }
            QJsonParseError error;
            const QJsonDocument document = QJsonDocument::fromJson(line, &error);
            if (error.error != QJsonParseError::NoError || !document.isObject()) {
                invocation.errorCode = QStringLiteral("malformed_event");
                invocation.errorMessage = error.errorString();
                process.kill();
                process.waitForFinished(1'000);
                return invocation;
            }
            const QJsonObject event = document.object();
            if (event.value(QStringLiteral("protocol_version")).toInt() != 1
                || event.value(QStringLiteral("request_id")).toString() != requestId) {
                invocation.errorCode = QStringLiteral("protocol_mismatch");
                invocation.errorMessage = QStringLiteral("provider event did not match the request");
                process.kill();
                process.waitForFinished(1'000);
                return invocation;
            }
            const QString kind = event.value(QStringLiteral("event")).toString();
            if (relayJsonl && (kind == QLatin1String("progress") || kind == QLatin1String("phase"))) {
                relayLine(line);
            }
            if (kind == QLatin1String("result")) {
                invocation.result = event.value(QStringLiteral("result")).toObject();
            } else if (kind == QLatin1String("error")) {
                invocation.errorCode = event.value(QStringLiteral("code")).toString();
                invocation.errorMessage = event.value(QStringLiteral("message")).toString();
            }
        }
        if (!terminationRequested && canceled()) {
            terminationRequested = true;
            process.terminate();
        }
        if (!terminationRequested && operationTimeoutMs > 0 && elapsed.elapsed() > operationTimeoutMs) {
            invocation.errorCode = QStringLiteral("timeout");
            invocation.errorMessage = QStringLiteral("provider operation timed out");
            process.kill();
            process.waitForFinished(1'000);
            return invocation;
        }
        if (terminationRequested && !process.waitForFinished(3'000)) {
            process.kill();
        }
    }
    buffered += process.readAllStandardOutput();
    if (!buffered.trimmed().isEmpty()) {
        const QList<QByteArray> lines = buffered.split('\n');
        for (const QByteArray &raw : lines) {
            const QByteArray line = raw.trimmed();
            if (line.isEmpty()) {
                continue;
            }
            const QJsonDocument document = QJsonDocument::fromJson(line);
            if (!document.isObject()) {
                invocation.errorCode = QStringLiteral("malformed_event");
                invocation.errorMessage = QStringLiteral("provider emitted malformed JSON");
                return invocation;
            }
            const QJsonObject event = document.object();
            if (event.value(QStringLiteral("protocol_version")).toInt() != 1
                || event.value(QStringLiteral("request_id")).toString() != requestId) {
                invocation.errorCode = QStringLiteral("protocol_mismatch");
                invocation.errorMessage = QStringLiteral("provider event did not match the request");
                return invocation;
            }
            const QString kind = event.value(QStringLiteral("event")).toString();
            if (relayJsonl && (kind == QLatin1String("progress") || kind == QLatin1String("phase"))) {
                relayLine(line);
            }
            if (kind == QLatin1String("result")) {
                invocation.result = event.value(QStringLiteral("result")).toObject();
            } else if (kind == QLatin1String("error")) {
                invocation.errorCode = event.value(QStringLiteral("code")).toString();
                invocation.errorMessage = event.value(QStringLiteral("message")).toString();
            }
        }
    }
    invocation.exitCode = terminationRequested ? 130 : process.exitCode();
    if (invocation.exitCode == 0 && invocation.result.isEmpty()) {
        invocation.exitCode = 4;
        invocation.errorCode = QStringLiteral("missing_result");
        invocation.errorMessage = QStringLiteral("provider exited without a terminal result");
    }
    return invocation;
}

std::optional<Resolved> discover(const QString &explicitPath, const QString &savedPath)
{
    QSet<QString> seen;
    for (const Candidate &candidate : candidates(explicitPath, savedPath)) {
        const QString path = executableAt(candidate.path);
        if (path.isEmpty()) {
            continue;
        }
        const QString canonical = QFileInfo(path).canonicalFilePath();
        if (seen.contains(canonical)) {
            continue;
        }
        seen.insert(canonical);
        const Invocation handshake = invoke(path, QStringLiteral("capabilities"), {}, false, [] { return false; });
        if (handshake.exitCode != 0
            || handshake.result.value(QStringLiteral("capability")).toString() != QLatin1String("clap")) {
            continue;
        }
        const QJsonArray versions = handshake.result.value(QStringLiteral("protocol_versions")).toArray();
        if (!versions.contains(1)) {
            continue;
        }
        return Resolved{path, candidate.source, handshake.result};
    }
    return std::nullopt;
}

} // namespace FeatureProvider
