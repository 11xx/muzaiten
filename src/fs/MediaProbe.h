#pragma once

#include <QString>

// Storage-class probe shared by the library scanner and the analysis
// pipeline. Network filesystems never expose a meaningful sysfs rotational
// flag, so they get their own class: both rotational disks and network
// mounts pay heavily for concurrent seeks, and callers use seekSensitive()
// to start IO-parallel work conservatively there.
namespace MediaProbe {

enum class Class {
    Fast,        // local non-rotational (SSD/NVMe/tmpfs)
    Rotational,  // local spinning disk
    Network,     // NFS/SMB/CIFS/FUSE-backed remote
};

Class classify(const QString &path);
bool seekSensitive(Class value);
QString name(Class value);

} // namespace MediaProbe
