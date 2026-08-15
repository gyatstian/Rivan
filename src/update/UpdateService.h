// UpdateService.h
// Performs one bounded, asynchronous GitHub release check for the Rivan application.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace rivan::update {

inline constexpr std::wstring_view kCurrentVersion{L"v1.99"};

// Published by shared pointer and never mutated after construction, so callers can safely
// retain a snapshot while a later service operation replaces the current snapshot.
struct UpdateSnapshot final {
    std::wstring const currentVersion;
    std::wstring const latestVersion;
    std::wstring const releaseUrl;
    bool const updateAvailable;

    UpdateSnapshot(std::wstring latestVersion = {}, std::wstring releaseUrl = {},
                   bool updateAvailable = false);
};

class UpdateService final {
public:
    using NotifyCallback = std::function<void()>;

    UpdateService();
    ~UpdateService();

    UpdateService(const UpdateService&) = delete;
    UpdateService& operator=(const UpdateService&) = delete;

    // Starts at most one non-blocking latest-release request for this service instance.
    void StartCheck();
    void Shutdown() noexcept;

    void SetNotify(NotifyCallback callback);
    [[nodiscard]] std::shared_ptr<const UpdateSnapshot> Snapshot() const;

    // Parses a complete response returned by GitHub's /releases/latest endpoint. This is
    // public to keep malformed-response and version-normalization behavior unit-testable.
    [[nodiscard]] static std::optional<UpdateSnapshot> ParseLatestReleaseJson(
        std::string_view json);
    [[nodiscard]] static bool VersionsMatch(std::wstring_view current,
                                            std::wstring_view latest) noexcept;

private:
    void Worker(std::stop_token stopToken) noexcept;
    [[nodiscard]] std::optional<UpdateSnapshot> FetchLatestRelease(
        std::stop_token stopToken);
    void CloseRequestHandles() noexcept;

    mutable std::mutex snapshotMutex_;
    std::shared_ptr<const UpdateSnapshot> snapshot_;

    std::mutex notifyMutex_;
    NotifyCallback notify_;

    std::mutex lifecycleMutex_;
    std::jthread worker_;
    bool started_{};
    bool shuttingDown_{};

    // Shutdown closes these handles before joining the worker, interrupting a pending
    // WinHTTP operation instead of delaying application teardown for its receive timeout.
    std::mutex requestMutex_;
    void* session_{};
    void* connection_{};
    void* request_{};
};

} // namespace rivan::update
