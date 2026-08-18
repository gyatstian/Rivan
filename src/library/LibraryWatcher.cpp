// LibraryWatcher.cpp
// Change-notification watcher over the library roots. Uses per-root recursive change
// notifications with a quiet-period debounce so bulk copy/rename bursts coalesce into a
// single callback.
#include "LibraryWatcher.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <chrono>
#include <utility>

namespace rivan::library {
namespace {

struct NotificationHandles final {
    ~NotificationHandles() {
        for (const HANDLE handle : handles) {
            if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
                FindCloseChangeNotification(handle);
            }
        }
    }
    std::vector<HANDLE> handles;
};

} // namespace

LibraryWatcher::~LibraryWatcher() {
    Shutdown();
}

void LibraryWatcher::SetNotify(std::function<void()> notify) {
    std::scoped_lock lock(notifyMutex_);
    notify_ = std::move(notify);
}

void LibraryWatcher::SetRoots(std::vector<std::filesystem::path> roots) {
    {
        std::scoped_lock lock(stateMutex_);
        roots_ = std::move(roots);
        if (thread_.joinable()) {
            thread_.request_stop();
        }
    }
    if (thread_.joinable()) thread_.join();
    {
        std::scoped_lock lock(stateMutex_);
        if (roots_.empty()) return;
        thread_ = std::jthread([this](std::stop_token stop) { Run(std::move(stop)); });
    }
}

void LibraryWatcher::Shutdown() {
    SetRoots({});
}

void LibraryWatcher::Run(std::stop_token stop) {
    std::vector<std::filesystem::path> roots;
    {
        std::scoped_lock lock(stateMutex_);
        roots = roots_;
    }
    NotificationHandles notifications;
    notifications.handles.reserve(roots.size());
    for (const auto& root : roots) {
        if (root.empty()) continue;
        const HANDLE handle = FindFirstChangeNotificationW(
            root.c_str(), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
        if (handle != INVALID_HANDLE_VALUE) notifications.handles.push_back(handle);
    }
    if (notifications.handles.empty()) return;

    constexpr DWORD kPollMilliseconds = 250;
    constexpr auto kQuietPeriod = std::chrono::milliseconds(1500);
    auto lastChange = std::chrono::steady_clock::time_point{};
    bool changed = false;
    while (!stop.stop_requested()) {
        const DWORD wait = WaitForMultipleObjects(
            static_cast<DWORD>(notifications.handles.size()),
            notifications.handles.data(), FALSE, kPollMilliseconds);
        if (wait >= WAIT_OBJECT_0 &&
            wait < WAIT_OBJECT_0 + notifications.handles.size()) {
            const auto index = wait - WAIT_OBJECT_0;
            (void)FindNextChangeNotification(notifications.handles[index]);
            changed = true;
            lastChange = std::chrono::steady_clock::now();
            continue;
        }
        if (changed && wait == WAIT_TIMEOUT &&
            std::chrono::steady_clock::now() - lastChange >= kQuietPeriod) {
            changed = false;
            std::function<void()> notify;
            {
                std::scoped_lock lock(notifyMutex_);
                notify = notify_;
            }
            if (notify) notify();
        }
    }
}

} // namespace rivan::library
#endif // _WIN32