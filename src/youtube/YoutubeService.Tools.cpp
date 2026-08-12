// YoutubeService.Tools.cpp
#include "YoutubeService.Internal.h"

#include <Urlmon.h>

#include <system_error>

#pragma comment(lib, "Urlmon.lib")

namespace rivan::youtube::detail {

bool DownloadUrlToFile(const wchar_t* url, const std::filesystem::path& dest,
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
        // Preserve the guard logic verbatim: Warm must never interrupt a real
        // running job, so it returns early here without joining when busy.
        {
            std::scoped_lock lock(mutex_);
            if (state_.busy) return;
            if (!LocateYtDlp().has_value()) return;
        }
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
            L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe", dest, error);
        if (ok && !detail::PathExistsFile(dest)) {
            ok = false;
            error = L"yt-dlp.exe missing after download";
        }
    } else {
        const auto zipPath = tools / L"ffmpeg-essentials.zip";
        const auto extractDir = tools / L"ffmpeg-extract";
        ok = detail::DownloadUrlToFile(
            L"https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip", zipPath, error);
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
            state_.status = tool == YoutubeTool::YtDlp ? L"yt-dlp installed" : L"ffmpeg installed";
        } else {
            state_.status = error.empty() ? L"Install failed" : error;
        }
        ++state_.generation;
    }
    Notify();
}

} // namespace rivan::youtube
