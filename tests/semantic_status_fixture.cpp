#include "features/ProviderClient.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QTextStream>

#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

namespace {

QJsonObject processIdentity()
{
    QJsonObject identity{{QStringLiteral("pid"), QCoreApplication::applicationPid()}};
#ifdef Q_OS_UNIX
    identity.insert(QStringLiteral("ppid"), qint64(getppid()));
    identity.insert(QStringLiteral("pgid"), qint64(getpgrp()));
#endif
    return identity;
}

bool record(const QDir &root, const QString &name, const QJsonObject &object)
{
    QFile file(root.filePath(name + QStringLiteral(".tmp")));
    if (!file.open(QIODevice::WriteOnly)) return false;
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size()) return false;
    file.close();
    return file.rename(root.filePath(name));
}

void hold(const QDir &root, const QString &latch)
{
    while (!QFile::exists(root.filePath(latch))) QThread::msleep(10);
}

void emitObject(const QJsonObject &object)
{
    QTextStream out(stdout);
    out << QJsonDocument(object).toJson(QJsonDocument::Compact) << '\n';
    out.flush();
}

QJsonObject modelStatus()
{
    return {{QStringLiteral("model"), QJsonObject{
                {QStringLiteral("source"), QStringLiteral("https://example.org/model")},
                {QStringLiteral("license"), QStringLiteral("CC0-1.0")},
                {QStringLiteral("approximate_bytes"), 790'000'000},
                {QStringLiteral("cache_path"), QStringLiteral("/isolated/model-cache")},
                {QStringLiteral("present"), true}, {QStringLiteral("valid"), true}}},
            {QStringLiteral("model_extra_installed"), true}};
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QDir root(qEnvironmentVariable("MUZAITEN_SEMANTIC_FIXTURE_ROOT"));
    if (root.path() == QLatin1String(".")) return 2;
    const QStringList args = app.arguments().mid(1);
    if (args.value(0) == QLatin1String("sentinel")) {
        if (!record(root, QStringLiteral("sentinel.json"), processIdentity())) return 2;
        hold(root, QStringLiteral("sentinel-release"));
        return 0;
    }
    if (args.value(0) == QLatin1String("status") || args.value(0) == QLatin1String("relay")) {
        QJsonObject identity = processIdentity();
        identity.insert(QStringLiteral("args"), QJsonArray::fromStringList(args));
        if (!record(root, QStringLiteral("helper.json"), identity)) return 2;
        if (qEnvironmentVariableIsSet("MUZAITEN_SEMANTIC_FIXTURE_START_LATCH")) {
            hold(root, QStringLiteral("helper-release"));
        }
        const auto invocation = FeatureProvider::invoke(app.applicationFilePath(), QStringLiteral("status"),
                                                        {}, args.value(0) == QLatin1String("relay"),
                                                        [] { return false; }, 0);
        if (invocation.exitCode != 0) return invocation.exitCode;
        emitObject({{QStringLiteral("semantic"), QJsonObject{
                        {QStringLiteral("state"), QStringLiteral("provider-ready")},
                        {QStringLiteral("provider"), invocation.result}}}});
        return 0;
    }
    if (args == QStringList{QStringLiteral("model"), QStringLiteral("download")}) {
        record(root, QStringLiteral("download.json"), processIdentity());
        return 0;
    }
    if (!args.isEmpty()) return 2;

    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly)) return 2;
    const QJsonObject request = QJsonDocument::fromJson(input.readAll()).object();
    const QString operation = request.value(QStringLiteral("operation")).toString();
    QJsonObject result;
    if (operation == QLatin1String("capabilities")) {
        result = {{QStringLiteral("capability"), QStringLiteral("clap")},
                  {QStringLiteral("protocol_versions"), QJsonArray{1}},
                  {QStringLiteral("operations"), QJsonArray{QStringLiteral("status")}}};
    } else {
#ifdef Q_OS_UNIX
        std::signal(SIGTERM, SIG_IGN);
#endif
        if (!record(root, QStringLiteral("provider.json"), processIdentity())) return 2;
        hold(root, QStringLiteral("provider-release"));
        result = modelStatus();
    }
    if (qEnvironmentVariableIsSet("MUZAITEN_SEMANTIC_FIXTURE_JSONL")) {
        emitObject({{QStringLiteral("protocol_version"), 1},
                    {QStringLiteral("request_id"), request.value(QStringLiteral("request_id"))},
                    {QStringLiteral("event"), QStringLiteral("phase")},
                    {QStringLiteral("phase"), QStringLiteral("fixture")}});
    }
    emitObject({{QStringLiteral("protocol_version"), 1},
                {QStringLiteral("request_id"), request.value(QStringLiteral("request_id"))},
                {QStringLiteral("event"), QStringLiteral("result")},
                {QStringLiteral("result"), result}});
    return 0;
}
