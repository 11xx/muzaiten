#pragma once

#include <QProcess>

#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

class SemanticStatusProcess final : public QProcess {
public:
    explicit SemanticStatusProcess(QObject *parent = nullptr) : QProcess(parent)
    {
#ifdef Q_OS_UNIX
        setUnixProcessParameters(UnixProcessFlag::CreateNewSession);
#endif
        connect(this, &QProcess::started, this, [this]() {
            if (m_cancelRequested) killOwnedGroup();
        });
    }

    void cancel()
    {
        m_cancelRequested = true;
        if (state() == Running) killOwnedGroup();
    }

private:
    void killOwnedGroup()
    {
        // The unreaped helper PID reserves its group ID until this call completes.
        if (state() != Running) return;
#ifdef Q_OS_UNIX
        const qint64 pid = processId();
        if (pid > 0) ::kill(-static_cast<pid_t>(pid), SIGKILL);
#endif
        kill();
    }

    bool m_cancelRequested = false;
};
