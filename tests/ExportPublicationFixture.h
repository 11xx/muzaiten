#pragma once

#include "fs/FilePublish.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QString>
#include <QStringList>

#include <cerrno>
#include <unistd.h>

// Helpers for publication tests. They report failures through their results
// and leave the assertions to the calling test.
namespace ExportPublicationFixture {

inline const QByteArray foreignContent = QByteArrayLiteral("FOREIGN OWNER CONTENT\n");
inline const QByteArray exportContent = QByteArrayLiteral("{\"exported\": true}\n");

inline void writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(bytes);
    }
}

inline QByteArray contents(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

inline QStringList entries(const QString &directory)
{
    QStringList names = QDir(directory).entryList(QDir::Files | QDir::System | QDir::Hidden);
    names.sort();
    return names;
}

inline void makeSymlink(const QString &target, const QString &path)
{
    ::symlink(QFile::encodeName(target).constData(), QFile::encodeName(path).constData());
}

inline int permissionBits(const QString &path)
{
    return QFileInfo(path).permissions().toInt();
}

// Permission bits of a file committed by QSaveFile, the behaviour publication keeps
// for new files. The scratch directory must not be the directory under test.
inline int referencePermissionBits(const QString &scratchDirectory)
{
    QDir().mkpath(scratchDirectory);
    const QString path = QDir(scratchDirectory).filePath(QStringLiteral("reference.bin"));
    QSaveFile reference(path);
    if (reference.open(QIODevice::WriteOnly)) {
        reference.write(exportContent);
        reference.commit();
    }
    return permissionBits(path);
}

inline int linkWithoutHardLinks(const char *, const char *)
{
    errno = EPERM;
    return -1;
}

inline int linkFailingUnexpectedly(const char *, const char *)
{
    errno = EIO;
    return -1;
}

// Clears the publication seams when a test ends, so later tests start clean.
class SeamReset final {
public:
    SeamReset() = default;
    SeamReset(const SeamReset &) = delete;
    SeamReset &operator=(const SeamReset &) = delete;
    ~SeamReset()
    {
        FilePublish::setBeforePublishHookForTesting(nullptr);
        FilePublish::setLinkFunctionForTesting(nullptr);
    }
};

} // namespace ExportPublicationFixture
