#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>
#include <string_view>

namespace rivan::lyrics {

struct LyricLine final {
    double timestampSeconds{-1.0};
    std::wstring text;
};

struct LyricsDocument final {
    std::vector<LyricLine> lines;
    bool synced{};

    [[nodiscard]] bool Empty() const noexcept { return lines.empty(); }
    [[nodiscard]] std::wstring PlainText() const;
};

struct LyricsSnapshot final {
    std::uint64_t trackId{};
    bool loading{};
    bool available{};
    LyricsDocument document;
    std::wstring status{L"No lyrics available"};
    std::uint64_t revision{};
};

class LyricsService final {
public:
    using Notify = std::function<void()>;

    explicit LyricsService(std::filesystem::path cacheDirectory = {});
    ~LyricsService();

    LyricsService(const LyricsService&) = delete;
    LyricsService& operator=(const LyricsService&) = delete;

    void SetNotify(Notify notify);
    void SetCacheEnabled(bool enabled);
    void Request(std::uint64_t trackId, std::wstring title, std::wstring artist,
                 std::wstring album, double durationSeconds);
    void Reset();
    void Shutdown();

    [[nodiscard]] LyricsSnapshot Snapshot() const;

    [[nodiscard]] std::uint64_t Revision() const noexcept {
        return publishedRevision_.load(std::memory_order_acquire);
    }

    [[nodiscard]] static LyricsDocument ParseLrc(std::wstring_view text);
    [[nodiscard]] static LyricsDocument ParseLrclibResponse(std::string_view json);

private:
    struct RequestData final {
        std::uint64_t trackId{};
        std::wstring title;
        std::wstring artist;
        std::wstring album;
        double durationSeconds{};
        std::uint64_t generation{};
    };

    void Worker(std::stop_token stop);
    [[nodiscard]] std::optional<LyricsDocument> LoadCache(const RequestData& request) const;
    void SaveCache(const RequestData& request, const LyricsDocument& document) const;
    [[nodiscard]] std::optional<LyricsDocument> Fetch(const RequestData& request,
                                                      std::stop_token stop) const;
    void Publish(const RequestData& request, LyricsDocument document, std::wstring status);
    [[nodiscard]] std::filesystem::path CachePath(const RequestData& request) const;

    std::filesystem::path cacheDirectory_;
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::optional<RequestData> pending_;
    LyricsSnapshot snapshot_;
    Notify notify_;
    bool cacheEnabled_{};
    std::uint64_t generation_{};
    std::jthread worker_;
    std::atomic<std::uint64_t> publishedRevision_{0};
};

} // namespace rivan::lyrics
