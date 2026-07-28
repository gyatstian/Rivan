// YoutubeService.cpp
// Spawns yt-dlp for search/URL listing and audio extraction into musicRoot/Youtube.
#include "YoutubeService.h"

#include "../core/AppPaths.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Urlmon.h>

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cwctype>
#include <functional>
#include <string_view>
#include <system_error>
#include <utility>

#pragma comment(lib, "Urlmon.lib")

namespace rivan::youtube {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

[[nodiscard]] std::uint64_t HashText(std::wstring_view text) noexcept {
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

[[nodiscard]] std::wstring Trim(std::wstring value) {
    while (!value.empty() && (value.front() == L' ' || value.front() == L'\t' ||
                              value.front() == L'\r' || value.front() == L'\n')) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == L' ' || value.back() == L'\t' ||
                              value.back() == L'\r' || value.back() == L'\n')) {
        value.pop_back();
    }
    return value;
}

[[nodiscard]] std::wstring Lower(std::wstring value) {
    for (auto& ch : value) ch = static_cast<wchar_t>(std::towlower(ch));
    return value;
}

[[nodiscard]] bool PathExistsFile(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
}

// Optional line callback while process runs (UTF-8 lines, no trailing CR/LF).
using ProcessLineCallback = std::function<void(std::string_view line)>;

// Child env with PYTHONUNBUFFERED so yt-dlp (Python) flushes each --print line to the pipe.
[[nodiscard]] std::wstring BuildUnbufferedEnvironment() {
    const wchar_t* parent = GetEnvironmentStringsW();
    std::wstring block;
    if (parent) {
        const wchar_t* cursor = parent;
        while (*cursor != L'\0') {
            const std::wstring entry = cursor;
            cursor += entry.size() + 1;
            // Drop keys we override (case-insensitive prefix match).
            if (entry.size() >= 17 &&
                _wcsnicmp(entry.c_str(), L"PYTHONUNBUFFERED=", 17) == 0) {
                continue;
            }
            if (entry.size() >= 16 &&
                _wcsnicmp(entry.c_str(), L"PYTHONIOENCODING=", 16) == 0) {
                continue;
            }
            block.append(entry);
            block.push_back(L'\0');
        }
        FreeEnvironmentStringsW(const_cast<wchar_t*>(parent));
    }
    block += L"PYTHONUNBUFFERED=1";
    block.push_back(L'\0');
    block += L"PYTHONIOENCODING=utf-8";
    block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

// Runs a process and captures combined stdout (UTF-8). Returns false on spawn failure.
// When onLine is set, each complete line is delivered as it arrives (for progress).
[[nodiscard]] bool RunProcessCapture(const std::filesystem::path& exe,
                                     const std::wstring& arguments,
                                     std::stop_token stop,
                                     std::string& stdoutText,
                                     std::string& errorText,
                                     DWORD* exitCode,
                                     ProcessLineCallback onLine = {}) {
    stdoutText.clear();
    errorText.clear();
    if (exitCode) *exitCode = static_cast<DWORD>(-1);

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
        errorText = "Unable to create pipe for yt-dlp";
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    // Small write buffer so the child is less likely to batch many lines before we read.
    DWORD pipeMode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    // Non-blocking read side is optional; keep blocking reads for simplicity.
    (void)pipeMode;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::wstring commandLine = L"\"" + exe.wstring() + L"\" " + arguments;
    std::wstring environment = BuildUnbufferedEnvironment();
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        nullptr, commandLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(), nullptr, &startup,
        &process);
    CloseHandle(writePipe);
    writePipe = nullptr;

    if (!created) {
        CloseHandle(readPipe);
        errorText = "Unable to start yt-dlp";
        return false;
    }

    std::string buffer;
    buffer.reserve(4096);
    std::string lineCarry;
    lineCarry.reserve(256);
    char chunk[1024];

    const auto feedLines = [&](std::string_view data) {
        if (!onLine) return;
        for (const char ch : data) {
            if (ch == '\n' || ch == '\r') {
                if (!lineCarry.empty()) {
                    onLine(lineCarry);
                    lineCarry.clear();
                }
            } else {
                lineCarry.push_back(ch);
            }
        }
    };

    for (;;) {
        if (stop.stop_requested()) {
            TerminateProcess(process.hProcess, 1);
            break;
        }
        DWORD available = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr)) {
            break;
        }
        if (available == 0) {
            const DWORD wait = WaitForSingleObject(process.hProcess, 15);
            if (wait == WAIT_OBJECT_0) {
                // Drain remaining.
                DWORD read = 0;
                while (ReadFile(readPipe, chunk, sizeof(chunk), &read, nullptr) && read > 0) {
                    buffer.append(chunk, chunk + read);
                    feedLines(std::string_view(chunk, read));
                }
                break;
            }
            continue;
        }
        DWORD read = 0;
        if (!ReadFile(readPipe, chunk, sizeof(chunk), &read, nullptr) || read == 0) break;
        buffer.append(chunk, chunk + read);
        feedLines(std::string_view(chunk, read));
        if (buffer.size() > 8 * 1024 * 1024) break;
    }
    if (onLine && !lineCarry.empty()) {
        onLine(lineCarry);
        lineCarry.clear();
    }

    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    if (exitCode) *exitCode = code;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(readPipe);

    stdoutText = std::move(buffer);
    if (stop.stop_requested()) {
        errorText = "Cancelled";
        return false;
    }
    return true;
}

// Parses yt-dlp progress lines like "[download]  45.2% of 3.50MiB at ...".
// Returns true when percent was extracted (0..100).
[[nodiscard]] bool ParseDownloadPercent(std::string_view line, float& percentOut) {
    const auto pos = line.find('%');
    if (pos == std::string_view::npos || pos == 0) return false;
    std::size_t start = pos;
    while (start > 0) {
        const char ch = line[start - 1];
        if ((ch >= '0' && ch <= '9') || ch == '.' || ch == ' ') {
            --start;
            continue;
        }
        break;
    }
    while (start < pos && line[start] == ' ') ++start;
    if (start >= pos) return false;
    float value = 0.0F;
    const auto [end, ec] =
        std::from_chars(line.data() + start, line.data() + pos, value);
    if (ec != std::errc{} || end != line.data() + pos) return false;
    if (value < 0.0F) value = 0.0F;
    if (value > 100.0F) value = 100.0F;
    percentOut = value;
    return true;
}

[[nodiscard]] bool LineLooksLikePostprocess(std::string_view line) {
    return line.find("[ExtractAudio]") != std::string_view::npos ||
           line.find("[ffmpeg]") != std::string_view::npos ||
           line.find("Destination:") != std::string_view::npos ||
           line.find("Deleting original file") != std::string_view::npos;
}

[[nodiscard]] std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), required);
    return result;
}

[[nodiscard]] std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0,
                                             nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), required, nullptr, nullptr);
    return result;
}

// Percent-encode for music.youtube.com/search?q= (UTF-8 bytes).
[[nodiscard]] std::wstring UrlEncodeQuery(std::wstring_view text) {
    const std::string utf8 = WideToUtf8(text);
    std::wstring out;
    out.reserve(utf8.size() * 3);
    for (const unsigned char ch : utf8) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out.push_back(static_cast<wchar_t>(ch));
        } else if (ch == ' ') {
            out.push_back(L'+');
        } else {
            static constexpr wchar_t kHex[] = L"0123456789ABCDEF";
            out.push_back(L'%');
            out.push_back(kHex[(ch >> 4) & 0xF]);
            out.push_back(kHex[ch & 0xF]);
        }
    }
    return out;
}

// YouTube video ids are 11 chars; YTM search also returns channels/albums/playlists.
// Reject channel (UC…), playlist (PL…/VL…), album (MPRE…) style ids.
[[nodiscard]] bool LooksLikeYoutubeVideoId(std::wstring_view id) noexcept {
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

[[nodiscard]] double ParseDuration(std::string_view text) {
    if (text.empty() || text == "NA" || text == "None") return 0.0;
    double value = 0.0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || value < 0.0) return 0.0;
    return value;
}

// Fields separated by ||| (avoids Windows shell/tab escaping issues).
[[nodiscard]] std::optional<YoutubeEntry> TryParseListingLine(std::string_view line) {
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
    // Drop non-playable rows early (channels/albums/playlists from YTM search).
    if (!LooksLikeYoutubeVideoId(entry.videoId)) return std::nullopt;
    if (entry.title.empty() || entry.title == L"NA") return std::nullopt;
    entry.durationSeconds = ParseDuration(duration);
    entry.webpageUrl = L"https://www.youtube.com/watch?v=" + entry.videoId;
    entry.id = HashText(entry.videoId);
    return entry;
}

[[nodiscard]] std::vector<YoutubeEntry> ParseListing(const std::string& stdoutText) {
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

[[nodiscard]] std::wstring TailWide(const std::string& text, std::size_t maxChars) {
    std::wstring wide = Utf8ToWide(text);
    // Prefer last non-empty line for yt-dlp errors.
    while (!wide.empty() && (wide.back() == L'\n' || wide.back() == L'\r')) wide.pop_back();
    const auto pos = wide.find_last_of(L'\n');
    if (pos != std::wstring::npos) wide = wide.substr(pos + 1);
    if (wide.size() > maxChars) wide = L"..." + wide.substr(wide.size() - maxChars);
    return wide;
}

[[nodiscard]] std::wstring QuoteArg(std::wstring_view value) {
    std::wstring out = L"\"";
    for (const wchar_t ch : value) {
        if (ch == L'"') out += L"\\\"";
        else out.push_back(ch);
    }
    out.push_back(L'"');
    return out;
}

[[nodiscard]] std::wstring FfmpegLocationArg() {
    const auto ffmpeg = YoutubeService::LocateFfmpeg();
    if (!ffmpeg) return {};
    const auto dir = ffmpeg->parent_path();
    return L" --ffmpeg-location " + QuoteArg(dir.empty() ? ffmpeg->wstring() : dir.wstring());
}

[[nodiscard]] std::optional<std::filesystem::path> FindDownloadedFile(
    const std::filesystem::path& directory, std::wstring_view videoId) {
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) return std::nullopt;
    const std::wstring id(videoId);
    const std::wstring needle = L"[" + id + L"]";
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const auto name = entry.path().filename().wstring();
        const auto stem = entry.path().stem().wstring();
        const auto ext = Lower(entry.path().extension().wstring());
        const bool media = ext == L".mp3" || ext == L".m4a" || ext == L".opus" ||
                           ext == L".webm" || ext == L".wav" || ext == L".flac" ||
                           ext == L".ogg" || ext == L".mp4" || ext == L".m4v";
        if (!media) continue;
        if (stem == id || name.find(needle) != std::wstring::npos ||
            name.find(id) != std::wstring::npos) {
            return entry.path();
        }
    }
    return std::nullopt;
}

// Rename a freshly downloaded "Title [videoId].ext" file to "Title.ext" so the user
// never sees the 11-char video id. yt-dlp still writes the [id] form (kept for reliable
// location); this strips it afterward. Returns the final path (unchanged on any failure
// or if no [id] suffix is present). Collisions get a " (n)" suffix.
[[nodiscard]] std::filesystem::path StripIdSuffixAndRename(const std::filesystem::path& file,
                                                          std::wstring_view videoId) {
    if (videoId.empty()) return file;
    std::wstring stem = file.stem().wstring();
    const std::wstring suffix = L" [" + std::wstring(videoId) + L"]";
    if (stem.size() <= suffix.size() ||
        stem.compare(stem.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return file;  // No trailing [id] to strip.
    }
    stem.resize(stem.size() - suffix.size());
    stem = Trim(std::move(stem));
    if (stem.empty()) return file;
    const auto ext = file.extension().wstring();
    const auto dir = file.parent_path();
    std::filesystem::path target = dir / (stem + ext);
    if (target == file) return file;
    for (int n = 2; PathExistsFile(target); ++n) {
        if (n > 9999) return file;
        target = dir / (stem + L" (" + std::to_wstring(n) + L")" + ext);
    }
    std::error_code ec;
    std::filesystem::rename(file, target, ec);
    if (ec) return file;
    return target;
}

} // namespace

namespace {

[[nodiscard]] bool DownloadUrlToFile(const wchar_t* url, const std::filesystem::path& dest,
                                      std::wstring& error) {
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    if (ec) {
        error = L"Unable to create tools directory";
        return false;
    }
    const auto temp = dest.wstring() + L".partial";
    DeleteFileW(temp.c_str());
    const HRESULT hr = URLDownloadToFileW(nullptr, url, temp.c_str(), 0, nullptr);
    if (FAILED(hr)) {
        error = L"Download failed (0x" + std::to_wstring(static_cast<unsigned long>(hr)) + L")";
        DeleteFileW(temp.c_str());
        return false;
    }
    std::filesystem::rename(temp, dest, ec);
    if (ec) {
        std::filesystem::remove(dest, ec);
        ec.clear();
        std::filesystem::rename(temp, dest, ec);
        if (ec) {
            error = L"Unable to place downloaded file";
            DeleteFileW(temp.c_str());
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<std::filesystem::path> FindFileRecursive(
    const std::filesystem::path& root, const wchar_t* fileName) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return std::nullopt;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (_wcsicmp(it->path().filename().c_str(), fileName) == 0) return it->path();
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::filesystem::path> SystemTarPath() {
    std::wstring directory(MAX_PATH, L'\0');
    const UINT length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    if (length == 0 || length >= directory.size()) return std::nullopt;
    directory.resize(length);
    const auto tar = std::filesystem::path(directory) / L"tar.exe";
    return PathExistsFile(tar) ? std::optional{tar} : std::nullopt;
}

} // namespace

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
    if (PathExistsFile(tools)) return tools;
    return std::nullopt;
}

std::optional<std::filesystem::path> YoutubeService::LocateFfmpeg() {
    const auto tools = ToolsDirectory() / L"ffmpeg.exe";
    if (PathExistsFile(tools)) return tools;
    return std::nullopt;
}

bool YoutubeService::LooksLikeUrl(std::wstring_view text) noexcept {
    auto trimmed = Trim(std::wstring(text));
    if (trimmed.empty()) return false;
    const auto lower = Lower(trimmed);
    if (lower.rfind(L"http://", 0) == 0 || lower.rfind(L"https://", 0) == 0) return true;
    if (lower.find(L"music.youtube.com") != std::wstring::npos) return true;
    if (lower.find(L"youtube.com") != std::wstring::npos) return true;
    if (lower.find(L"youtu.be") != std::wstring::npos) return true;
    return false;
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

void YoutubeService::Warm() {
    {
        std::scoped_lock lock(mutex_);
        if (state_.busy) return;
        if (!LocateYtDlp().has_value()) return;
    }
    // Reap a finished worker without cancelling an active job (busy already checked).
    if (worker_.joinable()) {
        std::scoped_lock lock(mutex_);
        if (state_.busy) return;
    }
    JoinWorker();
    {
        std::scoped_lock lock(mutex_);
        if (state_.busy) return;
    }
    worker_ = std::jthread([this](std::stop_token stop) { RunWarm(stop); });
}

void YoutubeService::RunWarm(std::stop_token stop) {
    // Touch yt-dlp.exe into the OS page cache so the first real search avoids cold I/O.
    const auto ytDlp = LocateYtDlp();
    if (!ytDlp || stop.stop_requested()) return;
    std::string output;
    std::string error;
    DWORD exitCode = 1;
    (void)RunProcessCapture(*ytDlp, L"--version --no-warnings", stop, output, error, &exitCode);
}

void YoutubeService::InstallTool(YoutubeTool tool) {
    JoinWorker();
    bool start = false;
    {
        std::scoped_lock lock(mutex_);
        WriteToolFlagsLocked();
        if (tool == YoutubeTool::YtDlp && state_.ytDlpInstalled) {
            state_.status = L"yt-dlp already installed";
            ++state_.generation;
        } else if (tool == YoutubeTool::Ffmpeg && state_.ffmpegInstalled) {
            state_.status = L"ffmpeg already installed";
            ++state_.generation;
        } else {
            state_.busy = true;
            state_.job = YoutubeJobKind::Install;
            state_.installingYtDlp = tool == YoutubeTool::YtDlp;
            state_.installingFfmpeg = tool == YoutubeTool::Ffmpeg;
            state_.status = tool == YoutubeTool::YtDlp ? L"Installing yt-dlp..."
                                                       : L"Installing ffmpeg...";
            ++state_.generation;
            start = true;
        }
    }
    Notify();
    if (!start) return;
    worker_ = std::jthread([this, tool](std::stop_token stop) { RunInstall(stop, tool); });
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
        [&query](const CachedSearch& c) { return c.query == query; });
    if (existing != searchCache_.end()) {
        existing->entries = std::move(entries);
        CachedSearch hit = std::move(*existing);
        searchCache_.erase(existing);
        searchCache_.push_back(std::move(hit));
    } else {
        if (searchCache_.size() >= kSearchCacheMax) {
            searchCache_.erase(searchCache_.begin());
        }
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
    if (state_.status == L"Searching..." || state_.status == L"Downloading..." ||
        state_.status.rfind(L"Downloading ", 0) == 0 || state_.status == L"Converting..." ||
        state_.status == L"Resolving..." || state_.status == L"Installing yt-dlp..." ||
        state_.status == L"Installing ffmpeg...") {
        state_.status = L"Cancelled";
    }
    WriteToolFlagsLocked();
    ++state_.generation;
}

void YoutubeService::SubmitQuery(std::wstring query, bool musicSearch) {
    query = Trim(std::move(query));
    if (query.empty()) return;

    JoinWorker();
    const bool url = LooksLikeUrl(query);
    // Separate cache buckets so switching YT ↔ YTM does not reuse the wrong list.
    const std::wstring cacheKey =
        url ? query : ((musicSearch ? L"m:" : L"y:") + query);
    {
        std::scoped_lock lock(mutex_);
        // Cache hit: render the prior result set immediately, no yt-dlp spawn. Only
        // text searches are cached (URL resolves are cheap/unique enough to skip).
        if (!url) {
            const auto cached = std::find_if(
                searchCache_.begin(), searchCache_.end(),
                [&cacheKey](const CachedSearch& c) { return c.query == cacheKey; });
            if (cached != searchCache_.end() && !cached->entries.empty()) {
                state_.entries = cached->entries;
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
                // Promote to most-recently-used.
                CachedSearch hit = std::move(*cached);
                searchCache_.erase(cached);
                searchCache_.push_back(std::move(hit));
                ++state_.generation;
                Notify();
                return;
            }
        }
        state_.busy = true;
        state_.job = YoutubeJobKind::Search;
        state_.status = url ? L"Resolving..."
                            : (musicSearch ? L"Searching YouTube Music..." : L"Searching...");
        state_.entries.clear();
        state_.searchPage = 0;
        state_.searchPageCount = 1;
        state_.searchIsPaged = false;
        ++state_.generation;
    }
    Notify();

    worker_ = std::jthread([this, query = std::move(query), musicSearch](std::stop_token stop) {
        RunSearch(stop, query, musicSearch);
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

void YoutubeService::Download(std::uint64_t entryId, std::filesystem::path musicRoot,
                              int audioQuality, int downloadMode, int mp4VideoQuality,
                              bool musicSearch) {
    if (audioQuality < 0) audioQuality = 0;
    if (audioQuality > 9) audioQuality = 9;
    if (downloadMode < 0) downloadMode = 0;
    if (downloadMode > 2) downloadMode = 2;
    if (mp4VideoQuality < 0) mp4VideoQuality = 0;
    if (mp4VideoQuality > 5) mp4VideoQuality = 5;
    JoinWorker();
    bool start = false;
    {
        std::scoped_lock lock(mutex_);
        auto found = std::find_if(state_.entries.begin(), state_.entries.end(),
                                  [entryId](const YoutubeEntry& e) { return e.id == entryId; });
        if (found == state_.entries.end()) return;
        if (!found->localPath.empty() && PathExistsFile(found->localPath)) {
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

    worker_ = std::jthread([this, entryId, musicRoot = std::move(musicRoot), audioQuality,
                            downloadMode, mp4VideoQuality,
                            musicSearch](std::stop_token stop) {
        RunDownload(stop, entryId, musicRoot, audioQuality, downloadMode, mp4VideoQuality,
                    musicSearch);
    });
}

YoutubeSnapshot YoutubeService::Snapshot() const {
    std::scoped_lock lock(mutex_);
    return state_;
}

void YoutubeService::RunInstall(std::stop_token stop, YoutubeTool tool) {
    const auto tools = ToolsDirectory();
    std::error_code ec;
    std::filesystem::create_directories(tools, ec);
    std::wstring error;
    bool ok = false;

    if (stop.stop_requested()) {
        error = L"Cancelled";
    } else if (tool == YoutubeTool::YtDlp) {
        const auto dest = tools / L"yt-dlp.exe";
        ok = DownloadUrlToFile(
            L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe", dest,
            error);
        if (ok && !PathExistsFile(dest)) {
            ok = false;
            error = L"yt-dlp.exe missing after download";
        }
    } else {
        const auto zipPath = tools / L"ffmpeg-essentials.zip";
        ok = DownloadUrlToFile(
            L"https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip", zipPath,
            error);
        if (ok && !stop.stop_requested()) {
            const auto extractDir = tools / L"ffmpeg-extract";
            std::filesystem::remove_all(extractDir, ec);
            std::filesystem::create_directories(extractDir, ec);
            std::string out;
            std::string err;
            DWORD code = 1;
            const std::wstring args = L"-xf " + QuoteArg(zipPath.wstring()) + L" -C " +
                                       QuoteArg(extractDir.wstring());
            const auto tar = SystemTarPath();
            if (!tar || !RunProcessCapture(*tar, args, stop, out, err, &code) || code != 0) {
                ok = false;
                error = L"Unable to extract ffmpeg archive (Windows tar.exe is required)";
            } else if (auto found = FindFileRecursive(extractDir, L"ffmpeg.exe")) {
                const auto dest = tools / L"ffmpeg.exe";
                std::filesystem::copy_file(*found, dest,
                                           std::filesystem::copy_options::overwrite_existing,
                                           ec);
                if (ec || !PathExistsFile(dest)) {
                    ok = false;
                    error = L"Unable to copy ffmpeg.exe";
                }
            } else {
                ok = false;
                error = L"ffmpeg.exe not found in archive";
            }
            std::filesystem::remove_all(extractDir, ec);
            std::filesystem::remove(zipPath, ec);
        }
    }

    {
        std::scoped_lock lock(mutex_);
        state_.busy = false;
        state_.job = YoutubeJobKind::Idle;
        state_.installingYtDlp = false;
        state_.installingFfmpeg = false;
        WriteToolFlagsLocked();
        if (stop.stop_requested()) {
            state_.status = L"Cancelled";
        } else if (ok) {
            state_.status = tool == YoutubeTool::YtDlp ? L"yt-dlp installed"
                                                       : L"ffmpeg installed";
        } else {
            state_.status = error.empty() ? L"Install failed" : error;
        }
        ++state_.generation;
    }
    Notify();
}

void YoutubeService::RunSearch(std::stop_token stop, std::wstring query, bool musicSearch) {
    const auto ytDlp = LocateYtDlp();
    if (!ytDlp) {
        {
            std::scoped_lock lock(mutex_);
            state_.busy = false;
            state_.job = YoutubeJobKind::Idle;
            state_.status = L"yt-dlp not installed — use Preferences → General";
            WriteToolFlagsLocked();
            ++state_.generation;
        }
        Notify();
        return;
    }

    const bool url = LooksLikeUrl(query);
    const std::wstring cacheKey =
        url ? query : ((musicSearch ? L"m:" : L"y:") + query);

    // Publish a full replacement of the result list (used after each stage completes,
    // and per-line when the pipe is not fully buffered).
    const auto publishEntries = [this, url, musicSearch](const std::vector<YoutubeEntry>& entries,
                                                         bool searching) {
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
                const std::wstring prefix =
                    url ? L"Resolving... "
                        : (musicSearch ? L"Searching YouTube Music... " : L"Searching... ");
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
        // id|||title|||duration — flat listing without downloading media.
        // --ignore-config / --no-cache-dir: skip user plugins + disk cache overhead.
        // --socket-timeout: fail fast on hung network instead of long stalls.
        // --playlist-end: cap YTM search pages (mixed channel/album/video rows).
        std::wstring arguments =
            L"--ignore-config --no-cache-dir --socket-timeout 12 "
            L"--flat-playlist --no-warnings --no-playlist-reverse --newline ";
        if (playlistEnd > 0) {
            arguments += L"--playlist-end " + std::to_wstring(playlistEnd) + L" ";
        }
        arguments += L"--print %(id)s|||%(title)s|||%(duration)s " + QuoteArg(target);

        output.clear();
        error.clear();
        exitCode = 1;
        outEntries.clear();

        const auto accept = [&](const YoutubeEntry& entry) {
            if (videosOnly && !LooksLikeYoutubeVideoId(entry.videoId)) return false;
            for (const auto& existing : outEntries) {
                if (existing.videoId == entry.videoId) return false;
            }
            return outEntries.size() < kSearchFetchCount;
        };

        const bool ran = RunProcessCapture(
            *ytDlp, arguments, stop, output, error, &exitCode,
            [&](std::string_view line) {
                if (stop.stop_requested()) return;
                if (auto entry = TryParseListingLine(line)) {
                    if (!accept(*entry)) return;
                    outEntries.push_back(std::move(*entry));
                    // Live UI update when stdout is line-flushed.
                    publishEntries(outEntries, true);
                }
            });

        if (ran && outEntries.empty()) {
            for (auto& entry : ParseListing(output)) {
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
            if (musicSearch) {
                for (auto& entry : best) {
                    if (entry.videoId.empty()) continue;
                    entry.webpageUrl =
                        L"https://music.youtube.com/watch?v=" + entry.videoId;
                }
            }
            publishEntries(best, false);
        }
    } else if (musicSearch) {
        // youtube:music:search_url — catalog tracks with better covers/official audio.
        // Over-fetch playlist rows then keep video-id tracks only.
        const std::wstring target =
            L"https://music.youtube.com/search?q=" + UrlEncodeQuery(query);
        std::vector<YoutubeEntry> stage;
        anyRan = runListing(target, stage, lastOutput, lastError, lastExit, true,
                            kSearchFetchCount * 3);
        if (!stage.empty()) {
            for (auto& entry : stage) {
                entry.webpageUrl = L"https://music.youtube.com/watch?v=" + entry.videoId;
            }
            best = std::move(stage);
            publishEntries(best, false);
        }
    } else {
        // Staged ytsearch: first hit ASAP (ytsearch1), then fill the rest in one expand.
        constexpr std::size_t kStages[] = {1, kSearchFetchCount};
        for (const std::size_t count : kStages) {
            if (stop.stop_requested()) break;
            std::vector<YoutubeEntry> stage;
            std::string output;
            std::string error;
            DWORD exitCode = 1;
            const std::wstring target =
                L"ytsearch" + std::to_wstring(count) + L":" + query;
            const bool ran = runListing(target, stage, output, error, exitCode, false, 0);
            anyRan = anyRan || ran;
            lastOutput = std::move(output);
            lastError = std::move(error);
            lastExit = exitCode;
            if (!stage.empty()) {
                best = std::move(stage);
                publishEntries(best, count < kSearchFetchCount);
            }
            // Fewer results than requested → no point expanding further.
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
        if (!best.empty()) {
            state_.entries = std::move(best);
        }
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
            const auto detail = TailWide(lastOutput.empty() ? lastError : lastOutput, 120);
            if (state_.entries.empty()) {
                state_.status =
                    detail.empty() ? L"Search failed" : (L"Search failed: " + detail);
            } else {
                state_.status = std::to_wstring(state_.entries.size()) + L" result(s)";
            }
        } else if (state_.entries.empty()) {
            const auto detail = TailWide(lastOutput, 120);
            state_.status = lastExit == 0
                                ? L"No results"
                                : (detail.empty() ? L"Search failed (yt-dlp error)"
                                                  : (L"Search failed: " + detail));
        } else if (state_.searchIsPaged) {
            state_.status = std::to_wstring(state_.entries.size()) + L" result(s) · page 1/" +
                            std::to_wstring(state_.searchPageCount);
        } else {
            state_.status = std::to_wstring(state_.entries.size()) + L" result(s)";
        }
        // Cache completed text searches so an identical re-query renders instantly.
        if (!url && !state_.entries.empty()) StoreSearchCacheLocked(cacheKey, state_.entries);
        ++state_.generation;
    }
    Notify();
}

void YoutubeService::RunDownload(std::stop_token stop, std::uint64_t entryId,
                                 std::filesystem::path musicRoot, int audioQuality,
                                 int downloadMode, int mp4VideoQuality,
                                 bool musicSearch) {
    // Mode: 0 = MP3 (ffmpeg transcode), 1 = Original (native stream, no ffmpeg),
    //       2 = Video (mp4 + picked audio stream).
    const bool convertToMp3 = downloadMode == 0;
    YoutubeEntry target;
    bool missing = false;
    {
        std::scoped_lock lock(mutex_);
        const auto found = std::find_if(state_.entries.begin(), state_.entries.end(),
                                        [entryId](const YoutubeEntry& e) {
                                            return e.id == entryId;
                                        });
        if (found == state_.entries.end()) {
            state_.busy = false;
            state_.job = YoutubeJobKind::Idle;
            ++state_.generation;
            missing = true;
        } else {
            target = *found;
        }
    }
    if (missing) {
        Notify();
        return;
    }

    const auto ytDlp = LocateYtDlp();
    if (!ytDlp) {
        {
            std::scoped_lock lock(mutex_);
            state_.busy = false;
            state_.job = YoutubeJobKind::Idle;
            state_.status = L"yt-dlp not installed — use Preferences → General";
            WriteToolFlagsLocked();
            for (auto& entry : state_.entries) {
                if (entry.id == entryId) {
                    entry.downloading = false;
                    entry.failed = true;
                    entry.downloadProgress = -1.0F;
                }
            }
            ++state_.generation;
        }
        Notify();
        return;
    }

    // ffmpeg only required when extracting/encoding to mp3.
    if (convertToMp3 && !LocateFfmpeg()) {
        {
            std::scoped_lock lock(mutex_);
            state_.busy = false;
            state_.job = YoutubeJobKind::Idle;
            state_.status = L"ffmpeg required for audio extract — install in Preferences";
            WriteToolFlagsLocked();
            for (auto& entry : state_.entries) {
                if (entry.id == entryId) {
                    entry.downloading = false;
                    entry.failed = true;
                    entry.downloadProgress = -1.0F;
                }
            }
            ++state_.generation;
        }
        Notify();
        return;
    }

    std::error_code ec;
    const auto directory = DownloadDirectory(musicRoot);
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        {
            std::scoped_lock lock(mutex_);
            state_.busy = false;
            state_.job = YoutubeJobKind::Idle;
            state_.status = L"Unable to create Youtube folder";
            for (auto& entry : state_.entries) {
                if (entry.id == entryId) {
                    entry.downloading = false;
                    entry.failed = true;
                    entry.downloadProgress = -1.0F;
                }
            }
            ++state_.generation;
        }
        Notify();
        return;
    }

    // Title for humans; [id] suffix keeps FindDownloadedFile / re-download stable.
    // yt-dlp sanitizes filesystem-illegal characters in %(title)s.
    const std::wstring outputTemplate =
        (directory / L"%(title)s [%(id)s].%(ext)s").wstring();
    // Always download via www.youtube.com — music.youtube.com/watch hits EU consent
    // redirects that yt-dlp treats as generic/unsupported. Search still uses YTM.
    (void)musicSearch;
    const std::wstring url =
        target.videoId.empty()
            ? (target.webpageUrl.empty()
                   ? std::wstring{}
                   : target.webpageUrl)
            : (L"https://www.youtube.com/watch?v=" + target.videoId);
    if (url.empty()) {
        {
            std::scoped_lock lock(mutex_);
            state_.busy = false;
            state_.job = YoutubeJobKind::Idle;
            state_.status = L"Download failed: missing video id";
            for (auto& entry : state_.entries) {
                if (entry.id == entryId) {
                    entry.downloading = false;
                    entry.failed = true;
                    entry.downloadProgress = -1.0F;
                }
            }
            ++state_.generation;
        }
        Notify();
        return;
    }

    if (audioQuality < 0) audioQuality = 0;
    if (audioQuality > 9) audioQuality = 9;
    if (mp4VideoQuality < 0) mp4VideoQuality = 0;
    if (mp4VideoQuality > 5) mp4VideoQuality = 5;

    // Height ladder for MP4 video: 0=worst .. 5=best (capped).
    static constexpr int kMp4Heights[] = {144, 240, 360, 480, 720, 1080};
    const int height = kMp4Heights[mp4VideoQuality];

    // Shared audio tier: map the single 0-9 quality knob to best/mid/worst stream ranks
    // so Original and Video modes honor the same control as MP3 encode quality.
    // 0-2 = best stream, 3-6 = mid (~<=160 kb/s), 7-9 = worst stream.
    const int audioTier = audioQuality <= 2 ? 0 : (audioQuality <= 6 ? 1 : 2);

    // Concurrent fragments speed multi-part streams; --newline makes progress parseable.
    // MP3 (0): -x + mp3 encode (audio-quality 0=best/slow .. 9=fast/smaller); needs ffmpeg.
    // Original (1): grab the native audio stream as-is — no -x, no ffmpeg, instant.
    // Video (2): pick video height + audio tier; merge DASH when ffmpeg present.
    // --embed-thumbnail + --add-metadata: cover art for file preview (ID3 APIC / mp4 covr).
    const std::wstring embedArt =
        convertToMp3 ? L" --embed-thumbnail --add-metadata --convert-thumbnails jpg"
                     : L" --embed-thumbnail --add-metadata";
    std::wstring arguments;
    if (convertToMp3) {
        arguments = L"-x --audio-format mp3 --audio-quality " +
                    std::to_wstring(audioQuality) +
                    L" --concurrent-fragments 4 --newline --progress --no-warnings "
                    L"--no-playlist" +
                    embedArt + FfmpegLocationArg() + L" -o " +
                    QuoteArg(outputTemplate) + L" " + QuoteArg(url);
    } else if (downloadMode == 1) {
        // Original: native audio stream, no re-encode. Prefer m4a (universal) then any
        // best-audio. Tier picks best / mid (<=160 kb/s) / worst without transcoding.
        std::wstring format;
        if (audioTier == 0) {
            format = L"ba[ext=m4a]/ba/bestaudio";
        } else if (audioTier == 1) {
            format = L"ba[abr<=160][ext=m4a]/ba[abr<=160]/ba[ext=m4a]/bestaudio";
        } else {
            format = L"wa[ext=m4a]/wa/worstaudio/bestaudio";
        }
        // No --ffmpeg-location and no -x: yt-dlp writes the raw stream (.m4a/.opus/.webm).
        arguments = L"-f " + format +
                    L" --concurrent-fragments 4 --newline --progress --no-warnings "
                    L"--no-playlist" +
                    embedArt + L" -o " + QuoteArg(outputTemplate) + L" " + QuoteArg(url);
    } else if (LocateFfmpeg()) {
        // Video: full OR-chains only — never embed lone `audio/fallback` so `/` cannot
        // strip video. Audio tier: 0=best, 1=mid (~<=160 kb/s), 2=worst.
        const std::wstring h = std::to_wstring(height);
        std::wstring format;
        // Best tier only: force AAC re-encode at 256 kb/s during merge so mp4 matches
        // the MP3 path's bitrate. Empty for mid/worst tiers (stream-copied as-is).
        std::wstring audioReencode;
        if (audioTier == 0) {
            // Prefer the genuine best audio (opus ~160 kb/s beats m4a/AAC ~128 kb/s on
            // YouTube). Drop the [ext=m4a] preference so opus can win, then re-encode to
            // AAC 256k in the Merger — opus is not mp4-native, and this lands the same
            // ~246 kb/s tier the MP3 encode produces.
            format = L"bv*[ext=mp4][height<=" + h + L"]+ba/"
                     L"bv*[height<=" + h + L"]+ba/"
                     L"b[ext=mp4][height<=" + h + L"]/b";
            audioReencode = L" --postprocessor-args \"Merger:-c:a aac -b:a 256k\"";
        } else if (audioTier == 1) {
            format = L"bv*[ext=mp4][height<=" + h + L"]+ba[abr<=160][ext=m4a]/"
                     L"bv*[ext=mp4][height<=" + h + L"]+ba[abr<=160]/"
                     L"bv*[height<=" + h + L"]+ba[abr<=160]/"
                     L"bv*[ext=mp4][height<=" + h + L"]+ba/"
                     L"b[ext=mp4][height<=" + h + L"]/b";
        } else {
            format = L"bv*[ext=mp4][height<=" + h + L"]+wa[ext=m4a]/"
                     L"bv*[ext=mp4][height<=" + h + L"]+wa/"
                     L"bv*[height<=" + h + L"]+wa/"
                     L"bv*[ext=mp4][height<=" + h + L"]+ba/"
                     L"b[ext=mp4][height<=" + h + L"]/b";
        }
        arguments = L"-f " + format +
                    L" --merge-output-format mp4 --concurrent-fragments 4 --newline "
                    L"--progress --no-warnings --no-playlist" +
                    audioReencode + embedArt + FfmpegLocationArg() + L" -o " +
                    QuoteArg(outputTemplate) + L" " + QuoteArg(url);
    } else {
        // No merge: progressive only — video height capped; audio bundled, not pickable.
        arguments = L"-f b[ext=mp4][height<=" + std::to_wstring(height) +
                    L"]/best[ext=mp4][height<=" + std::to_wstring(height) +
                    L"]/best --concurrent-fragments 4 --newline --progress --no-warnings "
                    L"--no-playlist" +
                    embedArt + L" -o " + QuoteArg(outputTemplate) + L" " +
                    QuoteArg(url);
    }

    auto lastNotify = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    float lastReported = -1.0F;
    bool converting = false;

    const auto pushProgress = [&](float percent, bool force, bool isConvert) {
        const auto now = std::chrono::steady_clock::now();
        const bool enoughTime =
            force || (now - lastNotify) >= std::chrono::milliseconds(150);
        const bool enoughDelta =
            force || lastReported < 0.0F ||
            std::fabs(percent - lastReported) >= 0.5F || isConvert != converting;
        if (!enoughTime && !enoughDelta) return;
        lastNotify = now;
        lastReported = percent;
        converting = isConvert;
        {
            std::scoped_lock lock(mutex_);
            for (auto& entry : state_.entries) {
                if (entry.id != entryId) continue;
                entry.downloadProgress = percent;
                entry.downloading = true;
            }
            if (isConvert) {
                state_.status = convertToMp3 ? L"Converting..." : L"Merging...";
            } else {
                const int shown = static_cast<int>(percent + 0.5F);
                state_.status = L"Downloading " + std::to_wstring(shown) + L"%";
            }
            ++state_.generation;
        }
        Notify();
    };

    std::string output;
    std::string error;
    DWORD exitCode = 1;
    (void)RunProcessCapture(
        *ytDlp, arguments, stop, output, error, &exitCode,
        [&](std::string_view line) {
            if (stop.stop_requested()) return;
            float percent = 0.0F;
            if (ParseDownloadPercent(line, percent)) {
                // Cap network phase below 100 so convert can still show.
                if (percent > 99.0F) percent = 99.0F;
                pushProgress(percent, false, false);
            } else if (LineLooksLikePostprocess(line)) {
                pushProgress(99.0F, true, true);
            }
        });

    std::optional<std::filesystem::path> local;
    if (!stop.stop_requested()) {
        // Prefer deterministic id.ext paths, then title [id].ext scan.
        // MP3 (0) -> .mp3; Original (1) -> native .m4a/.opus; Video (2) -> .mp4.
        if (convertToMp3) {
            const auto mp3 = directory / (target.videoId + L".mp3");
            if (PathExistsFile(mp3)) local = mp3;
        } else if (downloadMode == 1) {
            for (const auto* ext : {L".m4a", L".opus", L".webm", L".mp3"}) {
                const auto candidate = directory / (target.videoId + ext);
                if (PathExistsFile(candidate)) {
                    local = candidate;
                    break;
                }
            }
        } else {
            const auto mp4 = directory / (target.videoId + L".mp4");
            if (PathExistsFile(mp4)) local = mp4;
        }
        if (!local) local = FindDownloadedFile(directory, target.videoId);
        if (!local) {
            for (const auto* ext :
                 {L".mp3", L".mp4", L".m4a", L".opus", L".webm", L".wav", L".flac", L".m4v"}) {
                const auto candidate = directory / (target.videoId + ext);
                if (PathExistsFile(candidate)) {
                    local = candidate;
                    break;
                }
            }
        }
    }

    {
        std::scoped_lock lock(mutex_);
        state_.busy = false;
        state_.job = YoutubeJobKind::Idle;
        for (auto& entry : state_.entries) {
            if (entry.id != entryId) continue;
            entry.downloading = false;
            if (local) {
                entry.localPath = *local;
                entry.failed = false;
                entry.downloadProgress = 100.0F;
                state_.status = L"Downloaded: " + local->filename().wstring();
            } else {
                entry.failed = true;
                entry.downloadProgress = -1.0F;
                if (stop.stop_requested()) {
                    state_.status = L"Cancelled";
                } else {
                    const auto detail = TailWide(output.empty() ? error : output, 140);
                    state_.status = detail.empty()
                                        ? L"Download failed"
                                        : (L"Download failed: " + detail);
                }
            }
        }
        ++state_.generation;
    }
    Notify();
}

} // namespace rivan::youtube
