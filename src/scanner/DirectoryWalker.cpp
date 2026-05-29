#include "scanner/DirectoryWalker.h"

#include "scanner/LibraryScanner.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>

#include <dirent.h>
#include <sys/resource.h>
#include <sys/stat.h>

namespace {

std::unordered_set<std::string> lowercaseExtensionSet()
{
    std::unordered_set<std::string> extensions;
    for (const QString &extension : LibraryScanner::supportedAudioExtensions()) {
        extensions.insert(extension.toStdString());
    }
    return extensions;
}

// Lowercased suffix after the final '.', empty if none / dotfile.
std::string lowerSuffix(const char *name, std::size_t length)
{
    std::size_t dot = length;
    for (std::size_t i = length; i > 0; --i) {
        if (name[i - 1] == '.') {
            dot = i - 1;
            break;
        }
    }
    if (dot == length || dot == 0 || dot + 1 >= length) {
        return {};
    }
    std::string suffix(name + dot + 1, length - dot - 1);
    for (char &c : suffix) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return suffix;
}

} // namespace

DirectoryWalker::DirectoryWalker(Config config)
    : m_config(config)
{
}

std::vector<DirectoryWalker::Found> DirectoryWalker::enumerate(const std::string &root) const
{
    const std::unordered_set<std::string> audioExtensions = lowercaseExtensionSet();

    std::deque<std::string> queue;
    std::mutex mutex;
    std::condition_variable cv;
    int pending = 0;
    bool done = false;

    {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push_back(root);
        ++pending;
    }

    const int threadCount = std::max(1, m_config.threads);
    std::vector<std::vector<Found>> perThread(static_cast<std::size_t>(threadCount));

    const auto cancelled = [this]() {
        return m_config.cancel != nullptr && m_config.cancel->load();
    };

    const auto worker = [&](int index) {
        if (m_config.lowPriority) {
            setpriority(PRIO_PROCESS, 0, 10);
        }
        std::vector<Found> &results = perThread[static_cast<std::size_t>(index)];

        for (;;) {
            std::string dirPath;
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&]() { return done || !queue.empty(); });
                if (done && queue.empty()) {
                    return;
                }
                dirPath = std::move(queue.front());
                queue.pop_front();
            }

            if (!cancelled()) {
                DIR *dir = opendir(dirPath.c_str());
                if (dir != nullptr) {
                    while (struct dirent *entry = readdir(dir)) {
                        if (cancelled()) {
                            break;
                        }
                        const char *name = entry->d_name;
                        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
                            continue;
                        }

                        std::string childPath = dirPath;
                        childPath.push_back('/');
                        childPath.append(name);

                        unsigned char type = entry->d_type;
                        if (type == DT_UNKNOWN) {
                            struct stat st {};
                            if (lstat(childPath.c_str(), &st) != 0) {
                                continue;
                            }
                            if (S_ISLNK(st.st_mode)) {
                                continue; // do not follow symlinks
                            }
                            type = S_ISDIR(st.st_mode) ? DT_DIR : (S_ISREG(st.st_mode) ? DT_REG : DT_LNK);
                        }

                        if (type == DT_DIR) {
                            std::lock_guard<std::mutex> lock(mutex);
                            queue.push_back(std::move(childPath));
                            ++pending;
                            cv.notify_one();
                        } else if (type == DT_REG) {
                            const std::size_t len = std::char_traits<char>::length(name);
                            const std::string suffix = lowerSuffix(name, len);
                            if (suffix.empty() || audioExtensions.find(suffix) == audioExtensions.end()) {
                                continue;
                            }
                            struct stat st {};
                            if (lstat(childPath.c_str(), &st) != 0) {
                                continue;
                            }
                            Found found;
                            found.path = std::move(childPath);
                            found.mtime = static_cast<qint64>(st.st_mtime);
                            found.size = static_cast<qint64>(st.st_size);
                            results.push_back(std::move(found));
                        }
                        // DT_LNK and other types are skipped.
                    }
                    closedir(dir);
                }
            }

            std::lock_guard<std::mutex> lock(mutex);
            if (cancelled()) {
                done = true;
                cv.notify_all();
                return;
            }
            if (--pending == 0) {
                done = true;
                cv.notify_all();
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(threadCount));
    for (int i = 0; i < threadCount; ++i) {
        threads.emplace_back(worker, i);
    }
    for (std::thread &thread : threads) {
        thread.join();
    }

    std::vector<Found> merged;
    std::size_t total = 0;
    for (const std::vector<Found> &chunk : perThread) {
        total += chunk.size();
    }
    merged.reserve(total);
    for (std::vector<Found> &chunk : perThread) {
        for (Found &found : chunk) {
            merged.push_back(std::move(found));
        }
    }
    return merged;
}
