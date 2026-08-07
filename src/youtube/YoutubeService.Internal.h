// YoutubeService.Internal.h
#pragma once

#include "YoutubeService.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <string_view>

namespace rivan::youtube::detail {

using ProcessLineCallback = std::function<void(std::string_view line)>;

[[nodiscard]] std::wstring Trim(std::wstring value);
[[nodiscard]] std::wstring Lower(std::wstring value);
[[nodiscard]] bool PathExistsFile(const std::filesystem::path& path);

[[nodiscard]] std::wstring BuildUnbufferedEnvironment();
[[nodiscard]] bool RunProcessCapture(const std::filesystem::path& exe,
                                     const std::wstring& arguments,
                                     std::stop_token stop,
                                     std::string& stdoutText,
                                     std::string& errorText,
                                     DWORD* exitCode,
                                     ProcessLineCallback onLine = {});
[[nodiscard]] bool ParseDownloadPercent(std::string_view line, float& percentOut);
[[nodiscard]] bool LineLooksLikePostprocess(std::string_view line);

[[nodiscard]] std::wstring Utf8ToWide(std::string_view text);
[[nodiscard]] std::wstring QuoteArg(std::wstring_view value);
[[nodiscard]] std::wstring FfmpegLocationArg();
[[nodiscard]] std::wstring TailWide(const std::string& text, std::size_t maxChars);

[[nodiscard]] bool LooksLikeYoutubeVideoId(std::wstring_view id) noexcept;
[[nodiscard]] std::uint64_t HashText(std::wstring_view text) noexcept;
[[nodiscard]] double ParseDuration(std::string_view text);
[[nodiscard]] std::optional<YoutubeEntry> TryParseListingLine(std::string_view line);
[[nodiscard]] std::vector<YoutubeEntry> ParseListing(const std::string& stdoutText);
[[nodiscard]] std::optional<YoutubeProbe> ParseProbeJson(const std::string& stdoutText);

[[nodiscard]] std::optional<std::filesystem::path> FindDownloadedFile(
    const std::filesystem::path& directory, std::wstring_view videoId);
[[nodiscard]] std::filesystem::path StripIdSuffixAndRename(
    const std::filesystem::path& file, std::wstring_view videoId);

[[nodiscard]] bool DownloadUrlToFile(const wchar_t* url,
                                     const std::filesystem::path& dest,
                                     std::wstring& error);
[[nodiscard]] std::optional<std::filesystem::path> FindFileRecursive(
    const std::filesystem::path& root, const wchar_t* fileName);
[[nodiscard]] std::optional<std::filesystem::path> SystemTarPath();

} // namespace rivan::youtube::detail
