// LyricsService.cpp
// Service core: request lifecycle, background worker loop, snapshot publication,
// state settings, and lifetime management.
#include "LyricsService.h"

#include <system_error>
#include <utility>

namespace rivan::lyrics {

std::wstring LyricsDocument::PlainText() const {
    std::wstring result;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) result += L'\n';
        result += lines[i].text;
    }
    return result;
}

LyricsService::LyricsService(std::filesystem::path cacheDirectory)
    : cacheDirectory_(std::move(cacheDirectory)), worker_([this](std::stop_token stop) { Worker(stop); }) {}

LyricsService::~LyricsService() {
    Shutdown();
}

void LyricsService::Shutdown() {
    worker_.request_stop();
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void LyricsService::SetNotify(Notify notify) {
    std::scoped_lock lock(mutex_);
    notify_ = std::move(notify);
}

void LyricsService::SetCacheEnabled(bool enabled) {
    std::scoped_lock lock(mutex_);
    cacheEnabled_ = enabled;
}

void LyricsService::SetOnlineEnabled(bool enabled) {
    std::scoped_lock lock(mutex_);
    onlineEnabled_ = enabled;
}

void LyricsService::SetFakeTimestampsEnabled(bool enabled) {
    std::scoped_lock lock(mutex_);
    fakeTimestampsEnabled_ = enabled;
}

void LyricsService::SetDisabledSongs(const std::unordered_set<std::wstring>& songs) {
    std::scoped_lock lock(mutex_);
    disabledSongs_ = songs;
}

void LyricsService::Request(std::uint64_t trackId, std::wstring title, std::wstring artist,
                            std::wstring album, double durationSeconds,
                            std::filesystem::path filePath) {
    std::scoped_lock lock(mutex_);
    ++generation_;
    pending_ = RequestData{trackId, std::move(title), std::move(artist), std::move(album),
                           durationSeconds, std::move(filePath), generation_};
    snapshot_ = LyricsSnapshot{};
    snapshot_.trackId = trackId;
    snapshot_.loading = true;
    snapshot_.status = L"Loading lyrics...";
    PublishSnapshotLocked();
    condition_.notify_one();
}

void LyricsService::Reset() {
    std::scoped_lock lock(mutex_);
    ++generation_;
    pending_.reset();
    snapshot_ = LyricsSnapshot{};
    snapshot_.status = L"No lyrics available";
    PublishSnapshotLocked();
}

LyricsSnapshot LyricsService::Snapshot() const {
    std::scoped_lock lock(mutex_);
    return snapshot_;
}

void LyricsService::NotifyTrackRenamed(std::uint64_t oldTrackId, std::uint64_t newTrackId,
                                       const std::filesystem::path& oldPath,
                                       const std::filesystem::path& newPath) {
    {
        std::scoped_lock lock(mutex_);
        if (pending_ && pending_->trackId == oldTrackId) {
            pending_->trackId = newTrackId;
            pending_->filePath = newPath;
        }
        if (snapshot_.trackId == oldTrackId) snapshot_.trackId = newTrackId;
    }
    (void)RetargetLyricsFilePaths(cacheDirectory_, oldPath, newPath);
}

void LyricsService::Publish(const RequestData& request, LyricsDocument document, std::wstring status) {
    Notify notify;
    {
        std::scoped_lock lock(mutex_);
        if (request.generation != generation_) return;
        snapshot_.trackId = request.trackId;
        snapshot_.loading = false;
        snapshot_.available = !document.Empty();
        snapshot_.document = std::move(document);
        snapshot_.status = std::move(status);
        PublishSnapshotLocked();
        notify = notify_;
    }
    if (notify) notify();
}

void LyricsService::PublishSnapshotLocked() noexcept {
    snapshot_.revision = publishedRevision_.load(std::memory_order_relaxed) + 1;
    publishedRevision_.store(snapshot_.revision, std::memory_order_release);
}

void LyricsService::Worker(std::stop_token stop) {
    while (!stop.stop_requested()) {
        RequestData request;
        bool haveRequest = false;
        try {
            bool useCache = false;
            bool useOnline = true;
            bool useFakeTimestamps = false;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, stop, [this] { return pending_.has_value(); });
                if (stop.stop_requested()) return;
                request = std::move(*pending_);
                haveRequest = true;
                pending_.reset();
                useCache = cacheEnabled_;
                useOnline = onlineEnabled_;
                useFakeTimestamps = fakeTimestampsEnabled_;
            }
            // Publishes after optionally stamping generated timestamps. Generation is
            // in-memory only: the on-disk lyrics files always keep their original text.
            const auto publish = [this, &request, useFakeTimestamps](
                                     LyricsDocument document, std::wstring status) {
                if (useFakeTimestamps && !document.Empty()) {
                    document = WithFakeTimestamps(std::move(document));
                }
                Publish(request, std::move(document), std::move(status));
            };
            // Lyrics for this song disabled explicitly by the user: skip every source
            // (used when an online service returns wrong lyrics).
            if (!request.filePath.empty()) {
                bool disabled = false;
                {
                    std::scoped_lock lock(mutex_);
                    disabled = disabledSongs_.contains(NormalizedTrackPath(request.filePath));
                }
                if (disabled) {
                    publish(LyricsDocument{}, L"Lyrics disabled");
                    continue;
                }
            }
            // Local-first lookup order: the user's own lyrics files in the lyrics folder
            // win, then previously saved fetches, and only then are online services asked.
            // Unified lyrics files (fetched or user-authored) win over the legacy
            // fingerprint cache: they are human-readable, path-associated, and possibly
            // hand-edited. Loaded even when the fetch cache setting is disabled.
            if (auto document = LoadLyricsFiles(request)) {
                publish(std::move(*document), L"Lyrics");
                continue;
            }
            if (useCache) {
                if (auto document = LoadCache(request)) {
                    // Migrate legacy fingerprint-named cache files into the unified
                    // format so previously fetched lyrics become readable and
                    // path-associated like every other lyrics file.
                    if (!request.filePath.empty() &&
                        !SaveLyricsFile(cacheDirectory_, request.title,
                                        request.filePath, *document).empty()) {
                        std::error_code error;
                        std::filesystem::remove(CachePath(request), error);
                    }
                    publish(std::move(*document), L"Lyrics");
                    continue;
                }
            }
            if (!useOnline) {
                publish(LyricsDocument{}, L"No lyrics available");
                continue;
            }
            auto document = Fetch(request, stop);
            bool saveCache = false;
            {
                std::scoped_lock lock(mutex_);
                saveCache = cacheEnabled_;
            }
            if (document && saveCache) SaveCache(request, *document);
            publish(document ? std::move(*document) : LyricsDocument{},
                    document ? L"Lyrics" : L"No lyrics available");
        } catch (...) {
            if (haveRequest) {
                Publish(request, LyricsDocument{}, L"No lyrics available");
            }
        }
    }
}

} // namespace rivan::lyrics