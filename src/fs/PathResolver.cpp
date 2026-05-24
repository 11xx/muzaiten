#include "fs/LinkRoot.h"

#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace {

QString normalizedPath(QString path)
{
    if (path.isEmpty()) {
        return {};
    }
    return QDir::cleanPath(std::move(path));
}

bool prefixMatches(const QString &path, const QString &prefix)
{
    if (prefix.isEmpty()) {
        return false;
    }
    if (path == prefix) {
        return true;
    }
    return path.startsWith(prefix + QLatin1Char('/'));
}

QString replacePrefix(const QString &path, const QString &sourcePrefix, const QString &targetPrefix)
{
    if (path == sourcePrefix) {
        return targetPrefix;
    }

    const QString suffix = path.mid(sourcePrefix.size());
    return normalizedPath(targetPrefix + suffix);
}

bool isUsableFor(const QFileInfo &info, PathUse use)
{
    if (!info.exists() || !info.isFile()) {
        return false;
    }
    if (use == PathUse::Read) {
        return info.isReadable();
    }
    return info.isWritable();
}

} // namespace

PathResolver::PathResolver(QVector<LinkRoot> linkRoots)
    : m_linkRoots(std::move(linkRoots))
{
    std::stable_sort(m_linkRoots.begin(), m_linkRoots.end(), [](const LinkRoot &left, const LinkRoot &right) {
        return left.priority > right.priority;
    });
}

void PathResolver::setLinkRoots(QVector<LinkRoot> linkRoots)
{
    m_linkRoots = std::move(linkRoots);
    std::stable_sort(m_linkRoots.begin(), m_linkRoots.end(), [](const LinkRoot &left, const LinkRoot &right) {
        return left.priority > right.priority;
    });
}

PathResolution PathResolver::resolveLocalPath(const QString &path, PathUse use) const
{
    const QString cleanPath = normalizedPath(path);
    return resolveSourcePath(cleanPath, cleanPath, use);
}

PathResolution PathResolver::resolveMpdUri(const QString &uri, const QString &musicDirectory, PathUse use) const
{
    if (uri.isEmpty()) {
        PathResolution resolution;
        resolution.sourceUri = uri;
        resolution.failureReason = QStringLiteral("MPD URI is empty");
        return resolution;
    }
    if (QDir::isAbsolutePath(uri)) {
        return resolveSourcePath(uri, normalizedPath(uri), use);
    }
    if (musicDirectory.trimmed().isEmpty()) {
        PathResolution resolution;
        resolution.sourceUri = uri;
        resolution.failureReason = QStringLiteral("MPD music_directory is unknown");
        return resolution;
    }

    const QString sourcePath = normalizedPath(QDir(musicDirectory).filePath(uri));
    return resolveSourcePath(uri, sourcePath, use);
}

PathResolution PathResolver::resolveSourcePath(const QString &sourceUri, const QString &sourcePath, PathUse use) const
{
    PathResolution resolution;
    resolution.sourceUri = sourceUri;
    resolution.candidates = candidatesForSourcePath(sourcePath);

    for (const QString &candidate : resolution.candidates) {
        const QFileInfo info(candidate);
        if (!isUsableFor(info, use)) {
            continue;
        }

        resolution.preferredPath = candidate;
        resolution.exists = true;
        resolution.readable = info.isReadable();
        resolution.writable = info.isWritable();
        return resolution;
    }

    resolution.failureReason = use == PathUse::Read
        ? QStringLiteral("No readable file candidate exists")
        : QStringLiteral("No writable file candidate exists");
    return resolution;
}

QStringList PathResolver::candidatesForSourcePath(const QString &sourcePath) const
{
    QStringList candidates;
    auto addCandidate = [&candidates](const QString &path) {
        const QString cleanPath = normalizedPath(path);
        if (!cleanPath.isEmpty() && !candidates.contains(cleanPath)) {
            candidates.push_back(cleanPath);
        }
    };

    for (const LinkRoot &root : m_linkRoots) {
        if (!root.enabled) {
            continue;
        }
        const QString sourcePrefix = normalizedPath(root.sourcePrefix);
        const QString targetPrefix = normalizedPath(root.targetPrefix);
        if (!prefixMatches(sourcePath, sourcePrefix)) {
            continue;
        }
        addCandidate(replacePrefix(sourcePath, sourcePrefix, targetPrefix));
    }

    addCandidate(sourcePath);
    return candidates;
}
