// YoutubeService.Process.cpp
#include "YoutubeService.Internal.h"

#include <algorithm>
#include <charconv>
#include <cwctype>
#include <system_error>
#include <utility>

namespace rivan::youtube::detail {

std::wstring Trim(std::wstring value) {
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

std::wstring Lower(std::wstring value) {
    for (auto& ch : value) ch = static_cast<wchar_t>(std::towlower(ch));
    return value;
}

bool PathExistsFile(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) && !ec;
}

std::wstring BuildUnbufferedEnvironment() {
    const wchar_t* parent = GetEnvironmentStringsW();
    std::wstring block;
    if (parent) {
        const wchar_t* cursor = parent;
        while (*cursor != L'\0') {
            const std::wstring entry = cursor;
            cursor += entry.size() + 1;
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

bool RunProcessCapture(const std::filesystem::path& exe, const std::wstring& arguments,
                       std::stop_token stop, std::string& stdoutText, std::string& errorText,
                       DWORD* exitCode, ProcessLineCallback onLine) {
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
    DWORD pipeMode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
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
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available == 0) {
            const DWORD wait = WaitForSingleObject(process.hProcess, 15);
            if (wait == WAIT_OBJECT_0) {
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
        if (buffer.size() > 8 * 1024 * 1024) {
            TerminateProcess(process.hProcess, 1);
            errorText = "Captured process output exceeded 8 MiB";
            break;
        }
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
    if (!errorText.empty()) return false;
    if (stop.stop_requested()) {
        errorText = "Cancelled";
        return false;
    }
    return true;
}

bool ParseDownloadPercent(std::string_view line, float& percentOut) {
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

bool LineLooksLikePostprocess(std::string_view line) {
    return line.find("[ExtractAudio]") != std::string_view::npos ||
           line.find("[ffmpeg]") != std::string_view::npos ||
           line.find("Destination:") != std::string_view::npos ||
           line.find("Deleting original file") != std::string_view::npos;
}

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                             static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), required);
    return result;
}

std::wstring QuoteArg(std::wstring_view value) {
    std::wstring out = L"\"";
    for (const wchar_t ch : value) {
        if (ch == L'"') out += L"\\\"";
        else out.push_back(ch);
    }
    out.push_back(L'"');
    return out;
}

std::wstring FfmpegLocationArg() {
    const auto ffmpeg = YoutubeService::LocateFfmpeg();
    if (!ffmpeg) return {};
    const auto dir = ffmpeg->parent_path();
    return L" --ffmpeg-location " + QuoteArg(dir.empty() ? ffmpeg->wstring() : dir.wstring());
}

std::wstring TailWide(const std::string& text, std::size_t maxChars) {
    std::wstring wide = Utf8ToWide(text);
    while (!wide.empty() && (wide.back() == L'\n' || wide.back() == L'\r')) wide.pop_back();
    const auto pos = wide.find_last_of(L'\n');
    if (pos != std::wstring::npos) wide = wide.substr(pos + 1);
    if (wide.size() > maxChars) wide = L"..." + wide.substr(wide.size() - maxChars);
    return wide;
}

} // namespace rivan::youtube::detail
