#pragma once

#include <atomic>
#include <string>
#include <vector>

#include <QtTypes>

// Fast parallel directory enumeration inspired by fd: a bounded pool of threads
// pulls directories off a shared queue and classifies entries via dirent.d_type,
// avoiding a stat() per entry (the dominant cost of QDirIterator + QFileInfo).
// Only supported audio files are returned; each carries the mtime/size needed by
// the incremental-rescan diff (one lstat per audio file, not per entry).
class DirectoryWalker {
public:
    struct Found {
        std::string path;
        qint64 mtime = 0;
        qint64 size = 0;
    };

    struct Config {
        int threads = 4;
        bool lowPriority = true;
        const std::atomic_bool *cancel = nullptr;
    };

    explicit DirectoryWalker(Config config);

    // Blocking. Returns absolute paths of supported audio files under root.
    // Symlinks are not followed (matching the previous scanner semantics).
    std::vector<Found> enumerate(const std::string &root) const;

private:
    Config m_config;
};
