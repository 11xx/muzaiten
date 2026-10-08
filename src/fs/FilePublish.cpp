#include "fs/FilePublish.h"

#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSaveFile>

#include <cerrno>
#include <unistd.h>
#include <utility>

namespace FilePublish {

namespace {

std::function<void()> beforePublishHook;
int (*linkFunction)(const char *, const char *) = ::link;

// 0666 before the umask. The kernel applies the umask on creation, which keeps
// the permissions of a new file identical to those of a QSaveFile commit.
QFileDevice::Permissions creationPermissions()
{
    return QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup
        | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther;
}

bool occupied(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

// The errors link(2) reports on filesystems without hard links: vfat and
// exFAT, and many FUSE and SMB mounts.
bool lacksHardLinks(int code)
{
    switch (code) {
    case EPERM:
    case EOPNOTSUPP:
    case EXDEV:
        return true;
    default:
        return code == ENOTSUP;
    }
}

// The sibling name stays in the destination's directory, so the link never
// crosses a filesystem boundary. The random suffix keeps concurrent exports apart.
QString temporaryName(const QString &path)
{
    static const QString alphabet = QStringLiteral("abcdefghijklmnopqrstuvwxyz0123456789");
    QString suffix;
    for (int i = 0; i < 8; ++i) {
        suffix.append(alphabet.at(QRandomGenerator::global()->bounded(static_cast<int>(alphabet.size()))));
    }
    return path + QStringLiteral(".") + suffix + QStringLiteral(".tmp");
}

// Writes a complete, synced sibling of path. Only a colliding name is retried,
// so a failure never touches the destination.
bool writeTemporary(const QString &path, const QByteArray &bytes, QString *temporary)
{
    for (int attempt = 0; attempt < 16; ++attempt) {
        const QString candidate = temporaryName(path);
        QFile file(candidate);
        if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly, creationPermissions())) {
            if (occupied(candidate)) {
                continue;
            }
            return false;
        }
        const bool synced = file.write(bytes) == bytes.size() && file.flush() && ::fsync(file.handle()) == 0;
        file.close();
        if (!synced) {
            QFile::remove(candidate);
            return false;
        }
        *temporary = candidate;
        return true;
    }
    return false;
}

// Fallback for filesystems without hard links. An existing entry is never
// replaced, but the destination is visible while it is being written.
Outcome createExclusive(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly, creationPermissions())) {
        return occupied(path) ? Outcome::Exists : Outcome::Failed;
    }
    const bool synced = file.write(bytes) == bytes.size() && file.flush() && ::fsync(file.handle()) == 0;
    file.close();
    if (!synced) {
        QFile::remove(path);
        return Outcome::Failed;
    }
    return Outcome::Published;
}

Outcome publishExclusively(const QString &path, const QByteArray &bytes)
{
    QString temporary;
    if (!writeTemporary(path, bytes, &temporary)) {
        return Outcome::Failed;
    }
    if (beforePublishHook) {
        beforePublishHook();
    }
    Outcome outcome = Outcome::Failed;
    if (linkFunction(QFile::encodeName(temporary).constData(), QFile::encodeName(path).constData()) == 0) {
        outcome = Outcome::Published;
    } else if (errno == EEXIST) {
        outcome = Outcome::Exists;
    } else if (lacksHardLinks(errno)) {
        outcome = createExclusive(path, bytes);
    }
    QFile::remove(temporary);
    return outcome;
}

Outcome replaceAtomically(const QString &path, const QByteArray &bytes)
{
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
        return Outcome::Failed;
    }
    if (beforePublishHook) {
        beforePublishHook();
    }
    return file.commit() ? Outcome::Published : Outcome::Failed;
}

} // namespace

Outcome publish(const QString &path, const QByteArray &bytes, bool replace)
{
    return replace ? replaceAtomically(path, bytes) : publishExclusively(path, bytes);
}

void setBeforePublishHookForTesting(std::function<void()> hook)
{
    beforePublishHook = std::move(hook);
}

void setLinkFunctionForTesting(int (*replacement)(const char *, const char *))
{
    linkFunction = replacement ? replacement : ::link;
}

} // namespace FilePublish
