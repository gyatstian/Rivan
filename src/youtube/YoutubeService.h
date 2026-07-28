// YoutubeService.h
// Optional yt-dlp front-end: search, URL resolve, and audio download into the library.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace rivan::youtube {

struct YoutubeEntry final {
    std::uint64_t id{};
    std::wstring videoId;
    std::wstring title;
    std::wstring webpageUrl;
    double durationSeconds{};
    std::filesystem::path localPath;
    bool downloading{};
    bool failed{};
    // -1 = unknown/n/a; 0..100 while downloading or converting.
    float downloadProgress{-1.0F};
};

enum class YoutubeJobKind : std::uint8_t { Idle, Search, Download, Install };

enum class YoutubeTool : std::uint8_t { YtDlp, Ffmpeg };

inline constexpr std::size_t kSearchPageSize = 20;
// Cap yt-dlp batch size; results stream into the UI one line at a time.
inline constexpr std::size_t kSearchFetchCount = 40;

struct YoutubeSnapshot final {
    YoutubeJobKind job{YoutubeJobKind::Idle};
    bool busy{};
    std::wstring status;
    std::vector<YoutubeEntry> entries;
    std::uint64_t generation{};
    bool ytDlpInstalled{};
    bool ffmpegInstalled{};
    bool installingYtDlp{};
    bool installingFfmpeg{};
    // Client-side pages over the last search batch (yt-dlp fetch is capped).
    std::size_t searchPage{};
    std::size_t searchPageCount{1};
    bool searchIsPaged{};  // false for URL resolves / local library lists
};

class YoutubeService final {
public:
    YoutubeService();
    ~YoutubeService();

    YoutubeService(const YoutubeService&) = delete;
    YoutubeService& operator=(const YoutubeService&) = delete;

    // Notifies the UI thread (typically via PostMessage) when a job advances.
    void SetNotify(std::function<void()> notify);

    [[nodiscard]] static std::filesystem::path ToolsDirectory();
    [[nodiscard]] static std::optional<std::filesystem::path> LocateYtDlp();
    [[nodiscard]] static std::optional<std::filesystem::path> LocateFfmpeg();
    [[nodiscard]] static bool LooksLikeUrl(std::wstring_view text) noexcept;
    [[nodiscard]] static std::filesystem::path DownloadDirectory(
        const std::filesystem::path& musicRoot);

    // Cancel any in-flight job and clear entries/status when the feature is disabled.
    void Reset();
    void Cancel();

    // Refresh installed flags on the snapshot (cheap path probes).
    void RefreshToolStatus();

    // Prefetch yt-dlp into the OS page cache (async). Call when the user opens the
    // Youtube browser so the first real search skips cold process load.
    void Warm();

    // One-click HTTPS download into ToolsDirectory() (async).
    void InstallTool(YoutubeTool tool);

    // Search or resolve a URL into entries (async).
    // musicSearch: use YouTube Music search + music.youtube.com watch URLs.
    void SubmitQuery(std::wstring query, bool musicSearch = false);
    // Download one entry by id into musicRoot/Youtube (async).
    // downloadMode: 0 = MP3 (ffmpeg transcode), 1 = Original (native m4a/opus, no ffmpeg),
    //   2 = Video (mp4 with picked audio stream).
    // audioQuality: yt-dlp audio quality 0 (best) .. 9 (worst). Shared by all modes:
    //   MP3 encode VBR, Original stream pick, Video audio-stream pick.
    // mp4VideoQuality: 0=lowest .. 5=best height (Video mode only).
    // musicSearch: prefer music.youtube.com URL + music player client for audio.
    void Download(std::uint64_t entryId, std::filesystem::path musicRoot,
                  int audioQuality = 0, int downloadMode = 0, int mp4VideoQuality = 0,
                  bool musicSearch = false);
    // Flip client-side search page (0-based). Returns false if out of range / not paged.
    bool SetSearchPage(std::size_t page);

    [[nodiscard]] YoutubeSnapshot Snapshot() const;

private:
    void Notify() const;
    void RunSearch(std::stop_token stop, std::wstring query, bool musicSearch);
    void RunDownload(std::stop_token stop, std::uint64_t entryId,
                     std::filesystem::path musicRoot, int audioQuality, int downloadMode,
                     int mp4VideoQuality, bool musicSearch);
    void RunInstall(std::stop_token stop, YoutubeTool tool);
    void RunWarm(std::stop_token stop);
    void JoinWorker();
    void WriteToolFlagsLocked();
    void StoreSearchCacheLocked(const std::wstring& cacheKey, std::vector<YoutubeEntry> entries);

    // Small in-memory LRU of recent search results so an identical re-search renders
    // instantly instead of re-spawning yt-dlp (process + network startup is the main
    // browsing latency). Most-recently-used lives at the back. Cleared on Reset().
    // cacheKey includes source prefix (y: / m:) so YT and YTM stay separate.
    struct CachedSearch final {
        std::wstring query;
        std::vector<YoutubeEntry> entries;
    };
    static constexpr std::size_t kSearchCacheMax = 24;

    mutable std::mutex mutex_;
    YoutubeSnapshot state_;
    std::vector<CachedSearch> searchCache_;
    std::function<void()> notify_;
    std::jthread worker_;
};

} // namespace rivan::youtube
