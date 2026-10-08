#include "features/FeatureStoreLock.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace FeatureStoreLock {

namespace {

// Linux follows at most 40 symlinks in one path lookup (MAXSYMLINKS).
constexpr int kMaximumSymlinkHops = 40;

} // namespace

QString identity(const QString &storePath)
{
    // Path strings stay unnormalized until the kernel resolves them: QFileInfo's
    // absolute paths drop ".." lexically, which is wrong beneath a symlink.
    QString candidate = storePath.startsWith(u'/') ? storePath : QDir::currentPath() + u'/' + storePath;
    for (int hop = 0; hop < kMaximumSymlinkHops; ++hop) {
        const QFileInfo info(candidate);
        if (info.exists()) {
            return info.canonicalFilePath();
        }
        if (!info.isSymLink()) {
            break;
        }
        candidate = info.symLinkTarget();
    }

    // Absent: the nearest existing ancestor fixes the location, and the missing
    // components are appended as spelled.
    QStringList absent;
    QString ancestor = candidate;
    while (!QFileInfo::exists(ancestor)) {
        const qsizetype slash = ancestor.lastIndexOf(u'/');
        if (slash < 0) {
            break;
        }
        absent.prepend(ancestor.mid(slash + 1));
        ancestor.truncate(slash > 0 ? slash : 1);
    }
    const QString base = QFileInfo(ancestor).canonicalFilePath();
    return QDir::cleanPath(base + QStringLiteral("/") + absent.join(QStringLiteral("/")));
}

QString lockPath(const QString &storePath)
{
    return identity(storePath) + QStringLiteral(".lock");
}

bool prepareDirectory(const QString &storePath)
{
    const QString directory = QFileInfo(identity(storePath)).absolutePath();
    return QDir(directory).exists() || QDir().mkpath(directory);
}

} // namespace FeatureStoreLock
