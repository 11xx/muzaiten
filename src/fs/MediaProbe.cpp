#include "fs/MediaProbe.h"

#include <QFile>
#include <QFileInfo>

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>

namespace MediaProbe {
namespace {

bool isNetworkFilesystem(const QString &path)
{
    struct statfs fs {};
    if (statfs(path.toUtf8().constData(), &fs) != 0) {
        return false;
    }
    // f_type magics from statfs(2). FUSE counts as network: the dominant
    // FUSE mounts for music libraries (sshfs, rclone, davfs) are remote,
    // and misclassifying a local FUSE overlay only costs a conservative
    // starting parallelism that the adaptive gate raises again.
    switch (static_cast<unsigned int>(fs.f_type)) {
    case 0x6969U:      // NFS_SUPER_MAGIC
    case 0x517BU:      // SMB_SUPER_MAGIC
    case 0xFE534D42U:  // SMB2_MAGIC_NUMBER
    case 0xFF534D42U:  // CIFS_SUPER_MAGIC
    case 0x01021997U:  // V9FS_MAGIC
    case 0x65735546U:  // FUSE_SUPER_MAGIC
    case 0x73757245U:  // CODA_SUPER_MAGIC
    case 0x47504653U:  // GPFS
    case 0x013111A8U:  // IBRIX
    case 0x0BD00BD0U:  // LUSTRE
    case 0xFF534D53U:  // SMB3
        return true;
    default:
        return false;
    }
}

bool isRotationalDevice(const QString &path)
{
    struct stat st {};
    if (stat(path.toUtf8().constData(), &st) != 0) {
        return false;
    }
    const QString devLink = QStringLiteral("/sys/dev/block/%1:%2")
                                .arg(major(st.st_dev))
                                .arg(minor(st.st_dev));
    QString devDir = QFileInfo(devLink).canonicalFilePath();
    for (int level = 0; level < 2 && !devDir.isEmpty(); ++level) {
        QFile flag(devDir + QStringLiteral("/queue/rotational"));
        if (flag.exists() && flag.open(QIODevice::ReadOnly)) {
            return flag.readAll().trimmed() == "1";
        }
        devDir = QFileInfo(devDir).path(); // partition -> parent disk
    }
    return false;
}

} // namespace

Class classify(const QString &path)
{
    if (isNetworkFilesystem(path)) {
        return Class::Network;
    }
    if (isRotationalDevice(path)) {
        return Class::Rotational;
    }
    return Class::Fast;
}

bool seekSensitive(Class value)
{
    return value == Class::Rotational || value == Class::Network;
}

QString name(Class value)
{
    switch (value) {
    case Class::Fast:
        return QStringLiteral("fast");
    case Class::Rotational:
        return QStringLiteral("rotational");
    case Class::Network:
        return QStringLiteral("network");
    }
    return QStringLiteral("fast");
}

} // namespace MediaProbe
