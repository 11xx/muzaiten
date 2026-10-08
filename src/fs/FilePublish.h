#pragma once

#include <QByteArray>
#include <QString>

#include <functional>

namespace FilePublish {

enum class Outcome {
    Published,
    Exists,
    Failed,
};

// Publishes bytes as one complete file at path.
//
// Without replacement, the bytes are written and synced to a sibling temporary
// file, which is then hard-linked to path. link(2) fails with EEXIST when any
// entry, including a symlink or a directory, already occupies path, so a
// destination that appears after the caller's precheck is reported as Exists
// and is left untouched. Every outcome attempts to remove the temporary file.
// Filesystems without hard links (FAT, exFAT, many network mounts) create the
// destination exclusively instead. That path never replaces an entry, but the
// destination is visible while it is being written and is removed if writing
// fails.
//
// With replacement, the destination is swapped atomically (QSaveFile), and an
// existing file keeps its permissions. A new file gets the same permissions
// as in the exclusive path: 0666 before the umask.
Outcome publish(const QString &path, const QByteArray &bytes, bool replace);

// Test seams that production code never installs. The hook runs once the
// complete content is written and before the destination is published. The
// link function stands in for link(2) to exercise the no-hard-link fallback.
void setBeforePublishHookForTesting(std::function<void()> hook);
void setLinkFunctionForTesting(int (*linkFunction)(const char *, const char *));

} // namespace FilePublish
