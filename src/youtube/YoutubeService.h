// YoutubeService.h
// Optional yt-dlp front-end: search, URL probe, and audio download into the library.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
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

enum class YoutubeJobKind : std::uint8_t { Idle, Search, Probe, Download, Install };

// Deno is the JavaScript runtime yt-dlp uses to solve YouTube's JS challenges
// (EJS). Without it recent yt-dlp builds get HTTP 403 on media streams.
enum class YoutubeTool : std::uint8_t { YtDlp, Ffmpeg, Deno };

struct YoutubeVideoFormat final {
    std::wstring formatId;
    std::wstring extension;
    int height{};
    double fps{};
    std::uint64_t filesize{};
};

struct YoutubeAudioFormat final {
    std::wstring formatId;
    std::wstring extension;
    double abr{};
    std::uint64_t filesize{};
};

struct YoutubeProbe final {
    std::wstring videoId;
    std::wstring title;
    double durationSeconds{};
    std::vector<YoutubeVideoFormat> videoFormats;
    std::vector<YoutubeAudioFormat> audioFormats;
};

enum class YoutubeDownloadKind : std::uint8_t { Video, AudioOnly };

enum class YoutubeAudioOutputFormat : std::uint8_t { Native, Mp3, Aac, Opus, Flac, Wav };

struct YoutubeDownloadSelection final {
    YoutubeDownloadKind kind{YoutubeDownloadKind::AudioOnly};
    std::wstring videoFormatId;
    std::wstring preferredVideoExtension;
    int videoHeight{};
    double videoFps{};
    std::wstring audioFormatId;
    std::wstring preferredAudioExtension;
    int preferredAudioBitrate{};
    YoutubeAudioOutputFormat audioOutput{YoutubeAudioOutputFormat::Native};
    int audioQuality{};
};

inline constexpr std::size_t kSearchPageSize = 20;
// Cap yt-dlp batch size; results stream into the UI one line at a time.
inline constexpr std::size_t kSearchFetchCount = 40;

struct YoutubeSnapshot final {
    YoutubeJobKind job{YoutubeJobKind::Idle};
    bool busy{};
    std::wstring status;
    std::vector<YoutubeEntry> entries;
    std::optional<YoutubeProbe> probe;
    std::uint64_t probeEntryId{};
    std::uint64_t generation{};
    bool ytDlpInstalled{};
    bool ffmpegInstalled{};
    bool denoInstalled{};
    bool installingYtDlp{};
    bool installingFfmpeg{};
    bool installingDeno{};
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
    [[nodiscard]] static std::optional<std::filesystem::path> LocateDeno();
    [[nodiscard]] static bool LooksLikeUrl(std::wstring_view text) noexcept;
    [[nodiscard]] static bool LooksLikeYoutubeUrl(std::wstring_view text) noexcept;
    [[nodiscard]] static std::filesystem::path DownloadDirectory(
        const std::filesystem::path& musicRoot);

    // Cancel any in-flight job and clear entries/status when the feature is disabled.
    void Reset();

    // Refresh installed flags on the snapshot (cheap path probes).
    void RefreshToolStatus();

    // Prefetch yt-dlp into the OS page cache (async). Call when the user opens the
    // Youtube browser so the first real search skips cold process load.
    void Warm();

    // One-click HTTPS download into ToolsDirectory() (async).
    void InstallTool(YoutubeTool tool);

    // Search a query or directly probe a URL (async).
    void SubmitQuery(std::wstring query);
    // Probe one entry's title, duration, and available media formats (async).
    void Probe(std::uint64_t entryId);
    // Download one entry by id into musicRoot/Youtube (async).
    void Download(std::uint64_t entryId, std::filesystem::path musicRoot,
                  YoutubeDownloadSelection selection);
    // Flip client-side search page (0-based). Returns false if out of range / not paged.
    bool SetSearchPage(std::size_t page);

    [[nodiscard]] YoutubeSnapshot Snapshot() const;

private:
    void Notify() const;
    void RunSearch(std::stop_token stop, std::wstring query);
    void RunProbe(std::stop_token stop, std::uint64_t entryId);
    void RunDownload(std::stop_token stop, std::uint64_t entryId,
                     std::filesystem::path musicRoot, YoutubeDownloadSelection selection);
    void RunInstall(std::stop_token stop, YoutubeTool tool);
    void RunWarm(std::stop_token stop);
    void JoinWorker();
    void Enqueue(std::function<void()> step);
    void SupervisorLoop();
    void WriteToolFlagsLocked();
    void StoreSearchCacheLocked(const std::wstring& cacheKey, std::vector<YoutubeEntry> entries);

    // Small in-memory LRU of recent search results so an identical re-search renders
    // instantly instead of re-spawning yt-dlp (process + network startup is the main
    // browsing latency). Most-recently-used lives at the back. Cleared on Reset().
    // cacheKey includes the YouTube source prefix.
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
    // Supervisor-runner members: declared last so runner_ is joined before
    // other members are destroyed in the destructor.
    std::deque<std::function<void()>> queue_;
    std::condition_variable cv_;
    bool shutdown_{false};
    std::jthread runner_;
};

} // namespace rivan::youtube
