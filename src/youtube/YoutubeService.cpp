// YoutubeService.cpp
#include "YoutubeService.Internal.h"

#include "../core/AppPaths.h"

#include <algorithm>
#include <utility>

namespace rivan::youtube {

YoutubeService::YoutubeService() {
    RefreshToolStatus();
}

YoutubeService::~YoutubeService() {
    JoinWorker();
}

void YoutubeService::SetNotify(std::function<void()> notify) {
    std::scoped_lock lock(mutex_);
    notify_ = std::move(notify);
}

std::filesystem::path YoutubeService::ToolsDirectory() {
    return core::AppPaths::LocalDataRoot() / L"tools";
}

std::optional<std::filesystem::path> YoutubeService::LocateYtDlp() {
    const auto tools = ToolsDirectory() / L"yt-dlp.exe";
    if (detail::PathExistsFile(tools)) return tools;
    return std::nullopt;
}

std::optional<std::filesystem::path> YoutubeService::LocateFfmpeg() {
    const auto tools = ToolsDirectory() / L"ffmpeg.exe";
    if (detail::PathExistsFile(tools)) return tools;
    return std::nullopt;
}

bool YoutubeService::LooksLikeUrl(std::wstring_view text) noexcept {
    auto trimmed = detail::Trim(std::wstring(text));
    if (trimmed.empty()) return false;
    const auto lower = detail::Lower(trimmed);
    if (lower.rfind(L"http://", 0) == 0 || lower.rfind(L"https://", 0) == 0) return true;
    if (lower.find(L"youtube.com") != std::wstring::npos) return true;
    if (lower.find(L"youtu.be") != std::wstring::npos) return true;
    return false;
}

bool YoutubeService::LooksLikeYoutubeUrl(std::wstring_view text) noexcept {
    const auto trimmed = detail::Trim(std::wstring(text));
    const auto scheme = trimmed.find(L"://");
    if (scheme == std::wstring::npos) return false;
    const auto protocol = detail::Lower(trimmed.substr(0, scheme));
    if (protocol != L"http" && protocol != L"https") return false;
    const std::size_t hostStart = scheme + 3;
    const std::size_t hostEnd = trimmed.find_first_of(L"/?#", hostStart);
    std::wstring host = detail::Lower(trimmed.substr(hostStart, hostEnd - hostStart));
    if (host.empty()) return false;
    if (const auto portSeparator = host.find(L':'); portSeparator != std::wstring::npos) {
        const auto portText = std::wstring_view(host).substr(portSeparator + 1);
        unsigned portNumber{};
        if (portText.empty() || !std::all_of(portText.begin(), portText.end(),
                                             [&portNumber](wchar_t character) {
                                                 if (character < L'0' || character > L'9') return false;
                                                 portNumber = portNumber * 10u +
                                                              static_cast<unsigned>(character - L'0');
                                                 return portNumber <= 65535u;
                                             }) || portNumber == 0u) {
            return false;
        }
        host.resize(portSeparator);
    }
    const auto isYoutubeDomain = [&host](std::wstring_view domain) {
        return host == domain ||
               (host.size() > domain.size() &&
                host.compare(host.size() - domain.size(), domain.size(), domain) == 0 &&
                host[host.size() - domain.size() - 1] == L'.');
    };
    return isYoutubeDomain(L"youtube.com") || isYoutubeDomain(L"youtu.be");
}

std::filesystem::path YoutubeService::DownloadDirectory(
    const std::filesystem::path& musicRoot) {
    return musicRoot / L"Youtube";
}

void YoutubeService::WriteToolFlagsLocked() {
    state_.ytDlpInstalled = LocateYtDlp().has_value();
    state_.ffmpegInstalled = LocateFfmpeg().has_value();
}

void YoutubeService::RefreshToolStatus() {
    std::scoped_lock lock(mutex_);
    WriteToolFlagsLocked();
    ++state_.generation;
}

void YoutubeService::Notify() const {
    std::function<void()> callback;
    {
        std::scoped_lock lock(mutex_);
        callback = notify_;
    }
    if (callback) callback();
}

void YoutubeService::JoinWorker() {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}

void YoutubeService::Reset() {
    JoinWorker();
    std::scoped_lock lock(mutex_);
    const bool yt = LocateYtDlp().has_value();
    const bool ff = LocateFfmpeg().has_value();
    state_ = YoutubeSnapshot{};
    state_.ytDlpInstalled = yt;
    state_.ffmpegInstalled = ff;
    searchCache_.clear();
    ++state_.generation;
}

void YoutubeService::StoreSearchCacheLocked(const std::wstring& query,
                                            std::vector<YoutubeEntry> entries) {
    if (query.empty() || entries.empty()) return;
    const auto existing = std::find_if(
        searchCache_.begin(), searchCache_.end(),
        [&query](const CachedSearch& cached) { return cached.query == query; });
    if (existing != searchCache_.end()) {
        existing->entries = std::move(entries);
        CachedSearch hit = std::move(*existing);
        searchCache_.erase(existing);
        searchCache_.push_back(std::move(hit));
    } else {
        if (searchCache_.size() >= kSearchCacheMax) searchCache_.erase(searchCache_.begin());
        searchCache_.push_back(CachedSearch{query, std::move(entries)});
    }
}

void YoutubeService::Cancel() {
    JoinWorker();
    std::scoped_lock lock(mutex_);
    state_.busy = false;
    state_.job = YoutubeJobKind::Idle;
    state_.installingYtDlp = false;
    state_.installingFfmpeg = false;
    for (auto& entry : state_.entries) {
        entry.downloading = false;
        if (entry.downloadProgress >= 0.0F && entry.localPath.empty()) {
            entry.downloadProgress = -1.0F;
        }
    }
    if (state_.status == L"Searching..." || state_.status == L"Probing..." ||
        state_.status == L"Downloading..." ||
        state_.status.rfind(L"Downloading ", 0) == 0 || state_.status == L"Converting..." ||
        state_.status == L"Resolving..." || state_.status == L"Installing yt-dlp..." ||
        state_.status == L"Installing ffmpeg...") {
        state_.status = L"Cancelled";
    }
    WriteToolFlagsLocked();
    ++state_.generation;
}

void YoutubeService::SubmitQuery(std::wstring query) {
    query = detail::Trim(std::move(query));
    if (query.empty()) return;

    JoinWorker();
    const bool url = LooksLikeUrl(query);
    if (url) {
        YoutubeEntry entry;
        entry.id = detail::HashText(query);
        entry.title = L"Preparing YouTube video...";
        entry.webpageUrl = std::move(query);
        const auto entryId = entry.id;
        {
            std::scoped_lock lock(mutex_);
            state_.entries = {std::move(entry)};
            state_.busy = true;
            state_.job = YoutubeJobKind::Probe;
            state_.status = L"Probing...";
            state_.probe.reset();
            state_.probeEntryId = entryId;
            state_.searchPage = 0;
            state_.searchPageCount = 1;
            state_.searchIsPaged = false;
            ++state_.generation;
        }
        Notify();
        worker_ = std::jthread([this, entryId](std::stop_token stop) {
            RunProbe(stop, entryId);
        });
        return;
    }

    const std::wstring cacheKey = L"y:" + query;
    bool cacheHit = false;
    {
        std::scoped_lock lock(mutex_);
        const auto cached = std::find_if(
            searchCache_.begin(), searchCache_.end(),
            [&cacheKey](const CachedSearch& entry) { return entry.query == cacheKey; });
        if (cached != searchCache_.end() && !cached->entries.empty()) {
            state_.entries = cached->entries;
            state_.probe.reset();
            state_.probeEntryId = 0;
            state_.busy = false;
            state_.job = YoutubeJobKind::Idle;
            state_.searchPage = 0;
            state_.searchIsPaged = true;
            state_.searchPageCount =
                (state_.entries.size() + kSearchPageSize - 1) / kSearchPageSize;
            if (state_.searchPageCount == 0) state_.searchPageCount = 1;
            state_.status = std::to_wstring(state_.entries.size()) +
                            L" result(s) · page 1/" +
                            std::to_wstring(state_.searchPageCount);
            CachedSearch hit = std::move(*cached);
            searchCache_.erase(cached);
            searchCache_.push_back(std::move(hit));
            ++state_.generation;
            cacheHit = true;
        }
        if (cacheHit) {
            // Notify after releasing mutex_; Notify() takes the same lock to copy callback.
        } else {
            state_.busy = true;
            state_.job = YoutubeJobKind::Search;
            state_.status = L"Searching...";
            state_.entries.clear();
            state_.probe.reset();
            state_.probeEntryId = 0;
            state_.searchPage = 0;
            state_.searchPageCount = 1;
            state_.searchIsPaged = false;
            ++state_.generation;
        }
    }
    Notify();
    if (cacheHit) return;

    worker_ = std::jthread([this, query = std::move(query)](std::stop_token stop) {
        RunSearch(stop, query);
    });
}

bool YoutubeService::SetSearchPage(std::size_t page) {
    std::scoped_lock lock(mutex_);
    if (!state_.searchIsPaged || state_.searchPageCount == 0) return false;
    if (page >= state_.searchPageCount) return false;
    if (page == state_.searchPage) return true;
    state_.searchPage = page;
    state_.status = std::to_wstring(state_.entries.size()) + L" result(s) · page " +
                    std::to_wstring(page + 1) + L"/" +
                    std::to_wstring(state_.searchPageCount);
    ++state_.generation;
    return true;
}

void YoutubeService::Probe(std::uint64_t entryId) {
    JoinWorker();
    {
        std::scoped_lock lock(mutex_);
        const auto found = std::find_if(
            state_.entries.begin(), state_.entries.end(),
            [entryId](const YoutubeEntry& entry) { return entry.id == entryId; });
        if (found == state_.entries.end()) return;
        state_.busy = true;
        state_.job = YoutubeJobKind::Probe;
        state_.status = L"Probing...";
        state_.probe.reset();
        state_.probeEntryId = entryId;
        ++state_.generation;
    }
    Notify();
    worker_ = std::jthread([this, entryId](std::stop_token stop) { RunProbe(stop, entryId); });
}

void YoutubeService::Download(std::uint64_t entryId, std::filesystem::path musicRoot,
                              YoutubeDownloadSelection selection) {
    JoinWorker();
    bool start = false;
    {
        std::scoped_lock lock(mutex_);
        auto found = std::find_if(state_.entries.begin(), state_.entries.end(),
                                  [entryId](const YoutubeEntry& entry) {
                                      return entry.id == entryId;
                                  });
        if (found == state_.entries.end()) return;
        if (!found->localPath.empty() && detail::PathExistsFile(found->localPath)) {
            state_.status = L"Already downloaded";
            ++state_.generation;
        } else {
            found->downloading = true;
            found->failed = false;
            found->downloadProgress = 0.0F;
            state_.busy = true;
            state_.job = YoutubeJobKind::Download;
            state_.status = L"Downloading 0%";
            ++state_.generation;
            start = true;
        }
    }
    Notify();
    if (!start) return;

    worker_ = std::jthread([this, entryId, musicRoot = std::move(musicRoot),
                            selection = std::move(selection)](std::stop_token stop) mutable {
        RunDownload(stop, entryId, musicRoot, std::move(selection));
    });
}

YoutubeSnapshot YoutubeService::Snapshot() const {
    std::scoped_lock lock(mutex_);
    return state_;
}

} // namespace rivan::youtube
