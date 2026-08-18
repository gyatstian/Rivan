// LibraryWatcher.h
// Watches library root folders (recursive) and fires a notify callback after file
// operations settle, so external changes (renames, new downloads, deletions) surface in
// the library without a manual refresh.
#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace rivan::library {

class LibraryWatcher final {
public:
    LibraryWatcher() = default;
    ~LibraryWatcher();

    LibraryWatcher(const LibraryWatcher&) = delete;
    LibraryWatcher& operator=(const LibraryWatcher&) = delete;

    // Sets the callback invoked (from the watcher thread) after folder activity settles.
    void SetNotify(std::function<void()> notify);
    // Restarts watching the given roots. An empty list stops the watcher thread.
    void SetRoots(std::vector<std::filesystem::path> roots);
    void Shutdown();

private:
    void Run(std::stop_token stop);

    std::function<void()> notify_;
    std::mutex notifyMutex_;
    std::mutex stateMutex_;
    std::vector<std::filesystem::path> roots_;
    std::jthread thread_;
};

} // namespace rivan::library