#include "features/FeatureStoreLock.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace FeatureStoreLock {

namespace {

// Linux follows at most 40 symlinks in one path lookup (MAXSYMLINKS).
constexpr int kMaximumSymlinkExpansions = 40;

// The directory above an absolute path whose components are already resolved.
QString parentOf(const QString &resolved)
{
    const qsizetype slash = resolved.lastIndexOf(u'/');
    return slash > 0 ? resolved.left(slash) : QStringLiteral("/");
}

} // namespace

QString identity(const QString &storePath)
{
    // Components are resolved left to right, the way realpath(3) resolves them
    // with missing names allowed. The resolved prefix never contains a symlink,
    // so `..` can drop its last component and still match the kernel. A dangling
    // link is expanded like a live one, which lets a missing directory reached
    // through a symlink be created at its real location.
    QStringList pending = (storePath.startsWith(u'/') ? storePath : QDir::currentPath() + u'/' + storePath)
                              .split(u'/', Qt::SkipEmptyParts);
    QString resolved = QStringLiteral("/");
    int expansions = 0;
    while (!pending.isEmpty()) {
        const QString name = pending.takeFirst();
        if (name == QLatin1String(".")) {
            continue;
        }
        if (name == QLatin1String("..")) {
            resolved = parentOf(resolved);
            continue;
        }
        const QString candidate = QDir(resolved).filePath(name);
        const QFileInfo info(candidate);
        if (!info.isSymLink()) {
            resolved = candidate;
            continue;
        }
        if (++expansions > kMaximumSymlinkExpansions) {
            return {};
        }
        // An absolute target restarts from the root. A relative one continues from
        // the link's directory, which is already resolved. The raw target keeps `..`
        // unresolved: symLinkTarget() would collapse it lexically before a symlink
        // earlier in the target is expanded.
        const QString target = info.readSymLink();
        if (target.startsWith(u'/')) {
            resolved = QStringLiteral("/");
        }
        pending = target.split(u'/', Qt::SkipEmptyParts) + pending;
    }
    return resolved;
}

QString lockPath(const QString &storePath)
{
    const QString store = identity(storePath);
    return store.isEmpty() ? QString() : store + QStringLiteral(".lock");
}

bool prepareDirectory(const QString &storePath)
{
    const QString store = identity(storePath);
    if (store.isEmpty()) {
        return false;
    }
    const QString directory = parentOf(store);
    return QDir(directory).exists() || QDir().mkpath(directory);
}

} // namespace FeatureStoreLock
