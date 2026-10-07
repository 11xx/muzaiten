#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QThread>

#ifdef Q_OS_LINUX
#include <cerrno>
#include <csignal>
#include <sys/prctl.h>
#include <sys/wait.h>
#endif

class SemanticDescendantFixture final {
public:
    explicit SemanticDescendantFixture(const QString &directory)
        : root(directory), binary(root.filePath(QStringLiteral("muzaiten-features")))
        , savedPath(qgetenv("PATH")), savedRoot(qgetenv("MUZAITEN_SEMANTIC_FIXTURE_ROOT"))
    {
    }

    bool install()
    {
#ifdef Q_OS_LINUX
        if (prctl(PR_GET_CHILD_SUBREAPER, &savedSubreaper) != 0
            || prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) return false;
        subreaperSet = true;
#endif
        if (!QDir().mkpath(root.path())
            || !QFile::copy(QDir(QCoreApplication::applicationDirPath())
                               .filePath(QStringLiteral("semantic_status_fixture")), binary)) return false;
        qputenv("PATH", root.path().toUtf8() + ':' + savedPath);
        qputenv("MUZAITEN_SEMANTIC_FIXTURE_ROOT", root.path().toUtf8());
        sentinel.start(binary, {QStringLiteral("sentinel")});
        return sentinel.waitForStarted(1'000);
    }

    ~SemanticDescendantFixture()
    {
        release();
        QFile latch(root.filePath(QStringLiteral("sentinel-release")));
        if (latch.open(QIODevice::WriteOnly)) latch.close();
        if (!sentinel.waitForFinished(1'000) && sentinel.state() != QProcess::NotRunning) {
            sentinel.kill();
            sentinel.waitForFinished(1'000);
        }
#ifdef Q_OS_LINUX
        QElapsedTimer deadline;
        deadline.start();
        while (subreaperSet && !providerExited() && deadline.elapsed() < 1'000) QThread::msleep(10);
        if (subreaperSet) prctl(PR_SET_CHILD_SUBREAPER, savedSubreaper);
#endif
        qputenv("PATH", savedPath);
        if (savedRoot.isNull()) qunsetenv("MUZAITEN_SEMANTIC_FIXTURE_ROOT");
        else qputenv("MUZAITEN_SEMANTIC_FIXTURE_ROOT", savedRoot);
    }

    QJsonObject read(const QString &name) const
    {
        QFile file(root.filePath(name + QStringLiteral(".json")));
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object()
                                            : QJsonObject{};
    }

    bool ready() const { return !read(QStringLiteral("provider")).isEmpty(); }

    bool providerExited()
    {
#ifdef Q_OS_LINUX
        if (reaped) return true;
        const auto pid = pid_t(read(QStringLiteral("provider")).value(QStringLiteral("pid")).toInteger());
        if (pid <= 0) return false;
        int status = 0;
        providerWaitResult = waitpid(pid, &status, WNOHANG);
        providerWaitErrno = providerWaitResult < 0 ? errno : 0;
        if (providerWaitResult != pid) return false;
        providerWaitStatus = status;
        reaped = true;
        return true;
#else
        return false;
#endif
    }

    bool release() const
    {
        QFile latch(root.filePath(QStringLiteral("provider-release")));
        return latch.open(QIODevice::WriteOnly);
    }

    bool sentinelAlive() const { return sentinel.state() == QProcess::Running; }

    bool recordResult(qint64 cancelMs, bool helperExited, bool heartbeat, qint64 actionMs = 0) const
    {
        QFile file(root.filePath(QStringLiteral("result.json")));
        const QJsonObject result{
            {QStringLiteral("helper"), read(QStringLiteral("helper"))},
            {QStringLiteral("provider"), read(QStringLiteral("provider"))},
            {QStringLiteral("sentinel"), read(QStringLiteral("sentinel"))},
            {QStringLiteral("cancel_elapsed_ms"), cancelMs},
            {QStringLiteral("cancel_action_ms"), actionMs},
            {QStringLiteral("helper_exited"), helperExited},
            {QStringLiteral("provider_reaped"), reaped},
            {QStringLiteral("provider_wait_status"), providerWaitStatus},
            {QStringLiteral("provider_wait_result"), providerWaitResult},
            {QStringLiteral("provider_wait_errno"), providerWaitErrno},
            {QStringLiteral("sentinel_alive"), sentinelAlive()},
            {QStringLiteral("heartbeat"), heartbeat}};
        return file.open(QIODevice::WriteOnly)
            && file.write(QJsonDocument(result).toJson()) > 0;
    }

    const QDir root;
    const QString binary;
    QProcess sentinel;
    const QByteArray savedPath;
    const QByteArray savedRoot;
    bool reaped = false;
    int providerWaitStatus = 0;
    qint64 providerWaitResult = 0;
    int providerWaitErrno = 0;
#ifdef Q_OS_LINUX
    int savedSubreaper = 0;
    bool subreaperSet = false;
#endif
};
