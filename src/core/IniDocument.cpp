// Rivan source file
// Purpose: Compact UTF-8 INI parsing and atomic persistence.
#include "IniDocument.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace rivan::core {
namespace {

constexpr std::uintmax_t kMaximumIniBytes = 16U * 1024U * 1024U;
std::atomic_uint64_t g_tempSequence{0};

std::string_view Trim(std::string_view value) noexcept {
    constexpr std::string_view whitespace = " \t\r\n";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

bool IsValidUtf8(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        if (lead <= 0x7F) {
            ++index;
            continue;
        }

        std::size_t continuationCount = 0;
        std::uint32_t codePoint = 0;
        if (lead >= 0xC2 && lead <= 0xDF) {
            continuationCount = 1;
            codePoint = lead & 0x1F;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            continuationCount = 2;
            codePoint = lead & 0x0F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            continuationCount = 3;
            codePoint = lead & 0x07;
        } else {
            return false;
        }

        if (index + continuationCount >= text.size()) {
            return false;
        }
        for (std::size_t i = 1; i <= continuationCount; ++i) {
            const auto byte = static_cast<unsigned char>(text[index + i]);
            if ((byte & 0xC0) != 0x80) {
                return false;
            }
            codePoint = (codePoint << 6U) | (byte & 0x3FU);
        }

        if ((continuationCount == 2 && codePoint < 0x800) ||
            (continuationCount == 3 && codePoint < 0x10000) ||
            codePoint > 0x10FFFF ||
            (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
            return false;
        }
        index += continuationCount + 1;
    }
    return true;
}

std::string WindowsErrorMessage(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<wchar_t*>(&buffer),
        0,
        nullptr);
    if (length == 0 || buffer == nullptr) {
        return "Windows error " + std::to_string(code);
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    std::string message(required > 0 ? static_cast<std::size_t>(required) : 0U, '\0');
    if (required > 0) {
        WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), message.data(), required, nullptr, nullptr);
    }
    LocalFree(buffer);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
        message.pop_back();
    }
    return message.empty() ? "Windows error " + std::to_string(code) : message;
}

void SetError(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

bool ReadAll(const std::filesystem::path& path, std::string& content, std::string* error) {
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        SetError(error, "Unable to open INI file: " + WindowsErrorMessage(GetLastError()));
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > kMaximumIniBytes) {
        const DWORD code = GetLastError();
        CloseHandle(file);
        SetError(error, size.QuadPart > static_cast<LONGLONG>(kMaximumIniBytes)
                            ? "INI file exceeds the 16 MiB limit"
                            : "Unable to inspect INI file: " + WindowsErrorMessage(code));
        return false;
    }

    content.assign(static_cast<std::size_t>(size.QuadPart), '\0');
    std::size_t offset = 0;
    while (offset < content.size()) {
        const DWORD request = static_cast<DWORD>(
            (std::min)(content.size() - offset, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD read = 0;
        if (!ReadFile(file, content.data() + offset, request, &read, nullptr)) {
            const DWORD code = GetLastError();
            CloseHandle(file);
            SetError(error, "Unable to read INI file: " + WindowsErrorMessage(code));
            return false;
        }
        if (read == 0) {
            CloseHandle(file);
            SetError(error, "INI file changed while it was being read");
            return false;
        }
        offset += read;
    }
    CloseHandle(file);
    return true;
}

std::filesystem::path TemporaryPathFor(const std::filesystem::path& destination) {
    const auto sequence = g_tempSequence.fetch_add(1, std::memory_order_relaxed);
    std::wstring name = destination.filename().wstring();
    name += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(sequence);
    return destination.parent_path() / name;
}

bool WriteAllAndFlush(const std::filesystem::path& path, std::string_view content,
                      DWORD* createError, std::string* error) {
    if (createError != nullptr) {
        *createError = ERROR_SUCCESS;
    }
    const HANDLE file = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (createError != nullptr) {
            *createError = code;
        }
        SetError(error, "Unable to create temporary INI file: " + WindowsErrorMessage(code));
        return false;
    }

    std::size_t offset = 0;
    while (offset < content.size()) {
        const DWORD request = static_cast<DWORD>(
            (std::min)(content.size() - offset, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!WriteFile(file, content.data() + offset, request, &written, nullptr) || written == 0) {
            const DWORD code = GetLastError();
            CloseHandle(file);
            DeleteFileW(path.c_str());
            SetError(error, "Unable to write temporary INI file: " + WindowsErrorMessage(code));
            return false;
        }
        offset += written;
    }

    if (!FlushFileBuffers(file)) {
        const DWORD code = GetLastError();
        CloseHandle(file);
        DeleteFileW(path.c_str());
        SetError(error, "Unable to flush temporary INI file: " + WindowsErrorMessage(code));
        return false;
    }
    CloseHandle(file);
    return true;
}

} // namespace

std::optional<IniDocument> IniDocument::Parse(std::string_view utf8, std::string* error) {
    if (utf8.starts_with("\xEF\xBB\xBF")) {
        utf8.remove_prefix(3);
    }
    if (!IsValidUtf8(utf8)) {
        SetError(error, "INI content is not valid UTF-8");
        return std::nullopt;
    }

    IniDocument result;
    std::string currentSection;
    std::size_t lineNumber = 0;
    while (!utf8.empty()) {
        ++lineNumber;
        const auto end = utf8.find('\n');
        std::string_view line = end == std::string_view::npos ? utf8 : utf8.substr(0, end);
        utf8 = end == std::string_view::npos ? std::string_view{} : utf8.substr(end + 1);
        line = Trim(line);
        if (line.empty() || line.front() == ';' || line.front() == '#') {
            continue;
        }

        if (line.front() == '[') {
            if (line.size() < 3 || line.back() != ']') {
                SetError(error, "Malformed section header on line " + std::to_string(lineNumber));
                return std::nullopt;
            }
            const auto section = Trim(line.substr(1, line.size() - 2));
            if (section.empty() || section.find_first_of("[]=\r\n") != std::string_view::npos) {
                SetError(error, "Invalid section name on line " + std::to_string(lineNumber));
                return std::nullopt;
            }
            currentSection.assign(section);
            result.sections_.try_emplace(currentSection);
            continue;
        }

        const auto separator = line.find('=');
        if (separator == std::string_view::npos) {
            SetError(error, "Expected key=value on line " + std::to_string(lineNumber));
            return std::nullopt;
        }
        const auto key = Trim(line.substr(0, separator));
        const auto value = Trim(line.substr(separator + 1));
        if (key.empty() || key.find_first_of("[]=\r\n") != std::string_view::npos) {
            SetError(error, "Invalid key on line " + std::to_string(lineNumber));
            return std::nullopt;
        }
        result.sections_[currentSection][std::string(key)] = std::string(value);
    }

    if (error != nullptr) {
        error->clear();
    }
    return result;
}

std::optional<IniDocument> IniDocument::Load(const std::filesystem::path& path, std::string* error) {
    std::string content;
    if (!ReadAll(path, content, error)) {
        return std::nullopt;
    }
    return Parse(content, error);
}

std::string IniDocument::Serialize() const {
    std::string output;
    for (const auto& [sectionName, section] : sections_) {
        if (!sectionName.empty()) {
            if (!output.empty()) {
                output.push_back('\n');
            }
            output += '[';
            output += sectionName;
            output += "]\n";
        }
        for (const auto& [key, value] : section) {
            output += key;
            output += '=';
            output += value;
            output.push_back('\n');
        }
    }
    return output;
}

bool IniDocument::SaveAtomic(const std::filesystem::path& path, std::string* error) const {
    if (path.empty() || path.filename().empty()) {
        SetError(error, "INI destination path is empty");
        return false;
    }

    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            SetError(error, "Unable to create INI directory: " + ec.message());
            return false;
        }
    }

    const std::string serialized = Serialize();
    std::filesystem::path temporary;
    bool written = false;
    for (int attempt = 0; attempt < 16 && !written; ++attempt) {
        temporary = TemporaryPathFor(path);
        DWORD createError = ERROR_SUCCESS;
        written = WriteAllAndFlush(temporary, serialized, &createError, error);
        if (!written && createError != ERROR_FILE_EXISTS && createError != ERROR_ALREADY_EXISTS) {
            return false;
        }
    }
    if (!written) {
        SetError(error, "Unable to allocate a unique temporary INI file");
        return false;
    }

    const DWORD attributes = GetFileAttributesW(path.c_str());
    bool replaced = false;
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        replaced = ReplaceFileW(path.c_str(), temporary.c_str(), nullptr,
                                REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr) != FALSE;
    } else {
        replaced = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    }

    if (!replaced) {
        const DWORD code = GetLastError();
        DeleteFileW(temporary.c_str());
        SetError(error, "Unable to replace INI file atomically: " + WindowsErrorMessage(code));
        return false;
    }

    // Flush the parent directory so the rename survives a power loss. Directory
    // handles require FILE_FLAG_BACKUP_SEMANTICS; failures on older filesystems,
    // remote shares, or restricted volumes are non-fatal for correctness of the file.
    if (!path.parent_path().empty()) {
        const HANDLE directory = CreateFileW(
            path.parent_path().c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (directory != INVALID_HANDLE_VALUE) {
            (void)FlushFileBuffers(directory);
            CloseHandle(directory);
        }
    }

    if (error != nullptr) {
        error->clear();
    }
    return true;
}

std::optional<std::string_view> IniDocument::Get(
    std::string_view section,
    std::string_view key) const noexcept {
    const auto sectionIt = sections_.find(section);
    if (sectionIt == sections_.end()) {
        return std::nullopt;
    }
    const auto valueIt = sectionIt->second.find(key);
    if (valueIt == sectionIt->second.end()) {
        return std::nullopt;
    }
    return valueIt->second;
}

bool IniDocument::HasMetaFormat(std::string_view expected) const noexcept {
    const auto format = Get("meta", "format");
    return format && *format == expected;
}

void IniDocument::Set(std::string section, std::string key, std::string value) {
    sections_[std::move(section)][std::move(key)] = std::move(value);
}

} // namespace rivan::core
