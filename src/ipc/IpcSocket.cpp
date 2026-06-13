#include "ipc/IpcSocket.h"

#include "app/AppPaths.h"

#include <QCryptographicHash>
#include <QDir>

namespace IpcSocket {

QString serverPath()
{
    // The state dir is the per-instance identity: every override mechanism
    // (flags, env, --state-root, dev-state, config file) funnels through it.
    const QByteArray identity = AppPaths::stateDir().toUtf8();
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha1).toHex().left(8));

    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR").trimmed();
    const QString base = runtime.isEmpty() ? QDir::tempPath() : runtime;
    return QDir(base).filePath(QStringLiteral("muzaiten-%1.sock").arg(hash));
}

QString lockPath()
{
    return QDir(AppPaths::stateDir()).filePath(QStringLiteral("muzaiten.lock"));
}

} // namespace IpcSocket
