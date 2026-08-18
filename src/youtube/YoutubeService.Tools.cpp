// YoutubeService.Tools.cpp
#include "YoutubeService.Internal.h"

#include <winhttp.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <optional>
#include <system_error>

#pragma comment(lib, "winhttp.lib")

namespace rivan::youtube::detail {

namespace {

constexpr DWORD kNetworkTimeoutMilliseconds = 60000;
// Bounds INACTIVITY, not total transfer time: a long but active download is
// never killed, only a hung/silent transfer hits the deadline.
constexpr auto kDownloadInactivityTimeout = std::chrono::seconds{60};
constexpr int kMaxRedirects = 5;

struct ParsedUrl final {
    std::wstring host;
    INTERNET_PORT port{};
    std::wstring objectName;
    INTERNET_SCHEME scheme{};
};

std::optional<ParsedUrl> CrackDownloadUrl(std::wstring_view value) {
    URL_COMPONENTS components{sizeof(components)};
    wchar_t host[256]{};
    wchar_t path[2048]{};
    wchar_t extra[2048]{};
    components.lpszHostName = host;
    components.dwHostNameLength = static_cast<DWORD>(std::size(host));
    components.lpszUrlPath = path;
    components.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    components.lpszExtraInfo = extra;
    components.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!WinHttpCrackUrl(value.data(), static_cast<DWORD>(value.size()), 0, &components)) return std::nullopt;
    if (components.nScheme != INTERNET_SCHEME_HTTP && components.nScheme != INTERNET_SCHEME_HTTPS) {
        return std::nullopt;
    }
    ParsedUrl parsed;
    parsed.scheme = components.nScheme;
    parsed.host.assign(host, components.dwHostNameLength);
    parsed.port = components.nPort;
    parsed.objectName = std::wstring(path, components.dwUrlPathLength) +
                        std::wstring(extra, components.dwExtraInfoLength);
    return parsed;
}

// Combines a (possibly relative) redirect Location with the previous URL.
std::wstring ResolveRedirectLocation(const std::wstring& previous, const std::wstring& location) {
    if (location.find(L"://") != std::wstring::npos) return location;
    if (location.empty()) return previous;
    if (location.front() == L'/') {
        const auto schemeEnd = previous.find(L"://");
        if (schemeEnd == std::wstring::npos) return location;
        const auto pathStart = previous.find(L'/', schemeEnd + 3);
        const auto base = pathStart == std::wstring::npos ? previous : previous.substr(0, pathStart);
        return base + location;
    }
    const auto lastSlash = previous.rfind(L'/');
    if (lastSlash == std::wstring::npos) return previous + L"/" + location;
    return previous.substr(0, lastSlash + 1) + location;
}

} // namespace

bool DownloadUrlToFile(const wchar_t* url, const std::filesystem::path& dest,
                       std::wstring& error, std::stop_token stop) {
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    if (ec) {
        error = L"Unable to create tools directory";
        return false;
    }
    const auto temp = dest.wstring() + L".partial";
    DeleteFileW(temp.c_str());
    std::ofstream output(temp, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = L"Unable to create download file";
        return false;
    }

    if (stop.stop_requested()) {
        output.close();
        DeleteFileW(temp.c_str());
        error = L"Cancelled";
        return false;
    }
    auto deadline = std::chrono::steady_clock::now() + kDownloadInactivityTimeout;
    const auto timedOut = [&deadline] { return std::chrono::steady_clock::now() >= deadline; };

    auto parsed = CrackDownloadUrl(url);
    if (!parsed) {
        output.close();
        DeleteFileW(temp.c_str());
        error = L"Invalid download URL";
        return false;
    }

    HINTERNET session = WinHttpOpen(L"Rivan Tool Download/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        output.close();
        DeleteFileW(temp.c_str());
        error = L"Unable to initialize network download";
        return false;
    }
    WinHttpSetTimeouts(session, kNetworkTimeoutMilliseconds, kNetworkTimeoutMilliseconds,
                       kNetworkTimeoutMilliseconds, kNetworkTimeoutMilliseconds);

    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;
    std::wstring connectedHost;
    INTERNET_PORT connectedPort = 0;
    std::wstring currentUrl = url;
    DWORD status = 0;
    bool haveContent = false;

    const auto closeAll = [&] {
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
    };
    const auto fail = [&](const wchar_t* message) {
        closeAll();
        output.close();
        DeleteFileW(temp.c_str());
        error = message;
        return false;
    };

    for (int redirects = 0; redirects <= kMaxRedirects && !stop.stop_requested() && !timedOut();
         ++redirects) {
        if (request) {
            WinHttpCloseHandle(request);
            request = nullptr;
        }
        if (connectedHost != parsed->host || connectedPort != parsed->port) {
            if (connection) {
                WinHttpCloseHandle(connection);
                connection = nullptr;
            }
            connection = WinHttpConnect(session, parsed->host.c_str(), parsed->port, 0);
            if (!connection) return fail(L"Unable to connect to download host");
            connectedHost = parsed->host;
            connectedPort = parsed->port;
        }
        const DWORD flags = parsed->scheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        request = WinHttpOpenRequest(connection, L"GET", parsed->objectName.c_str(), nullptr,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request || !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr)) {
            return fail(L"Download failed (network error)");
        }
        DWORD statusSize = sizeof(status);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                 WINHTTP_NO_HEADER_INDEX)) {
            return fail(L"Download failed (no response status)");
        }
        if (status >= 200 && status < 300) {
            haveContent = true;
            break;
        }
        const bool redirect =
            status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
        if (!redirect || redirects == kMaxRedirects) {
            const std::wstring statusError =
                L"Download failed (HTTP status " + std::to_wstring(status) + L")";
            return fail(statusError.c_str());
        }
        wchar_t location[4096]{};
        DWORD locationSize = sizeof(location);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                 location, &locationSize, WINHTTP_NO_HEADER_INDEX) ||
            locationSize == 0) {
            return fail(L"Download failed (redirect without location)");
        }
        currentUrl = ResolveRedirectLocation(currentUrl, location);
        const auto next = CrackDownloadUrl(currentUrl);
        if (!next) return fail(L"Download failed (invalid redirect location)");
        parsed = *next;
        // A successful hop is activity; only a stalled transfer hits the deadline.
        deadline = std::chrono::steady_clock::now() + kDownloadInactivityTimeout;
    }
    if (!haveContent) {
        if (stop.stop_requested()) return fail(L"Cancelled");
        if (timedOut()) return fail(L"Download timed out");
        return fail(L"Download failed");
    }

    // Stream the body into the .partial file as it arrives.
    bool bodyComplete = false;
    while (!stop.stop_requested() && !timedOut()) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) break;
        if (available == 0) {
            bodyComplete = true;
            break;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
        if (read == 0) {
            bodyComplete = true;
            break;
        }
        output.write(chunk.data(), static_cast<std::streamsize>(read));
        if (!output) return fail(L"Unable to write downloaded data");
        // A successful chunk is activity; only a silent transfer hits the deadline.
        deadline = std::chrono::steady_clock::now() + kDownloadInactivityTimeout;
    }
    output.close();
    if (!bodyComplete) {
        if (stop.stop_requested()) return fail(L"Cancelled");
        if (timedOut()) return fail(L"Download timed out");
        return fail(L"Download failed while receiving data");
    }
    closeAll();

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

std::optional<std::filesystem::path> FindFileRecursive(const std::filesystem::path& root,
                                                       const wchar_t* fileName) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return std::nullopt;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (_wcsicmp(it->path().filename().c_str(), fileName) == 0) return it->path();
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> SystemTarPath() {
    std::wstring directory(MAX_PATH, L'\0');
    const UINT length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    if (length == 0 || length >= directory.size()) return std::nullopt;
    directory.resize(length);
    const auto tar = std::filesystem::path(directory) / L"tar.exe";
    return PathExistsFile(tar) ? std::optional{tar} : std::nullopt;
}

} // namespace rivan::youtube::detail

namespace rivan::youtube {

void YoutubeService::Warm() {
    Enqueue([this] {
        // The whole body runs as one step of the serialized supervisor queue, so no
        // other step can change state_.busy while it executes.
        {
            std::scoped_lock lock(mutex_);
            if (state_.busy) return;
            if (!LocateYtDlp().has_value()) return;
        }
        JoinWorker();
        worker_ = std::jthread([this](std::stop_token stop) { RunWarm(stop); });
    });
}

void YoutubeService::RunWarm(std::stop_token stop) {
    const auto ytDlp = LocateYtDlp();
    if (!ytDlp || stop.stop_requested()) return;
    std::string output;
    std::string error;
    DWORD exitCode = 1;
    (void)detail::RunProcessCapture(*ytDlp, L"--version --no-warnings", stop, output, error,
                                    &exitCode);
}

void YoutubeService::InstallTool(YoutubeTool tool) {
    Enqueue([this, tool] {
        JoinWorker();
        bool start = false;
        {
            std::scoped_lock lock(mutex_);
            WriteToolFlagsLocked();
            const bool already =
                (tool == YoutubeTool::YtDlp && state_.ytDlpInstalled) ||
                (tool == YoutubeTool::Ffmpeg && state_.ffmpegInstalled) ||
                (tool == YoutubeTool::Deno && state_.denoInstalled);
            if (already) {
                state_.status = tool == YoutubeTool::YtDlp ? L"yt-dlp already installed"
                               : tool == YoutubeTool::Ffmpeg ? L"ffmpeg already installed"
                                                              : L"deno already installed";
                ++state_.generation;
            } else {
                state_.busy = true;
                state_.job = YoutubeJobKind::Install;
                state_.installingYtDlp = tool == YoutubeTool::YtDlp;
                state_.installingFfmpeg = tool == YoutubeTool::Ffmpeg;
                state_.installingDeno = tool == YoutubeTool::Deno;
                state_.status = tool == YoutubeTool::YtDlp ? L"Installing yt-dlp..."
                               : tool == YoutubeTool::Ffmpeg ? L"Installing ffmpeg..."
                                                              : L"Installing deno...";
                ++state_.generation;
                start = true;
            }
        }
        Notify();
        if (!start) return;
        worker_ = std::jthread([this, tool](std::stop_token stop) { RunInstall(stop, tool); });
    });
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
        ok = detail::DownloadUrlToFile(
            L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe", dest, error,
            stop);
        if (ok && !detail::PathExistsFile(dest)) {
            ok = false;
            error = L"yt-dlp.exe missing after download";
        }
    } else if (tool == YoutubeTool::Ffmpeg) {
        const auto zipPath = tools / L"ffmpeg-essentials.zip";
        const auto extractDir = tools / L"ffmpeg-extract";
        ok = detail::DownloadUrlToFile(
            L"https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip", zipPath, error,
            stop);
        if (ok && !stop.stop_requested()) {
            std::filesystem::remove_all(extractDir, ec);
            std::filesystem::create_directories(extractDir, ec);
            std::string out;
            std::string err;
            DWORD code = 1;
            const std::wstring args = L"-xf " + detail::QuoteArg(zipPath.wstring()) + L" -C " +
                                       detail::QuoteArg(extractDir.wstring());
            const auto tar = detail::SystemTarPath();
            if (!tar || !detail::RunProcessCapture(*tar, args, stop, out, err, &code) || code != 0) {
                ok = false;
                error = L"Unable to extract ffmpeg archive (Windows tar.exe is required)";
            } else if (auto found = detail::FindFileRecursive(extractDir, L"ffmpeg.exe")) {
                const auto dest = tools / L"ffmpeg.exe";
                std::filesystem::copy_file(*found, dest,
                                           std::filesystem::copy_options::overwrite_existing, ec);
                if (ec || !detail::PathExistsFile(dest)) {
                    ok = false;
                    error = L"Unable to copy ffmpeg.exe";
                }
            } else {
                ok = false;
                error = L"ffmpeg.exe not found in archive";
            }
        }
        // Remove the per-install working files on every path, including a cancel that
        // lands right after the archive download completed, so ffmpeg-essentials.zip
        // and ffmpeg-extract cannot leak in tools\. Only these install artifacts are
        // removed; an installed ffmpeg.exe is never deleted.
        std::filesystem::remove_all(extractDir, ec);
        std::filesystem::remove(zipPath, ec);
    } else {
        const auto zipPath = tools / L"deno.zip";
        const auto extractDir = tools / L"deno-extract";
        ok = detail::DownloadUrlToFile(
            L"https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip",
            zipPath, error, stop);
        if (ok && !stop.stop_requested()) {
            std::filesystem::remove_all(extractDir, ec);
            std::filesystem::create_directories(extractDir, ec);
            std::string out;
            std::string err;
            DWORD code = 1;
            const std::wstring args = L"-xf " + detail::QuoteArg(zipPath.wstring()) + L" -C " +
                                       detail::QuoteArg(extractDir.wstring());
            const auto tar = detail::SystemTarPath();
            if (!tar || !detail::RunProcessCapture(*tar, args, stop, out, err, &code) || code != 0) {
                ok = false;
                error = L"Unable to extract deno archive (Windows tar.exe is required)";
            } else if (auto found = detail::FindFileRecursive(extractDir, L"deno.exe")) {
                const auto dest = tools / L"deno.exe";
                std::filesystem::copy_file(*found, dest,
                                           std::filesystem::copy_options::overwrite_existing, ec);
                if (ec || !detail::PathExistsFile(dest)) {
                    ok = false;
                    error = L"Unable to copy deno.exe";
                }
            } else {
                ok = false;
                error = L"deno.exe not found in archive";
            }
        }
        // Same cleanup contract as the ffmpeg branch: never leave the per-install
        // download/extract artifacts behind, but keep an installed deno.exe.
        std::filesystem::remove_all(extractDir, ec);
        std::filesystem::remove(zipPath, ec);
    }

    {
        std::scoped_lock lock(mutex_);
        state_.busy = false;
        state_.job = YoutubeJobKind::Idle;
        state_.installingYtDlp = false;
        state_.installingFfmpeg = false;
        state_.installingDeno = false;
        WriteToolFlagsLocked();
        if (stop.stop_requested()) {
            state_.status = L"Cancelled";
        } else if (ok) {
            state_.status = tool == YoutubeTool::YtDlp ? L"yt-dlp installed"
                           : tool == YoutubeTool::Ffmpeg ? L"ffmpeg installed"
                                                          : L"deno installed";
        } else {
            state_.status = error.empty() ? L"Install failed" : error;
        }
        ++state_.generation;
    }
    Notify();
}

} // namespace rivan::youtube
