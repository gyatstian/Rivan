// DiscordPresence.cpp
// Discord IPC client: connect to discord-ipc-N, HANDSHAKE, SET_ACTIVITY.
// Failures are silent; reconnect is best-effort so Discord absence never blocks audio.
#include "DiscordPresence.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <chrono>
#include <string_view>
#include <utility>

namespace rivan::discord {
namespace {

constexpr std::uint32_t kOpcodeHandshake = 0;
constexpr std::uint32_t kOpcodeFrame = 1;
constexpr std::uint32_t kOpcodeClose = 2;
constexpr std::uint32_t kOpcodePing = 3;
constexpr std::uint32_t kOpcodePong = 4;

constexpr DWORD kPipeBuffer = 64 * 1024;
constexpr std::string_view kDiscordApplicationId = "1529189289429172324";
constexpr std::string_view kGithubRepoUrl = "https://github.com/gyatstian/Rivan";

std::string EscapeJson(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const unsigned char ch : text) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[(ch >> 4) & 0xF]);
                out.push_back(hex[ch & 0xF]);
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return out;
}

std::string TruncateUtf8(std::string text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) return text;
    while (maxBytes > 0 && (static_cast<unsigned char>(text[maxBytes]) & 0xC0) == 0x80) {
        --maxBytes;
    }
    text.resize(maxBytes);
    return text;
}

} // namespace

DiscordPresence::DiscordPresence() {
    worker_ = std::jthread([this](std::stop_token stop) { WorkerMain(std::move(stop)); });
}

DiscordPresence::~DiscordPresence() {
    {
        std::lock_guard lock(mutex_);
        enabled_.store(false, std::memory_order_release);
        clearRequested_ = true;
        dirty_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    Disconnect();
}

void DiscordPresence::SetEnabled(bool enabled) {
    {
        std::lock_guard lock(mutex_);
        if (enabled_.exchange(enabled, std::memory_order_acq_rel) == enabled) return;
        if (!enabled) clearRequested_ = true;
        dirty_ = true;
    }
    cv_.notify_all();
}

void DiscordPresence::SetActivity(PresenceActivity activity) {
    activity.details = TruncateUtf8(std::move(activity.details), 128);
    activity.state = TruncateUtf8(std::move(activity.state), 128);
    {
        std::lock_guard lock(mutex_);
        pending_ = std::move(activity);
        clearRequested_ = false;
        dirty_ = true;
    }
    cv_.notify_all();
}

void DiscordPresence::Clear() {
    {
        std::lock_guard lock(mutex_);
        pending_ = {};
        clearRequested_ = true;
        dirty_ = true;
    }
    cv_.notify_all();
}

void DiscordPresence::Disconnect() noexcept {
    if (pipe_ != nullptr && pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(pipe_));
    }
    pipe_ = nullptr;
}

bool DiscordPresence::WriteFrame(std::uint32_t opcode, const std::string& payload) {
    if (pipe_ == nullptr || pipe_ == INVALID_HANDLE_VALUE) return false;
    const auto handle = static_cast<HANDLE>(pipe_);
    std::uint32_t header[2] = {opcode, static_cast<std::uint32_t>(payload.size())};
    DWORD written = 0;
    if (!WriteFile(handle, header, sizeof(header), &written, nullptr) ||
        written != sizeof(header)) {
        return false;
    }
    if (payload.empty()) return true;
    written = 0;
    return WriteFile(handle, payload.data(), static_cast<DWORD>(payload.size()), &written,
                     nullptr) &&
           written == payload.size();
}

bool DiscordPresence::ReadFrame(std::uint32_t& opcode, std::string& payload,
                                std::uint32_t timeoutMs) {
    if (pipe_ == nullptr || pipe_ == INVALID_HANDLE_VALUE) return false;
    const auto handle = static_cast<HANDLE>(pipe_);

    // Named pipes ignore COMMTIMEOUTS; use PeekNamedPipe for non-blocking drain.
    if (timeoutMs == 0) {
        DWORD available = 0;
        if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr) ||
            available < sizeof(std::uint32_t) * 2) {
            return false;
        }
    }

    std::uint32_t header[2]{};
    DWORD read = 0;
    if (!ReadFile(handle, header, sizeof(header), &read, nullptr) || read != sizeof(header)) {
        return false;
    }
    opcode = header[0];
    const std::uint32_t length = header[1];
    if (length > kPipeBuffer) return false;
    payload.assign(length, '\0');
    if (length == 0) return true;
    read = 0;
    return ReadFile(handle, payload.data(), length, &read, nullptr) && read == length;
}

bool DiscordPresence::Handshake() {
    const std::string payload = R"({"v":1,"client_id":")" +
                                std::string(kDiscordApplicationId) + R"("})";
    if (!WriteFrame(kOpcodeHandshake, payload)) return false;

    std::uint32_t opcode = 0;
    std::string response;
    if (!ReadFrame(opcode, response, 3000)) return false;
    if (opcode == kOpcodeClose) return false;
    return opcode == kOpcodeFrame || opcode == kOpcodePong;
}

bool DiscordPresence::EnsureConnected(std::stop_token stop) {
    if (pipe_ != nullptr && pipe_ != INVALID_HANDLE_VALUE) return true;

    for (int index = 0; index < 10 && !stop.stop_requested(); ++index) {
        const std::wstring path = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(index);
        const HANDLE handle =
            CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) continue;
        pipe_ = handle;
        if (Handshake()) return true;
        Disconnect();
    }
    return false;
}

void DiscordPresence::DrainIncoming() {
    for (;;) {
        std::uint32_t opcode = 0;
        std::string payload;
        if (!ReadFrame(opcode, payload, 0)) break;
        if (opcode == kOpcodeClose) {
            Disconnect();
            break;
        }
        if (opcode == kOpcodePing) {
            (void)WriteFrame(kOpcodePong, payload);
        }
    }
}

bool DiscordPresence::PublishActivity(const PresenceActivity& activity) {
    const std::uint64_t nonce = ++nonce_;
    std::string payload;
    payload.reserve(384);

    if (!activity.hasTrack) {
        payload = R"({"cmd":"SET_ACTIVITY","args":{"pid":)";
        payload += std::to_string(GetCurrentProcessId());
        payload += R"(},"nonce":")";
        payload += std::to_string(nonce);
        payload += R"("})";
    } else {
        const std::string details =
            EscapeJson(activity.details.empty() ? "Listening" : activity.details);

        payload = R"({"cmd":"SET_ACTIVITY","args":{"pid":)";
        payload += std::to_string(GetCurrentProcessId());
        payload += R"(,"activity":{"type":2,"details":")";
        payload += details;
        payload += '"';
        if (!activity.state.empty()) {
            payload += R"(,"state":")";
            payload += EscapeJson(activity.state);
            payload += '"';
        }
        // Use the uploaded application asset; Discord does not reliably resolve external image URLs.
        payload += R"(,"assets":{"large_image":"rivan")";
        if (activity.showImageText) payload += R"(,"large_text":"Rivan")";
        payload += '}';
        if (activity.playing && activity.startUnix > 0) {
            payload += R"(,"timestamps":{"start":)";
            payload += std::to_string(activity.startUnix);
            if (activity.endUnix > activity.startUnix) {
                payload += R"(,"end":)";
                payload += std::to_string(activity.endUnix);
            }
            payload += '}';
        }
        if (activity.showGithubButton) {
            payload += R"(,"buttons":[{"label":"Rivan","url":")";
            payload += EscapeJson(kGithubRepoUrl);
            payload += R"("}])";
        }
        payload += R"(}},"nonce":")";
        payload += std::to_string(nonce);
        payload += R"("})";
    }

    if (!WriteFrame(kOpcodeFrame, payload)) return false;
    DrainIncoming();
    return pipe_ != nullptr && pipe_ != INVALID_HANDLE_VALUE;
}

void DiscordPresence::WorkerMain(std::stop_token stop) {
    PresenceActivity local{};
    bool needClear = false;
    auto nextRetry = std::chrono::steady_clock::now();

    while (!stop.stop_requested()) {
        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, stop, std::chrono::seconds(2),
                         [this] { return dirty_; });
            if (stop.stop_requested()) break;

            if (dirty_) {
                local = pending_;
                needClear = clearRequested_ || !enabled_.load(std::memory_order_relaxed);
                dirty_ = false;
                clearRequested_ = false;
            } else if (!enabled_.load(std::memory_order_relaxed)) {
                continue;
            } else if (pipe_ != nullptr) {
                // Keepalive drain while idle with an open pipe.
                lock.unlock();
                DrainIncoming();
                continue;
            } else if (std::chrono::steady_clock::now() < nextRetry) {
                continue;
            }
        }

        if (!enabled_.load(std::memory_order_acquire)) {
            if (pipe_ != nullptr && pipe_ != INVALID_HANDLE_VALUE) {
                PresenceActivity empty{};
                (void)PublishActivity(empty);
                Disconnect();
            }
            needClear = false;
            continue;
        }

        if (!EnsureConnected(stop)) {
            nextRetry = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            continue;
        }

        PresenceActivity publish = needClear ? PresenceActivity{} : local;
        needClear = false;
        if (!PublishActivity(publish)) {
            Disconnect();
            nextRetry = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            std::lock_guard lock(mutex_);
            dirty_ = true;
        }
    }

    if (pipe_ != nullptr && pipe_ != INVALID_HANDLE_VALUE) {
        PresenceActivity empty{};
        (void)PublishActivity(empty);
        Disconnect();
    }
}

} // namespace rivan::discord
