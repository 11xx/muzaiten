#pragma once

#include <QString>

// Writer-lock identity for a feature store. Every spelling that reaches one
// database file (directory or file symlinks, relative paths, and symlinks to a
// store or directory that does not exist yet) takes the same QLockFile. Hard
// links name one inode through two paths; they are not resolved and do not
// share a lock.
namespace FeatureStoreLock {

// Absolute path of the store with every symlink replaced by its target, whether
// or not the target exists. Names that do not exist are kept, and `.` and `..`
// among them are applied lexically, as realpath -m applies them. Returns an empty
// string when the path cannot be resolved, such as a symlink cycle: no
// coordinate is safe to lock on in that case.
QString identity(const QString &storePath);

// identity(storePath) with ".lock" appended, or an empty string when identity()
// is empty.
QString lockPath(const QString &storePath);

// Creates the directory that holds identity(storePath) when it is absent, so the
// writer lock can exist before the first write creates the store. Returns false
// when the path does not resolve or the directory cannot be created.
bool prepareDirectory(const QString &storePath);

} // namespace FeatureStoreLock
