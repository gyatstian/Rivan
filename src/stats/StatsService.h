// src/stats/StatsService.h
// Runtime listen-statistics service: samples the transport, runs the continuous
// session / 30% play logic, and persists to the Stats folder.
#pragma once

#include "../audio/AudioTypes.h"
#include "../library/Track.h"
#include "ListenStats.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

namespace rivan::stats {

class StatsService final {
public:
    using TransportSource = std::function<audio::LiveTransport()>;

    // transportSource must remain valid for the whole service lifetime (App owns
    // audio_; App destroys stats_ before audio_). Thread starts here.
    StatsService(TransportSource transportSource, std::filesystem::path statsDirectory);
    ~StatsService();

    StatsService(const StatsService&) = delete;
    StatsService& operator=(const StatsService&) = delete;

    // Starts/stops accumulation. Persists pending state when disabling.
    void SetEnabled(bool enabled);
    [[nodiscard]] bool Enabled() const;

    // App pushes the currently active library track (or nullopt when none).
    void SetActiveTrack(std::optional<library::Track> track);

    // Persists the in-memory state to the main file now (idempotent, no-op if clean).
    void Flush();

    // Reads the last published immutable model without acquiring the service mutex. The
    // returned copy is safe to retain while the sampler updates its private model.
    [[nodiscard]] std::shared_ptr<const ListenStatsModel> Snapshot() const noexcept;

    // Same-file restart (repeat-one, or PlayPause-restart from stopped) starts a fresh
    // listening session so a repeated full listen earns another play. The state sampler
    // cannot see EndOfStream -> Loading -> Playing collapsing under one tick.
    void OnPlaybackRestarted();

private:
    void Run(std::stop_token stop);
    void SampleTick(const audio::LiveTransport& live,
                    std::chrono::steady_clock::time_point now,
                    std::unique_lock<std::mutex>& lock);
    // Starts a fresh listening session for the given track. Caller holds mutex_.
    void BeginSessionLocked(const library::Track& track);
    void CloseSession();
    void PublishSnapshotLocked();

    TransportSource transportSource_;
    std::filesystem::path statsDirectory_;
    std::filesystem::path mainFile_;

    mutable std::mutex mutex_;
    bool enabled_{};
    bool dirty_{};
    std::chrono::steady_clock::time_point lastPersist_{};
    ListenStatsModel model_;
    std::atomic<std::shared_ptr<const ListenStatsModel>> publishedModel_;
    std::optional<library::Track> activeTrack_;

    // Current session state (all mutated only on the sampler thread).
    bool sessionActive_{};
    std::wstring sessionPath_;
    std::string sessionSongSection_;
    ListenSession session_;
    double sessionFraction_{};  // sub-second carry for seconds attribution
    bool playingLast_{};
    std::chrono::steady_clock::time_point lastSampleTime_{};
    // Wall-clock second of the last period-rollover check (recompute at most 1/sec).
    std::time_t lastWallSecond_{-1};

    std::jthread thread_;
    std::condition_variable_any cv_;
    bool wake_{};
};

} // namespace rivan::stats
