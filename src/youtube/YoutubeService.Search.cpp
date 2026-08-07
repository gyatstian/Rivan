// YoutubeService.Search.cpp
#include "YoutubeService.Internal.h"

#include <algorithm>
#include <charconv>
#include <cwctype>
#include <utility>

namespace rivan::youtube::detail {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::uint64_t HashText(std::wstring_view text) noexcept {
    std::uint64_t hash = kFnvOffset;
    for (wchar_t ch : text) {
        ch = static_cast<wchar_t>(std::towlower(ch));
        const auto value = static_cast<std::uint32_t>(ch);
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= static_cast<unsigned char>((value >> shift) & 0xffu);
            hash *= kFnvPrime;
        }
    }
    return hash == 0 ? 1 : hash;
}

bool LooksLikeYoutubeVideoId(std::wstring_view id) noexcept {
    if (id.size() != 11) return false;
    if (id.rfind(L"UC", 0) == 0 || id.rfind(L"PL", 0) == 0 || id.rfind(L"VL", 0) == 0 ||
        id.rfind(L"RD", 0) == 0 || id.rfind(L"OL", 0) == 0) {
        return false;
    }
    for (const wchar_t ch : id) {
        if (!((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z') ||
              (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_')) {
            return false;
        }
    }
    return true;
}

double ParseDuration(std::string_view text) {
    if (text.empty() || text == "NA" || text == "None") return 0.0;
    double value = 0.0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || value < 0.0) return 0.0;
    return value;
}

std::optional<YoutubeEntry> TryParseListingLine(std::string_view line) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) return std::nullopt;

    constexpr std::string_view kSep = "|||";
    const auto p1 = line.find(kSep);
    if (p1 == std::string_view::npos) return std::nullopt;
    const auto p2 = line.find(kSep, p1 + kSep.size());
    if (p2 == std::string_view::npos) return std::nullopt;

    const auto id = line.substr(0, p1);
    const auto title = line.substr(p1 + kSep.size(), p2 - (p1 + kSep.size()));
    const auto duration = line.substr(p2 + kSep.size());
    if (id.empty() || title.empty()) return std::nullopt;

    YoutubeEntry entry;
    entry.videoId = Utf8ToWide(id);
    entry.title = Utf8ToWide(title);
    if (!LooksLikeYoutubeVideoId(entry.videoId)) return std::nullopt;
    if (entry.title.empty() || entry.title == L"NA") return std::nullopt;
    entry.durationSeconds = ParseDuration(duration);
    entry.webpageUrl = L"https://www.youtube.com/watch?v=" + entry.videoId;
    entry.id = HashText(entry.videoId);
    return entry;
}

std::vector<YoutubeEntry> ParseListing(const std::string& stdoutText) {
    std::vector<YoutubeEntry> entries;
    std::size_t lineStart = 0;
    while (lineStart < stdoutText.size()) {
        std::size_t lineEnd = stdoutText.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = stdoutText.size();
        std::string_view line(stdoutText.data() + lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 1;
        if (auto entry = TryParseListingLine(line)) {
            entries.push_back(std::move(*entry));
            if (entries.size() >= kSearchFetchCount) break;
        }
    }
    return entries;
}

} // namespace rivan::youtube::detail

namespace rivan::youtube {

void YoutubeService::RunSearch(std::stop_token stop, std::wstring query) {
    const auto ytDlp = LocateYtDlp();
    if (!ytDlp) {
        {
            std::scoped_lock lock(mutex_);
            state_.busy = false;
            state_.job = YoutubeJobKind::Idle;
            state_.status = L"yt-dlp not installed — use Preferences → Online";
            WriteToolFlagsLocked();
            ++state_.generation;
        }
        Notify();
        return;
    }

    const bool url = LooksLikeUrl(query);
    const std::wstring cacheKey = url ? query : (L"y:" + query);

    const auto publishEntries = [this, url](
                                    const std::vector<YoutubeEntry>& entries, bool searching) {
        {
            std::scoped_lock lock(mutex_);
            state_.entries = entries;
            state_.searchPage = 0;
            if (!url && !state_.entries.empty()) {
                state_.searchIsPaged = true;
                state_.searchPageCount =
                    (state_.entries.size() + kSearchPageSize - 1) / kSearchPageSize;
                if (state_.searchPageCount == 0) state_.searchPageCount = 1;
            } else {
                state_.searchIsPaged = false;
                state_.searchPageCount = 1;
            }
            if (searching) {
                const std::wstring prefix = url ? L"Resolving... " : L"Searching... ";
                state_.status = prefix + std::to_wstring(state_.entries.size()) + L" result(s)";
            } else if (state_.entries.empty()) {
                state_.status = L"No results";
            } else if (state_.searchIsPaged) {
                state_.status = std::to_wstring(state_.entries.size()) + L" result(s) · page 1/" +
                                std::to_wstring(state_.searchPageCount);
            } else {
                state_.status = std::to_wstring(state_.entries.size()) + L" result(s)";
            }
            ++state_.generation;
        }
        Notify();
    };

    const auto runListing = [&](const std::wstring& target, std::vector<YoutubeEntry>& outEntries,
                                std::string& output, std::string& error, DWORD& exitCode,
                                bool videosOnly, std::size_t playlistEnd) -> bool {
        std::wstring arguments =
            L"--ignore-config --no-cache-dir --socket-timeout 12 "
            L"--flat-playlist --no-warnings --no-playlist-reverse --newline ";
        if (playlistEnd > 0) {
            arguments += L"--playlist-end " + std::to_wstring(playlistEnd) + L" ";
        }
        arguments += L"--print %(id)s|||%(title)s|||%(duration)s " + detail::QuoteArg(target);

        output.clear();
        error.clear();
        exitCode = 1;
        outEntries.clear();

        const auto accept = [&](const YoutubeEntry& entry) {
            if (videosOnly && !detail::LooksLikeYoutubeVideoId(entry.videoId)) return false;
            for (const auto& existing : outEntries) {
                if (existing.videoId == entry.videoId) return false;
            }
            return outEntries.size() < kSearchFetchCount;
        };

        const bool ran = detail::RunProcessCapture(
            *ytDlp, arguments, stop, output, error, &exitCode,
            [&](std::string_view line) {
                if (stop.stop_requested()) return;
                if (auto entry = detail::TryParseListingLine(line)) {
                    if (!accept(*entry)) return;
                    outEntries.push_back(std::move(*entry));
                    publishEntries(outEntries, true);
                }
            });

        if (ran && outEntries.empty()) {
            for (auto& entry : detail::ParseListing(output)) {
                if (!accept(entry)) continue;
                outEntries.push_back(std::move(entry));
                if (outEntries.size() >= kSearchFetchCount) break;
            }
        }
        return ran;
    };

    std::string lastOutput;
    std::string lastError;
    DWORD lastExit = 1;
    bool anyRan = false;
    std::vector<YoutubeEntry> best;

    if (url) {
        anyRan = runListing(query, best, lastOutput, lastError, lastExit, false, 0);
        if (!best.empty()) {
            publishEntries(best, false);
        }
    } else {
        constexpr std::size_t kStages[] = {1, kSearchFetchCount};
        for (const std::size_t count : kStages) {
            if (stop.stop_requested()) break;
            std::vector<YoutubeEntry> stage;
            std::string output;
            std::string error;
            DWORD exitCode = 1;
            const std::wstring target = L"ytsearch" + std::to_wstring(count) + L":" + query;
            const bool ran = runListing(target, stage, output, error, exitCode, false, 0);
            anyRan = anyRan || ran;
            lastOutput = std::move(output);
            lastError = std::move(error);
            lastExit = exitCode;
            if (!stage.empty()) {
                best = std::move(stage);
                publishEntries(best, count < kSearchFetchCount);
            }
            if (ran && best.size() < count) break;
        }
    }

    if (stop.stop_requested()) {
        {
            std::scoped_lock lock(mutex_);
            state_.busy = false;
            state_.job = YoutubeJobKind::Idle;
            if (state_.entries.empty()) {
                state_.status = L"Cancelled";
            } else if (state_.searchIsPaged) {
                state_.status = std::to_wstring(state_.entries.size()) + L" result(s) · page " +
                                std::to_wstring(state_.searchPage + 1) + L"/" +
                                std::to_wstring(state_.searchPageCount) + L" (cancelled)";
            } else {
                state_.status = std::to_wstring(state_.entries.size()) + L" result(s) (cancelled)";
            }
            ++state_.generation;
        }
        Notify();
        return;
    }

    {
        std::scoped_lock lock(mutex_);
        state_.busy = false;
        state_.job = YoutubeJobKind::Idle;
        if (!best.empty()) state_.entries = std::move(best);
        state_.searchPage = 0;
        if (!url && !state_.entries.empty()) {
            state_.searchIsPaged = true;
            state_.searchPageCount =
                (state_.entries.size() + kSearchPageSize - 1) / kSearchPageSize;
            if (state_.searchPageCount == 0) state_.searchPageCount = 1;
        } else {
            state_.searchIsPaged = false;
            state_.searchPageCount = 1;
        }
        if (!anyRan) {
            const auto errorDetail =
                detail::TailWide(lastOutput.empty() ? lastError : lastOutput, 120);
            if (state_.entries.empty()) {
                state_.status = errorDetail.empty() ? L"Search failed"
                                                    : (L"Search failed: " + errorDetail);
            } else {
                state_.status = std::to_wstring(state_.entries.size()) + L" result(s)";
            }
        } else if (state_.entries.empty()) {
            const auto errorDetail = detail::TailWide(lastOutput, 120);
            state_.status = lastExit == 0
                                ? L"No results"
                                : (errorDetail.empty() ? L"Search failed (yt-dlp error)"
                                                       : (L"Search failed: " + errorDetail));
        } else if (state_.searchIsPaged) {
            state_.status = std::to_wstring(state_.entries.size()) + L" result(s) · page 1/" +
                            std::to_wstring(state_.searchPageCount);
        } else {
            state_.status = std::to_wstring(state_.entries.size()) + L" result(s)";
        }
        if (!url && !state_.entries.empty()) StoreSearchCacheLocked(cacheKey, state_.entries);
        ++state_.generation;
    }
    Notify();
}

} // namespace rivan::youtube
