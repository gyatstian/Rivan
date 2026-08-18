// src/stats/StatsService.cpp
// Runtime listen-statistics service: samples the transport, runs the continuous
// session / 30% play logic, and persists to the Stats folder.
#include "StatsService.h"

#include "../core/Text.h"

#include <algorithm>
#include <cmath>
#include <ctime>

namespace rivan::stats {
namespace {

constexpr auto kSampleInterval = std::chrono::milliseconds(500);
constexpr auto kPersistInterval = std::chrono::seconds(10);

void MergePeriod(PeriodData& target, const PeriodData& source) noexcept {
    target.plays += source.plays;
    target.seconds += source.seconds;
}

void MergeEntity(EntityData& target, const EntityData& source) noexcept {
    MergePeriod(target.week, source.week);
    MergePeriod(target.fourWeeks, source.fourWeeks);
    MergePeriod(target.month, source.month);
    MergePeriod(target.sixMonths, source.sixMonths);
    MergePeriod(target.year, source.year);
    MergePeriod(target.lifetime, source.lifetime);
}

} // namespace

StatsService::StatsService(TransportSource transportSource,
                           std::filesystem::path statsDirectory)
    : transportSource_(std::move(transportSource)),
      statsDirectory_(std::move(statsDirectory)),
      mainFile_(statsDirectory_ / L"rivan-stats.ini") {
    // Resume previously accumulated stats. A missing file is not an error (fresh
    // install); any other load failure is ignored so stats never crash startup.
    // Legacy tolerance: older installs wrote the main file without the ".ini"
    // suffix. If the extensionless file still exists, load it so accumulated
    // counters are not lost; saves still target the ".ini" file, which migrates
    // the data to the new name on the next persist.
    std::error_code ec;
    const auto legacyMain = statsDirectory_ / L"rivan-stats";
    if (!std::filesystem::exists(mainFile_, ec) && std::filesystem::exists(legacyMain, ec)) {
        (void)LoadMainStatsFile(legacyMain, model_, nullptr);
    } else {
        (void)LoadMainStatsFile(mainFile_, model_, nullptr);
    }
    PublishSnapshotLocked();
    thread_ = std::jthread([this](std::stop_token stop) { Run(std::move(stop)); });
}

StatsService::~StatsService() {
    thread_.request_stop();
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void StatsService::Run(std::stop_token stop) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop.stop_requested()) {
        cv_.wait_for(lock, kSampleInterval, [&] { return stop.stop_requested() || wake_; });
        wake_ = false;
        if (stop.stop_requested()) break;
        const auto live = transportSource_();  // cheap atomics, safe under lock
        const auto now = std::chrono::steady_clock::now();
        SampleTick(live, now, lock);

        // Periodic persist: serialize under the lock, write outside it. A failed
        // write keeps dirty_ set so the next tick retries.
        if (enabled_ && dirty_ && now - lastPersist_ >= kPersistInterval) {
            const auto document = BuildMainStatsDocument(model_);
            lock.unlock();
            const bool saved = document.SaveAtomic(mainFile_, nullptr);
            lock.lock();
            if (saved) {
                dirty_ = false;
                lastPersist_ = now;
            }
        }
    }
    // Final persist on stop: same serialize-under-lock / write-outside pattern.
    if (dirty_) {
        const auto document = BuildMainStatsDocument(model_);
        lock.unlock();
        const bool saved = document.SaveAtomic(mainFile_, nullptr);
        lock.lock();
        if (saved) dirty_ = false;
    }
}

void StatsService::SampleTick(const audio::LiveTransport& live,
                              std::chrono::steady_clock::time_point now,
                              std::unique_lock<std::mutex>& lock) {
    (void)lock;
    if (!enabled_) return;

    // Period rollover: recompute at most once per wall-clock second; snapshot any
    // period whose boundary has been crossed and clear its counters.
    std::time_t wallNow = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    if (wallNow != lastWallSecond_) {
        lastWallSecond_ = wallNow;
        std::tm localNow{};
        if (localtime_s(&localNow, &wallNow) == 0) {
            const auto keys = CurrentPeriodKeys(localNow);
            const PeriodKeys before = model_.periods;
            const auto written = RolloverPeriods(statsDirectory_, model_, keys);
            if (model_.periods.weekMonday != before.weekMonday ||
                model_.periods.fourWeekStart != before.fourWeekStart ||
                model_.periods.month != before.month ||
                model_.periods.sixMonthStart != before.sixMonthStart ||
                model_.periods.year != before.year) {
                dirty_ = true;
                PublishSnapshotLocked();
            }
        }
    }

    const bool playing = live.state == audio::PlaybackState::Playing;
    if (playing) {
        if (activeTrack_ && sessionActive_ && sessionPath_ != activeTrack_->filePath) {
            // std::filesystem::path == compares case-insensitively on Windows. A path
            // change closes the current session and falls through to start the new one.
            CloseSession();
        }
        if (!sessionActive_) {
            if (!activeTrack_) {
                lastSampleTime_ = now;
                return;
            }
            // Sessions start immediately on the first playing tick for the track, so the sampler can begin
            // accumulating on the very first tick for the track.
            BeginSessionLocked(*activeTrack_);
        }

        // Accumulate listened seconds. Only ticks where playback was already running
        // count (pause/resume gaps are excluded); delta is clamped so a thread stall or
        // system sleep cannot inflate the counters.
        double delta = 0.0;
        if (playingLast_) {
            delta = std::chrono::duration<double>(now - lastSampleTime_).count();
            delta = std::clamp(delta, 0.0, 10.0);
        }
        lastSampleTime_ = now;
        const double total = delta + sessionFraction_;
        const auto whole = static_cast<std::uint64_t>(std::floor(total));
        sessionFraction_ = total - static_cast<double>(whole);
        if (whole > 0) {
            session_.AddSeconds(static_cast<double>(whole));
            model_.AddSeconds(sessionSongSection_, whole);
            dirty_ = true;
            PublishSnapshotLocked();
        }

        // 30% play rule: one play per continuous session once 30% is reached.
        const double durationSeconds =
            live.duration.count() > 0 ? std::chrono::duration<double>(live.duration).count()
                                      : (activeTrack_ ? (*activeTrack_).durationSeconds : 0.0);
        if (session_.CountPlayIfQualified(durationSeconds)) {
            model_.AddPlay(sessionSongSection_);
            dirty_ = true;
            PublishSnapshotLocked();
        }
        playingLast_ = true;
    } else {
        if (live.state == audio::PlaybackState::Ended ||
            live.state == audio::PlaybackState::Stopped) {
            CloseSession();
        }
        // Paused/Loading/Idle keep the session open: pause/resume is one continuous
        // session, so only a hard stop/end resets the 30% accumulation.
        lastSampleTime_ = now;
        playingLast_ = false;
    }
}

void StatsService::BeginSessionLocked(const library::Track& track) {
    sessionPath_ = track.filePath;
    sessionSongSection_ = SongSectionName(track.id);
    model_.SetSongPath(sessionSongSection_, core::WideToUtf8(track.filePath.wstring()));

    session_.Reset();
    sessionFraction_ = 0;
    playingLast_ = true;
    lastSampleTime_ = std::chrono::steady_clock::now();
    sessionActive_ = true;
}

void StatsService::CloseSession() {
    sessionActive_ = false;
    sessionPath_.clear();
    sessionSongSection_.clear();
    session_.Reset();
    sessionFraction_ = 0;
    playingLast_ = false;
}

void StatsService::SetEnabled(bool enabled) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!enabled) {
            // Stop accumulation before the disable-save so the sampler cannot dirty the
            // model while the atomic write runs outside the lock.
            enabled_ = false;
            if (dirty_ || sessionActive_) {
                const auto document = BuildMainStatsDocument(model_);
                lock.unlock();
                const bool saved = document.SaveAtomic(mainFile_, nullptr);
                lock.lock();
                // Only clear dirty_ when the write actually succeeded; a failed write
                // leaves it set so a later save retries.
                if (saved) dirty_ = false;
            }
            CloseSession();
        } else {
            enabled_ = true;
        }
        wake_ = true;
    }
    cv_.notify_one();
}

void StatsService::Flush() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!dirty_) return;
    const auto document = BuildMainStatsDocument(model_);
    lock.unlock();
    const bool saved = document.SaveAtomic(mainFile_, nullptr);
    lock.lock();
    if (saved) dirty_ = false;
}

void StatsService::OnPlaybackRestarted() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        CloseSession();
        wake_ = true;
    }
    cv_.notify_one();
}

void StatsService::SetActiveTrack(std::optional<library::Track> track) {
    std::lock_guard<std::mutex> lock(mutex_);
    activeTrack_ = std::move(track);
}

void StatsService::ApplyTrackRename(std::uint64_t oldId, std::uint64_t newId,
                                    const std::filesystem::path& oldPath,
                                    const std::filesystem::path& newPath) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto oldSection = SongSectionName(oldId);
        const auto newSection = SongSectionName(newId);
        if (oldSection != newSection) {
            const auto found = model_.entities.find(oldSection);
            if (found != model_.entities.end()) {
                auto entity = std::move(found->second);
                model_.entities.erase(found);
                entity.songPath = core::WideToUtf8(newPath.wstring());
                const auto target = model_.entities.find(newSection);
                if (target != model_.entities.end()) {
                    MergeEntity(target->second, entity);
                } else {
                    model_.entities.emplace(newSection, std::move(entity));
                }
                dirty_ = true;
                PublishSnapshotLocked();
            }
            // Redirect the live session so post-rename seconds keep accumulating under
            // the renamed song instead of recreating the old section on the next tick.
            if (sessionActive_ && sessionSongSection_ == oldSection) {
                sessionSongSection_ = newSection;
                sessionPath_ = newPath;
                dirty_ = true;
            }
            wake_ = true;
        }
    }
    cv_.notify_one();
    // Historical period snapshots keyed by the old id move too (best effort).
    (void)RewriteSnapshotSongIdentifier(statsDirectory_, mainFile_, oldId, newId,
                                        core::WideToUtf8(oldPath.wstring()),
                                        core::WideToUtf8(newPath.wstring()));
}

std::shared_ptr<const ListenStatsModel> StatsService::Snapshot() const noexcept {
    return publishedModel_.load(std::memory_order_acquire);
}

void StatsService::PublishSnapshotLocked() {
    publishedModel_.store(std::make_shared<const ListenStatsModel>(model_),
                          std::memory_order_release);
}

} // namespace rivan::stats
