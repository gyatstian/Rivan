// DiscordPresence.h
// Optional Discord Rich Presence via Discord IPC (named pipes). No SDK dependency.
// Off when disabled or Discord is closed. Worker thread owns I/O.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

namespace rivan::discord {

struct PresenceActivity final {
    std::string details;    // track title (UTF-8)
    std::string state;      // optional artist (UTF-8); empty = omit
    // Artwork tooltip (UTF-8); empty = omit. Carries the current synced verse while
    // verse syncing is active; empty otherwise (no placeholder text).
    std::string imageText;
    bool showGithubButton = false;
    bool playing = false;
    bool hasTrack = false;
    // Unix seconds; 0 = omit. Used for Discord progress when duration known.
    std::int64_t startUnix = 0;
    std::int64_t endUnix = 0;
};

enum class ActivityUpdateKind : std::uint8_t {
    Track,
    Lyric,
};

class DiscordPresence final {
public:
    DiscordPresence();
    ~DiscordPresence();

    DiscordPresence(const DiscordPresence&) = delete;
    DiscordPresence& operator=(const DiscordPresence&) = delete;

    void SetEnabled(bool enabled);
    void SetActivity(PresenceActivity activity,
                     ActivityUpdateKind kind = ActivityUpdateKind::Track);
    void Clear();

private:
    void WorkerMain(std::stop_token stop);
    [[nodiscard]] bool EnsureConnected(std::stop_token stop);
    void Disconnect() noexcept;
    [[nodiscard]] bool WriteFrame(std::uint32_t opcode, const std::string& payload);
    [[nodiscard]] bool ReadFrame(std::uint32_t& opcode, std::string& payload,
                                 std::chrono::steady_clock::time_point deadline,
                                 std::stop_token stop = {});
    [[nodiscard]] bool Handshake(std::chrono::steady_clock::time_point deadline,
                                 std::stop_token stop);
    [[nodiscard]] bool PublishActivity(const PresenceActivity& activity);
    void DrainIncoming();

    std::jthread worker_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    // `current_` is retained for a reconnect. New track activity supersedes queued
    // lyrics; one lyric slot retains only the latest transition between sends.
    PresenceActivity current_{};
    std::optional<PresenceActivity> pendingTrack_;
    std::optional<PresenceActivity> pendingLyric_;
    bool dirty_{true};
    bool clearRequested_{};
    std::atomic_bool enabled_{false};
    void* pipe_{}; // HANDLE
    std::uint64_t nonce_{};
};

} // namespace rivan::discord
