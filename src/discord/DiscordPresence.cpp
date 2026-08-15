// DiscordPresence.cpp
// Discord IPC client: connect to discord-ipc-N, HANDSHAKE, SET_ACTIVITY.
// Failures are silent; reconnect is best-effort so Discord absence never blocks audio.
#include "DiscordPresence.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
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
// Discord IPC is local. A connected pipe should answer quickly; most importantly,
// this is one total budget for the complete frame, not a separate budget for its
// header and payload.
constexpr auto kHandshakeTimeout = std::chrono::seconds{3};
constexpr DWORD kPipePollMilliseconds = 10;
// Discord documents a maximum of five Activity updates per 20 seconds. Enforce that
// budget locally as a sliding window so Discord never has to queue our requests. This
// still allows a burst of urgent track/lyric changes, then waits only when the actual
// documented budget is exhausted.
constexpr std::size_t kActivityBurstLimit = 5;
constexpr auto kActivityRateWindow = std::chrono::seconds{20};

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
        current_ = {};
        pendingTrack_.reset();
        pendingLyric_.reset();
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
        if (!enabled) {
            // Do not resurrect a pre-disable activity if Discord is enabled later.
            current_ = {};
            pendingTrack_.reset();
            pendingLyric_.reset();
            clearRequested_ = true;
        }
        dirty_ = true;
    }
    cv_.notify_all();
}

void DiscordPresence::SetActivity(PresenceActivity activity, const ActivityUpdateKind kind) {
    activity.details = TruncateUtf8(std::move(activity.details), 128);
    activity.state = TruncateUtf8(std::move(activity.state), 128);
    {
        std::lock_guard lock(mutex_);
        if (!activity.hasTrack) {
            current_ = {};
            pendingTrack_.reset();
            pendingLyric_.reset();
            clearRequested_ = true;
            dirty_ = true;
        } else {
            current_ = activity;
            if (kind == ActivityUpdateKind::Track) {
                pendingTrack_ = std::move(activity);
                pendingLyric_.reset();
            } else if (pendingTrack_) {
                // Initial track publish has not reached Discord yet. Fold newest verse
                // into it rather than publishing stale track text then a second update.
                pendingTrack_ = std::move(activity);
            } else {
                pendingLyric_ = std::move(activity);
            }
            clearRequested_ = false;
            dirty_ = true;
        }
    }
    cv_.notify_all();
}

void DiscordPresence::Clear() {
    {
        std::lock_guard lock(mutex_);
        current_ = {};
        pendingTrack_.reset();
        pendingLyric_.reset();
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
                                const std::chrono::steady_clock::time_point deadline,
                                const std::stop_token stop) {
    if (pipe_ == nullptr || pipe_ == INVALID_HANDLE_VALUE) return false;
    const auto handle = static_cast<HANDLE>(pipe_);
    const auto waitAvailable = [&](std::uint32_t needed) {
        if (deadline <= std::chrono::steady_clock::now()) {
            DWORD available = 0;
            return PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr) != FALSE &&
                   available >= needed;
        }
        for (;;) {
            if (stop.stop_requested()) return false;
            DWORD available = 0;
            if (PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr) == FALSE) {
                return false;
            }
            if (available >= needed) return true;
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return false;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - now).count();
            Sleep(static_cast<DWORD>((std::min)(
                static_cast<std::int64_t>(kPipePollMilliseconds), (std::max)(1LL, remaining))));
        }
    };

    if (!waitAvailable(sizeof(std::uint32_t) * 2)) return false;

    std::uint32_t header[2]{};
    DWORD available = 0;
    if (!PeekNamedPipe(handle, header, sizeof(header), nullptr, &available, nullptr) ||
        available < sizeof(header)) {
        return false;
    }
    const std::uint32_t length = header[1];
    if (length > kPipeBuffer) return false;
    if (length != 0 && !waitAvailable(sizeof(header) + length)) return false;

    std::string frame(sizeof(header) + length, '\0');
    DWORD read = 0;
    if (!ReadFile(handle, frame.data(), static_cast<DWORD>(frame.size()), &read, nullptr) ||
        read != frame.size()) {
        return false;
    }
    std::memcpy(header, frame.data(), sizeof(header));
    opcode = header[0];
    payload.assign(frame.data() + sizeof(header), frame.data() + frame.size());
    return true;
}

bool DiscordPresence::Handshake(const std::chrono::steady_clock::time_point deadline,
                                const std::stop_token stop) {
    const std::string payload = R"({"v":1,"client_id":")" +
                                std::string(kDiscordApplicationId) + R"("})";
    if (stop.stop_requested() || !WriteFrame(kOpcodeHandshake, payload)) return false;

    std::uint32_t opcode = 0;
    std::string response;
    if (!ReadFrame(opcode, response, deadline, stop)) return false;
    if (opcode == kOpcodeClose) return false;
    return opcode == kOpcodeFrame || opcode == kOpcodePong;
}

bool DiscordPresence::EnsureConnected(std::stop_token stop) {
    if (pipe_ != nullptr && pipe_ != INVALID_HANDLE_VALUE) return true;

    const auto deadline = std::chrono::steady_clock::now() + kHandshakeTimeout;
    for (int index = 0; index < 10 && !stop.stop_requested() &&
         std::chrono::steady_clock::now() < deadline; ++index) {
        const std::wstring path = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(index);
        const HANDLE handle =
            CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) continue;
        pipe_ = handle;
        if (Handshake(deadline, stop)) return true;
        Disconnect();
    }
    return false;
}

void DiscordPresence::DrainIncoming() {
    for (;;) {
        std::uint32_t opcode = 0;
        std::string payload;
        if (!ReadFrame(opcode, payload, std::chrono::steady_clock::now())) {
            // ReadFrame(0) fails for both 'nothing buffered' (normal after a publish
            // or during idle keepalive) and 'pipe broken'. Only drop the handle when
            // the pipe itself actually failed; otherwise keepalive would disconnect a
            // healthy connection after every publish and never stabilize.
            if (pipe_ != nullptr && pipe_ != INVALID_HANDLE_VALUE) {
                DWORD available = 0;
                if (PeekNamedPipe(static_cast<HANDLE>(pipe_), nullptr, 0, nullptr,
                                  &available, nullptr) == FALSE) {
                    Disconnect();
                }
            }
            break;
        }
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
        if (!activity.imageText.empty()) {
            payload += R"(,"large_text":")";
            payload += EscapeJson(TruncateUtf8(activity.imageText, 128));
            payload += '"';
        }
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
    auto retryAt = std::chrono::steady_clock::now();
    bool republishAfterReconnect = false;
    std::deque<std::chrono::steady_clock::time_point> activitySentAt;

    while (!stop.stop_requested()) {
        if (!enabled_.load(std::memory_order_acquire)) {
            bool publishClear = false;
            {
                std::scoped_lock lock(mutex_);
                publishClear = clearRequested_;
                clearRequested_ = false;
                dirty_ = false;
            }
            if (pipe_ != nullptr && pipe_ != INVALID_HANDLE_VALUE) {
                if (publishClear) {
                    PresenceActivity empty{};
                    (void)PublishActivity(empty);
                }
                Disconnect();
            }
            republishAfterReconnect = false;
            retryAt = std::chrono::steady_clock::now();

            std::unique_lock lock(mutex_);
            cv_.wait(lock, stop, [this] {
                return enabled_.load(std::memory_order_relaxed);
            });
            continue;
        }

        if (pipe_ == nullptr || pipe_ == INVALID_HANDLE_VALUE) {
            if (std::chrono::steady_clock::now() < retryAt) {
                std::unique_lock lock(mutex_);
                const bool activityArrived = cv_.wait_until(lock, stop, retryAt, [this] {
                    return !enabled_.load(std::memory_order_relaxed) || dirty_;
                });
                if (activityArrived && enabled_.load(std::memory_order_relaxed)) {
                    retryAt = std::chrono::steady_clock::now();
                }
                continue;
            }

            bool waitForActivity = false;
            {
                std::scoped_lock lock(mutex_);
                if (clearRequested_ || !current_.hasTrack) {
                    dirty_ = false;
                    clearRequested_ = false;
                    pendingTrack_.reset();
                    pendingLyric_.reset();
                    republishAfterReconnect = false;
                    waitForActivity = true;
                }
            }
            if (waitForActivity) {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, stop, [this] {
                    return !enabled_.load(std::memory_order_relaxed) ||
                           (dirty_ && !clearRequested_ && current_.hasTrack);
                });
                continue;
            }

            if (!EnsureConnected(stop)) {
                retryAt = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                republishAfterReconnect = true;
                continue;
            }
            republishAfterReconnect = true;
        }

        PresenceActivity activity;
        bool publish = republishAfterReconnect;
        bool clear = false;
        ActivityUpdateKind updateKind = ActivityUpdateKind::Track;
        {
            std::unique_lock lock(mutex_);
            if (!enabled_.load(std::memory_order_relaxed)) continue;
            if (clearRequested_) {
                activity = {};
                current_ = {};
                pendingTrack_.reset();
                pendingLyric_.reset();
                clearRequested_ = false;
                dirty_ = false;
                publish = true;
                clear = true;
            } else if (pendingTrack_) {
                activity = std::move(*pendingTrack_);
                pendingTrack_.reset();
                pendingLyric_.reset();
                dirty_ = false;
                publish = true;
                updateKind = ActivityUpdateKind::Track;
            } else if (publish) {
                activity = current_;
                clear = !activity.hasTrack;
                pendingLyric_.reset();
                dirty_ = false;
            } else if (pendingLyric_) {
                activity = std::move(*pendingLyric_);
                pendingLyric_.reset();
                dirty_ = false;
                publish = true;
                updateKind = ActivityUpdateKind::Lyric;
            } else {
                dirty_ = false;
            }
        }

        if (publish) {
            const auto now = std::chrono::steady_clock::now();
            while (!activitySentAt.empty() &&
                   now - activitySentAt.front() >= kActivityRateWindow) {
                activitySentAt.pop_front();
            }
            const auto due = activitySentAt.size() >= kActivityBurstLimit
                ? activitySentAt.front() + kActivityRateWindow
                : std::chrono::steady_clock::time_point::min();
            if (due != std::chrono::steady_clock::time_point::min() && now < due) {
                // Keep only latest desired state while the documented client budget is
                // exhausted. A newer track supersedes any selected lyric on wake.
                const auto waitUntil = due;
                const auto selectedKind = updateKind;
                std::unique_lock lock(mutex_);
                // Do not wake early for a new value. No request can be accepted before
                // the window opens; waking early would only spin while clearRequested_
                // remains set. The pending slots already retain newer values.
                (void)cv_.wait_until(lock, stop, waitUntil, [this] {
                    return !enabled_.load(std::memory_order_relaxed);
                });
                if (!stop.stop_requested() && enabled_.load(std::memory_order_relaxed) &&
                    !clearRequested_) {
                    // Do not overwrite a newer value that arrived while waiting.
                    if (selectedKind == ActivityUpdateKind::Track) {
                        if (!pendingTrack_) pendingTrack_ = std::move(activity);
                    } else if (!pendingLyric_) {
                        pendingLyric_ = std::move(activity);
                    }
                    dirty_ = pendingTrack_.has_value() || pendingLyric_.has_value();
                } else if (!stop.stop_requested() && enabled_.load(std::memory_order_relaxed) &&
                           clearRequested_) {
                    dirty_ = true;
                }
                continue;
            }
        }

        if (!publish) {
            std::unique_lock lock(mutex_);
            const bool changed = cv_.wait_for(lock, stop, std::chrono::seconds(2), [this] {
                return dirty_ || !enabled_.load(std::memory_order_relaxed);
            });
            const bool drain = !changed && enabled_.load(std::memory_order_relaxed);
            lock.unlock();
            if (drain) {
                DrainIncoming();
                if (pipe_ == nullptr || pipe_ == INVALID_HANDLE_VALUE) {
                    retryAt = std::chrono::steady_clock::now();
                    republishAfterReconnect = true;
                }
            }
            continue;
        }

        // A track switch must never wait behind a lyric selected moments earlier. Also
        // replace a selected lyric with its newest pending value before writing IPC.
        {
            std::unique_lock lock(mutex_);
            if (!enabled_.load(std::memory_order_relaxed)) continue;
            if (clearRequested_) continue;
            if (pendingTrack_) {
                activity = std::move(*pendingTrack_);
                pendingTrack_.reset();
                pendingLyric_.reset();
                dirty_ = false;
                clear = false;
                updateKind = ActivityUpdateKind::Track;
            } else if (pendingLyric_) {
                activity = std::move(*pendingLyric_);
                pendingLyric_.reset();
                dirty_ = false;
                clear = false;
                updateKind = ActivityUpdateKind::Lyric;
            }
        }
        if (!PublishActivity(clear ? PresenceActivity{} : activity)) {
            Disconnect();
            retryAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            republishAfterReconnect = !clear;
            continue;
        }
        activitySentAt.push_back(std::chrono::steady_clock::now());
        republishAfterReconnect = false;
    }

    if (pipe_ != nullptr && pipe_ != INVALID_HANDLE_VALUE) {
        PresenceActivity empty{};
        (void)PublishActivity(empty);
        Disconnect();
    }
}

} // namespace rivan::discord
