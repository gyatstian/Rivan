// YoutubeService.Process.cpp
#include "YoutubeService.Internal.h"

#include <charconv>
#include <chrono>
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
                       DWORD* exitCode, ProcessLineCallback onLine,
                       std::chrono::milliseconds timeout) {
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

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    // Give the child an immediate-EOF stdin instead of inheriting the app's console
    // input. The NUL handle is non-inheritable (no SECURITY_ATTRIBUTES), so the child
    // sees an invalid stdin; fall back to the parent's stdin if NUL cannot be opened.
    HANDLE nulInput = INVALID_HANDLE_VALUE;
    {
        const HANDLE candidate = CreateFileW(L"NUL", GENERIC_READ,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                             OPEN_EXISTING, 0, nullptr);
        if (candidate != INVALID_HANDLE_VALUE) {
            startup.hStdInput = candidate;
            nulInput = candidate;
        } else {
            startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        }
    }

    // A job object so a cancel terminates the whole process tree (yt-dlp and any
    // spawned ffmpeg), not just the leader. Best-effort: if the job cannot be created
    // or assigned, continue without one.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    std::wstring commandLine = L"\"" + exe.wstring() + L"\" " + arguments;
    std::wstring environment = BuildUnbufferedEnvironment();
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        nullptr, commandLine.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(), nullptr, &startup,
        &process);
    CloseHandle(writePipe);
    writePipe = nullptr;
    if (nulInput != INVALID_HANDLE_VALUE) CloseHandle(nulInput);

    if (!created) {
        if (job) CloseHandle(job);
        CloseHandle(readPipe);
        errorText = "Unable to start yt-dlp";
        return false;
    }

    // Assign the leader to the job so TerminateJobObject on cancel kills spawned
    // children too. Best-effort: proceed without a job if assignment fails (e.g.
    // ERROR_ACCESS_DENIED on systems without nested-job support).
    if (job && !AssignProcessToJobObject(job, process.hProcess)) {
        CloseHandle(job);
        job = nullptr;
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

    bool processExited = false;
    std::chrono::steady_clock::time_point drainDeadline{};
    // A deadline that resets on every progress output, so a long but active download
    // is never killed, while a hung process is terminated after the inactivity window.
    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (std::chrono::steady_clock::now() >= deadline) {
            errorText = "yt-dlp timed out";
            TerminateProcess(process.hProcess, 1);
            if (job) TerminateJobObject(job, 1);
            WaitForSingleObject(process.hProcess, 2000);
            break;
        }
        if (stop.stop_requested()) {
            TerminateProcess(process.hProcess, 1);
            if (job) TerminateJobObject(job, 1); // kill any spawned children (ffmpeg)
            // Termination is asynchronous; wait briefly so the exit code read below
            // is not a stale STILL_ACTIVE (259).
            WaitForSingleObject(process.hProcess, 2000);
            break;
        }
        DWORD available = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr)) {
            // ERROR_BROKEN_PIPE / ERROR_NO_DATA: write end closed, drain complete.
            break;
        }
        if (available > 0) {
            DWORD read = 0;
            if (!ReadFile(readPipe, chunk, sizeof(chunk), &read, nullptr) || read == 0) break;
            buffer.append(chunk, chunk + read);
            feedLines(std::string_view(chunk, read));
            // Activity keeps the child alive; only a silent process hits the deadline.
            deadline = std::chrono::steady_clock::now() + timeout;
            if (buffer.size() > 8 * 1024 * 1024) {
                // Don't kill a healthy long download: keep the tail and continue.
                // Callers read the retained tail via TailWide for error diagnostics,
                // progress lines stream through onLine, and Probe output is far below
                // this cap, so the trimming cannot corrupt boundary parsing.
                buffer.erase(0, buffer.size() / 2);
            }
            continue;
        }
        if (!processExited) {
            const DWORD wait = WaitForSingleObject(process.hProcess, 100);
            if (wait == WAIT_OBJECT_0) {
                processExited = true;
                drainDeadline =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
            } else if (wait == WAIT_FAILED || wait == WAIT_ABANDONED) {
                errorText = "Unable to wait for yt-dlp process";
                break;
            }
            continue;
        }
        // Process exited. Drain any remaining pipe data with a bounded deadline so a
        // grandchild that inherited the write handle cannot stall this worker forever.
        if (std::chrono::steady_clock::now() >= drainDeadline) break;
        Sleep(5);
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
    // KILL_ON_JOB_CLOSE means any process still in the job dies with this handle;
    // by now the leader has exited or been terminated (and waited for), so closing
    // here is safe and also cleans up on early-exit error paths.
    if (job) CloseHandle(job);

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
    std::wstring out;
    out.reserve(value.size() + 8);
    out.push_back(L'"');
    std::size_t backslashRun = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashRun;
            continue;
        }
        if (ch == L'"') {
            // CommandLineToArgvW: an even run of backslashes before a quote yields
            // half as many literal backslashes, then the quote acts as a delimiter;
            // an odd run yields (n-1)/2 backslashes and an escaped literal quote.
            // Doubling the run before every quote makes the quote literal and keeps
            // the argument intact.
            out.append(backslashRun * 2, L'\\');
            out += L"\\\"";
            backslashRun = 0;
            continue;
        }
        out.append(backslashRun, L'\\');
        backslashRun = 0;
        out.push_back(ch);
    }
    // Trailing run before the closing quote must also be doubled so an odd count
    // does not escape the closing quote.
    out.append(backslashRun * 2, L'\\');
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
