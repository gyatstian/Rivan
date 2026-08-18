// LyricsService.Storage.cpp
// Lyrics file storage: unified user/fetched lyrics files, the legacy fingerprint cache,
// path association, and cache pruning.
#include "LyricsService.Internal.h"

#include "../core/Text.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace rivan::lyrics {
namespace {

constexpr std::size_t kCacheMaxFiles = 4096;

std::uint64_t HashText(std::wstring_view value, std::uint64_t hash) noexcept {
    constexpr std::uint64_t prime = 1099511628211ull;
    for (const wchar_t character : value) {
        const auto code = static_cast<std::uint32_t>(character);
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= static_cast<unsigned char>((code >> shift) & 0xffu);
            hash *= prime;
        }
    }
    return hash;
}

std::uint64_t RequestFingerprint(std::wstring_view title, std::wstring_view artist,
                                 std::wstring_view album, const double durationSeconds) noexcept {
    constexpr std::uint64_t offset = 14695981039346656037ull;
    auto hash = HashText(title, offset);
    hash = HashText(artist, hash);
    hash = HashText(album, hash);
    const auto duration = static_cast<std::uint64_t>(std::max(0.0, durationSeconds) * 100.0);
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= static_cast<unsigned char>((duration >> shift) & 0xffu);
        hash *= 1099511628211ull;
    }
    return hash;
}

void PruneCacheIfNeeded(const std::filesystem::path& cacheDirectory) {
    // Quick over-cap probe: stop counting once the cap is exceeded so the common
    // small-cache path iterates at most kCacheMaxFiles + 1 entries.
    std::error_code ec;
    bool overCap = false;
    std::size_t count = 0;
    for (std::filesystem::directory_iterator it(cacheDirectory, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (it->path().extension() != L".lyrics") continue;
        if (++count > kCacheMaxFiles) {
            overCap = true;
            break;
        }
    }
    if (ec || !overCap) return;

    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
    for (std::filesystem::directory_iterator it(cacheDirectory, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto& path = it->path();
        if (path.extension() != L".lyrics") continue;
        files.emplace_back(std::filesystem::last_write_time(path, ec), path);
        ec.clear();
    }
    std::sort(files.begin(), files.end(),
              [](const auto& left, const auto& right) { return left.first > right.first; });
    for (std::size_t index = kCacheMaxFiles; index < files.size(); ++index) {
        std::filesystem::remove(files[index].second, ec);
        ec.clear();
    }
}

// Windows file names cannot contain these characters (nor the C0 control range).
// Also capped so title-derived names stay well under MAX_PATH even with a suffix.
std::wstring SanitizeFileName(std::wstring value) {
    constexpr std::size_t kMaximumFileNameLength = 120;
    value = detail::Trim(std::move(value));
    std::wstring result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        if (result.size() >= kMaximumFileNameLength) break;
        if (character == L'<' || character == L'>' || character == L':' ||
            character == L'"' || character == L'/' || character == L'\\' ||
            character == L'|' || character == L'?' || character == L'*' ||
            character < 0x20u) {
            continue;
        }
        result.push_back(character);
    }
    // Trailing dots and spaces are also rejected by Windows.
    while (!result.empty() && (result.back() == L'.' || result.back() == L' ')) result.pop_back();
    return result.empty() ? std::wstring(L"Lyrics") : result;
}

// Canonicalizes and case-folds a path so two spellings of the same song file compare
// equal. Windows paths are case-insensitive, so the fold matters for a hand-edited file.
std::wstring NormalizePathText(const std::filesystem::path& path) {
    std::error_code ec;
    auto absolute = std::filesystem::absolute(path, ec);
    if (ec) absolute = path;
    auto normalized = std::filesystem::weakly_canonical(absolute, ec);
    if (ec) normalized = absolute;
    return detail::FoldCase(normalized.wstring());
}

// Decodes text bytes honoring UTF-8 BOM and UTF-16 LE/BE BOMs, so lyrics saved by any
// common Windows editor (Notepad default encodings included) decode correctly.
std::wstring DecodeTextBytes(std::string bytes) {
    const auto byte = [&bytes](std::size_t index) {
        return static_cast<unsigned char>(bytes[index]);
    };
    if (bytes.size() >= 3 && byte(0) == 0xefu && byte(1) == 0xbbu && byte(2) == 0xbfu) {
        return core::Utf8ToWide(std::string_view(bytes).substr(3));
    }
    if (bytes.size() >= 2 && byte(0) == 0xffu && byte(1) == 0xfeu) {
        const auto chars = (bytes.size() - 2) / 2;
        std::wstring result(chars, L'\0');
        if (chars != 0) std::memcpy(result.data(), bytes.data() + 2, chars * sizeof(wchar_t));
        return result;
    }
    if (bytes.size() >= 2 && byte(0) == 0xfeu && byte(1) == 0xffu) {
        const auto chars = (bytes.size() - 2) / 2;
        std::wstring result(chars, L'\0');
        for (std::size_t index = 0; index < chars; ++index) {
            const auto offset = 2 + index * 2;
            result[index] = static_cast<wchar_t>(
                (byte(offset) << 8) | byte(offset + 1));
        }
        return result;
    }
    return core::Utf8ToWide(bytes);
}

// Reads a small text file in full.
std::optional<std::wstring> ReadTextFileWide(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > detail::kMaximumResponseBytes) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string bytes(static_cast<std::size_t>(size), '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(size));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    return DecodeTextBytes(std::move(bytes));
}

// Reads only the leading bytes of a small text file. The unified lyrics header (magic
// + "#Song file:" path) lives at the top, so scanning a full directory only needs this
// prefix until a file actually matches the requested song.
std::optional<std::wstring> ReadTextFilePrefixWide(const std::filesystem::path& path,
                                                   std::size_t prefixBytes) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > detail::kMaximumResponseBytes) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    const auto toRead = std::min<std::size_t>(size, prefixBytes);
    std::string bytes(toRead, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(toRead));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    return DecodeTextBytes(std::move(bytes));
}

// Formats a timestamp as an LRC "[mm:ss.hh]" prefix, e.g. 75.5 -> "[01:15.50]".
std::wstring FormatLrcTimestamp(const double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) return L"[00:00.00]";
    const auto totalMilliseconds = static_cast<long long>(std::llround(seconds * 1000.0));
    const auto minutes = totalMilliseconds / 60000;
    const auto remainder = totalMilliseconds % 60000;
    const auto wholeSeconds = remainder / 1000;
    const auto hundredths = (remainder % 1000) / 10;
    wchar_t buffer[32]{};
    std::swprintf(buffer, std::size(buffer), L"[%02lld:%02lld.%02lld]",
                  minutes, wholeSeconds, hundredths);
    return buffer;
}

} // namespace

namespace detail {

std::wstring FoldCase(std::wstring value) {
    for (auto& character : value) character = static_cast<wchar_t>(std::towlower(character));
    return value;
}

} // namespace detail

std::wstring LyricsService::NormalizedTrackPath(const std::filesystem::path& path) {
    auto result = path.wstring();
    for (auto& character : result) {
        character = static_cast<wchar_t>(std::towlower(character));
    }
    return result;
}

std::filesystem::path LyricsService::CreateUserLyricsFile(
    std::uint64_t, std::wstring title, std::wstring, std::wstring, double,
    std::filesystem::path filePath) const {
    if (cacheDirectory_.empty() || filePath.empty()) return {};
    std::error_code ec;
    std::filesystem::create_directories(cacheDirectory_, ec);
    if (ec) return {};
    const auto base = SanitizeFileName(std::move(title));
    const auto expected = NormalizePathText(filePath);
    const auto fullName = [&base](std::size_t suffix) {
        return suffix == 1 ? base + std::wstring(detail::kCustomLyricsExtension)
                           : base + L" (" + std::to_wstring(suffix) + L")" +
                                 std::wstring(detail::kCustomLyricsExtension);
    };
    auto candidate = cacheDirectory_ / fullName(1);
    for (std::size_t suffix = 1; suffix <= 128; ++suffix) {
        if (std::filesystem::exists(candidate, ec)) {
            // Reuse an existing custom file that already belongs to this song, so a
            // second click opens the same document instead of stacking duplicates.
            if (const auto text = ReadTextFileWide(candidate);
                text && NormalizePathText(UserLyricsTrackPath(*text)) == expected) {
                return candidate;
            }
            ec.clear();
            candidate = cacheDirectory_ / fullName(suffix + 1);
            continue;
        }
        if (ec) return {};
        break;
    }

    std::wstring content = std::wstring(detail::kCustomLyricsMagic) + L"\n";
    content += std::wstring(detail::kSongFileHeader) + L" " + filePath.wstring() + L"\n";
    content += L"\n";
    content += L"# Paste your lyrics below, one verse per line.\n";
    content += L"# Synced lyrics: prefix a line with its start time, e.g. [00:12.50]Hello world.\n";
    content += L"# Keep the '#RIVAN-CUSTOM-LYRICS-1' and '#Song file:' header lines unchanged.\n";
    content += L"\n";
    const auto utf8 = core::WideToUtf8(content);
    std::ofstream output(candidate, std::ios::binary | std::ios::trunc);
    if (!output) return {};
    // A UTF-8 BOM lets modern Windows Notepad detect the encoding for non-ASCII lyrics.
    constexpr char kUtf8Bom[] = "\xef\xbb\xbf";
    output.write(kUtf8Bom, static_cast<std::streamsize>(sizeof(kUtf8Bom) - 1));
    output.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    output.close();
    if (!output) {
        std::filesystem::remove(candidate, ec);
        return {};
    }
    return candidate;
}

std::filesystem::path LyricsService::CachePath(const RequestData& request) const {
    std::wostringstream name;
    name << request.trackId << L'.' << std::hex
         << RequestFingerprint(request.title, request.artist, request.album, request.durationSeconds)
         << L".lyrics";
    return cacheDirectory_ / name.str();
}

std::optional<LyricsDocument> LyricsService::LoadCache(const RequestData& request) const {
    if (cacheDirectory_.empty()) return std::nullopt;
    const auto cachePath = CachePath(request);
    std::error_code error;
    const auto size = std::filesystem::file_size(cachePath, error);
    if (error || size > detail::kMaximumResponseBytes) return std::nullopt;
    std::ifstream input(cachePath, std::ios::binary);
    if (!input) return std::nullopt;
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (size > 0) {
        input.read(bytes.data(), static_cast<std::streamsize>(size));
        bytes.resize(static_cast<std::size_t>(input.gcount()));
    }
    if (bytes.rfind("RIVAN-LYRICS-1\n", 0) != 0) return std::nullopt;
    constexpr std::string_view header = "RIVAN-LYRICS-1\n";
    const auto separator = bytes.find('\n', header.size());
    if (separator == std::string::npos) return std::nullopt;
    LyricsDocument document;
    document.synced = bytes.substr(header.size(), separator - header.size()) == "synced=1";
    const auto wide = core::Utf8ToWide(bytes.substr(separator + 1));
    std::wistringstream stream{wide};
    std::wstring row;
    while (std::getline(stream, row)) {
        const auto tab = row.find(L'\t');
        if (tab == std::wstring::npos) continue;
        const auto timestamp = detail::ParseNumber(row.substr(0, tab));
        if (timestamp) document.lines.push_back({*timestamp, row.substr(tab + 1)});
    }
    return document.Empty() ? std::nullopt : std::optional<LyricsDocument>(std::move(document));
}

std::optional<LyricsDocument> LyricsService::LoadLyricsFiles(const RequestData& request) const {
    if (cacheDirectory_.empty() || request.filePath.empty()) return std::nullopt;
    const auto expected = NormalizePathText(request.filePath);
    // Header scan reads only a prefix per file; the header (magic + song path) always
    // lives at the top. Full content is read only for the file that actually matches.
    constexpr std::size_t kHeaderScanBytes = 4096;
    std::error_code ec;
    std::filesystem::path best;
    std::filesystem::file_time_type bestTime{};
    for (std::filesystem::directory_iterator it(cacheDirectory_, ec), end;
         it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto& path = it->path();
        if (path.extension() != detail::kCustomLyricsExtension) continue;
        auto prefix = ReadTextFilePrefixWide(path, kHeaderScanBytes);
        if (!prefix || prefix->find(detail::kCustomLyricsMagic) == std::wstring::npos) continue;
        if (NormalizePathText(UserLyricsTrackPath(*prefix)) != expected) continue;
        // Multiple files may reference the same song (e.g. hand-edited copies); the most
        // recently modified one wins so a fresh user edit always shows.
        const auto modified = std::filesystem::last_write_time(path, ec);
        if (ec || best.empty() || modified > bestTime) {
            best = path;
            bestTime = modified;
            ec.clear();
        }
    }
    if (best.empty()) return std::nullopt;
    auto text = ReadTextFileWide(best);
    if (!text) return std::nullopt;
    auto document = ParseCustomLyrics(*text);
    return document.Empty() ? std::nullopt : std::optional<LyricsDocument>(std::move(document));
}

std::filesystem::path LyricsService::SaveLyricsFile(
    const std::filesystem::path& directory, std::wstring title,
    const std::filesystem::path& filePath, const LyricsDocument& document) {
    if (directory.empty() || filePath.empty() || document.Empty()) return {};
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) return {};
    const auto expected = NormalizePathText(filePath);
    const auto base = SanitizeFileName(std::move(title));
    const auto fullName = [&base](std::size_t suffix) {
        return suffix == 1 ? base + std::wstring(detail::kCustomLyricsExtension)
                           : base + L" (" + std::to_wstring(suffix) + L")" +
                                 std::wstring(detail::kCustomLyricsExtension);
    };
    auto candidate = directory / fullName(1);
    for (std::size_t suffix = 1; suffix <= 128; ++suffix) {
        if (std::filesystem::exists(candidate, ec)) {
            // A lyrics file for this song already exists (possibly user-edited); never
            // overwrite it with fetched data.
            if (const auto text = ReadTextFileWide(candidate);
                text && NormalizePathText(UserLyricsTrackPath(*text)) == expected) {
                return candidate;
            }
            ec.clear();
            candidate = directory / fullName(suffix + 1);
            continue;
        }
        if (ec) return {};
        break;
    }

    std::wstring content = std::wstring(detail::kCustomLyricsMagic) + L"\n";
    content += std::wstring(detail::kSongFileHeader) + L" " + filePath.wstring() + L"\n";
    content += L"\n";
    if (document.synced) {
        for (const auto& line : document.lines) {
            content += line.timestampSeconds >= 0.0
                           ? FormatLrcTimestamp(line.timestampSeconds) + line.text
                           : line.text;
            content += L'\n';
        }
    } else {
        for (const auto& line : document.lines) {
            content += line.text + L'\n';
        }
    }
    const auto utf8 = core::WideToUtf8(content);
    std::ofstream output(candidate, std::ios::binary | std::ios::trunc);
    if (!output) return {};
    // A UTF-8 BOM lets modern Windows Notepad detect the encoding for non-ASCII lyrics.
    constexpr char kUtf8Bom[] = "\xef\xbb\xbf";
    output.write(kUtf8Bom, static_cast<std::streamsize>(sizeof(kUtf8Bom) - 1));
    output.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    output.close();
    if (!output) {
        std::filesystem::remove(candidate, ec);
        return {};
    }
    return candidate;
}

void LyricsService::SaveCache(const RequestData& request, const LyricsDocument& document) const {
    if (cacheDirectory_.empty() || document.Empty()) return;
    (void)SaveLyricsFile(cacheDirectory_, request.title, request.filePath, document);
    // Unified ".txt" files are user-visible and possibly hand-edited, so they are never
    // pruned; the legacy ".lyrics" files (fingerprint-named) still are.
    PruneCacheIfNeeded(cacheDirectory_);
}

std::size_t LyricsService::RetargetLyricsFilePaths(const std::filesystem::path& directory,
                                                   const std::filesystem::path& oldPath,
                                                   const std::filesystem::path& newPath) {
    if (directory.empty() || oldPath.empty() || newPath.empty()) return 0;
    const auto expectedOld = NormalizePathText(oldPath);
    std::size_t rewritten = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(directory, ec), end; it != end && !ec;
         it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        if (!it->is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        const auto path = it->path();
        if (path.extension() != detail::kCustomLyricsExtension) continue;
        auto text = ReadTextFileWide(path);
        if (!text) continue;
        if (NormalizePathText(UserLyricsTrackPath(*text)) != expectedOld) continue;
        // Rebuild the body, retargeting the song-path header line to the new path.
        std::wistringstream input(*text);
        std::wstring output;
        std::wstring line;
        bool changed = false;
        while (std::getline(input, line)) {
            if (!changed && line.rfind(detail::kSongFileHeader, 0) == 0 &&
                NormalizePathText(detail::Trim(line.substr(detail::kSongFileHeader.size()))) == expectedOld) {
                line = std::wstring(detail::kSongFileHeader) + L" " + newPath.wstring();
                changed = true;
            }
            output += line;
            output += L'\n';
        }
        if (!changed) continue;
        const auto utf8 = core::WideToUtf8(output);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        constexpr char kUtf8Bom[] = "\xef\xbb\xbf";
        file.write(kUtf8Bom, static_cast<std::streamsize>(sizeof(kUtf8Bom) - 1));
        file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
        file.close();
        if (file) ++rewritten;
    }
    return rewritten;
}

} // namespace rivan::lyrics