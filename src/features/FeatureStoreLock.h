#pragma once

#include <QString>

// Writer-lock identity for a feature store. Every spelling that reaches one
// database file (directory or file symlinks, relative paths, a dangling link
// to a store not yet created) takes the same QLockFile. Hard links name one
// inode through two paths; they are not resolved and do not share a lock.
namespace FeatureStoreLock {

// Absolute path of the store with symlinks resolved. An absent store resolves
// through its nearest existing ancestor, so it names the same location a later
// write will create.
QString identity(const QString &storePath);

// The QLockFile path every writer takes for the store: identity(storePath)
// with ".lock" appended.
QString lockPath(const QString &storePath);

// Creates the store's directory when absent, so the writer lock can be created
// before the first write creates the store. Returns false when that fails.
bool prepareDirectory(const QString &storePath);

} // namespace FeatureStoreLock
