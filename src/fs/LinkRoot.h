#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

enum class PathUse {
    Read,
    Write
};

struct LinkRoot {
    int id = 0;
    QString name;
    QString sourcePrefix;
    QString targetPrefix;
    int priority = 0;
    bool readable = true;
    bool writable = false;
    bool enabled = true;
};

struct PathResolution {
    QString sourceUri;
    QString preferredPath;
    QStringList candidates;
    QString failureReason;
    bool exists = false;
    bool readable = false;
    bool writable = false;
};

class PathResolver final {
public:
    explicit PathResolver(QVector<LinkRoot> linkRoots = {});

    void setLinkRoots(QVector<LinkRoot> linkRoots);
    PathResolution resolveLocalPath(const QString &path, PathUse use) const;
    PathResolution resolveMpdUri(const QString &uri, const QString &musicDirectory, PathUse use) const;

private:
    PathResolution resolveSourcePath(const QString &sourceUri, const QString &sourcePath, PathUse use) const;
    QStringList candidatesForSourcePath(const QString &sourcePath) const;

    QVector<LinkRoot> m_linkRoots;
};
