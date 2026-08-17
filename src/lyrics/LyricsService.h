#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_set>
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
    // Songs (by file path) whose lyrics the user explicitly disabled. Lookups match
    // path-insensitively against the disabled set and skip all lyric sources.
    void SetDisabledSongs(const std::unordered_set<std::wstring>& songs);
    // Off restricts lookup to local lyrics files (user lyrics and previously saved
    // fetches) and disables the online fetchers (lrclib.net / lyrics.ovh).
    void SetOnlineEnabled(bool enabled);
    // When enabled, lyric lines without timestamps get generated LRC-style timestamps
    // spaced 4-7 seconds apart so the document behaves as synced. In-memory only; the
    // on-disk lyrics are never rewritten.
    void SetFakeTimestampsEnabled(bool enabled);
    // filePath identifies the backing media file so user-authored lyrics stay tied to
    // the song even when its metadata (title/artist/album/duration) changes.
    void Request(std::uint64_t trackId, std::wstring title, std::wstring artist,
                 std::wstring album, double durationSeconds,
                 std::filesystem::path filePath = {});
    void Reset();
    void Shutdown();

    [[nodiscard]] LyricsSnapshot Snapshot() const;

    [[nodiscard]] std::uint64_t Revision() const noexcept {
        return publishedRevision_.load(std::memory_order_acquire);
    }

    // Creates (or reuses) a human-editable lyrics file named after the song inside the
    // cache directory, embedding the song's file path in its header so the service can
    // retrieve the right lyrics when the same song is chosen again. Returns the file
    // path, or an empty path when creation failed. The file is opened for the user by
    // the caller.
    [[nodiscard]] std::filesystem::path CreateUserLyricsFile(
        std::uint64_t trackId, std::wstring title, std::wstring artist,
        std::wstring album, double durationSeconds, std::filesystem::path filePath) const;

    [[nodiscard]] static LyricsDocument ParseLrc(std::wstring_view text);
    [[nodiscard]] static LyricsDocument ParseLrclibResponse(std::string_view json);
    // Parses a lyrics file body: leading '#RIVAN-CUSTOM-LYRICS-1' header lines are
    // stripped, then the rest is parsed line-by-line like LRC so plain and synced lyrics
    // share one linebreak-aware parser.
    [[nodiscard]] static LyricsDocument ParseCustomLyrics(std::wstring_view text);
    // Assigns generated timestamps to every line missing one: untimed lines follow the
    // previous line's time by a random 4-7 seconds. Already-timed lines keep their time.
    // Returns a copy with document.synced = true once any line was assigned a timestamp.
    // Used for the fake-timestamps preference; results are never persisted.
    [[nodiscard]] static LyricsDocument WithFakeTimestamps(LyricsDocument document);
    // Extracts the '#Song file:' header value from a lyrics file body.
    [[nodiscard]] static std::wstring UserLyricsTrackPath(std::wstring_view text);
    // Canonical key for the per-song lyrics disabled set: the song file path folded to
    // lowercase so one spelling stays consistent across loads and restarts.
    [[nodiscard]] static std::wstring NormalizedTrackPath(const std::filesystem::path& path);
    // Writes lyrics in the unified human-readable format named after the song title and
    // embedding the song's file path in its header. Used for both fetched and user
    // lyrics. Never overwrites an existing lyrics file that already references the song;
    // returns the path that was written (or the pre-existing file's path).
    [[nodiscard]] static std::filesystem::path SaveLyricsFile(
        const std::filesystem::path& directory, std::wstring title,
        const std::filesystem::path& filePath, const LyricsDocument& document);

private:
    struct RequestData final {
        std::uint64_t trackId{};
        std::wstring title;
        std::wstring artist;
        std::wstring album;
        double durationSeconds{};
        std::filesystem::path filePath;
        std::uint64_t generation{};
    };

    void Worker(std::stop_token stop);
    [[nodiscard]] std::optional<LyricsDocument> LoadCache(const RequestData& request) const;
    // Unified lyrics files (suffix ".txt" with the RIVAN-CUSTOM-LYRICS marker): fetched
    // and user-authored lyrics share one format. Loaded before the legacy fingerprint
    // cache and before fetching.
    [[nodiscard]] std::optional<LyricsDocument> LoadLyricsFiles(const RequestData& request) const;
    void SaveCache(const RequestData& request, const LyricsDocument& document) const;
    [[nodiscard]] std::optional<LyricsDocument> Fetch(const RequestData& request,
                                                      std::stop_token stop) const;
    void Publish(const RequestData& request, LyricsDocument document, std::wstring status);
    void PublishSnapshotLocked() noexcept;
    [[nodiscard]] std::filesystem::path CachePath(const RequestData& request) const;

    std::filesystem::path cacheDirectory_;
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::optional<RequestData> pending_;
    LyricsSnapshot snapshot_;
    Notify notify_;
    bool cacheEnabled_{};
    bool onlineEnabled_{true};
    bool fakeTimestampsEnabled_{};
    std::unordered_set<std::wstring> disabledSongs_;
    std::uint64_t generation_{};
    std::jthread worker_;
    std::atomic<std::uint64_t> publishedRevision_{0};
};

} // namespace rivan::lyrics
