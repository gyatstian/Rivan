// Rivan native audio public types.
// This header is platform-neutral; Windows multimedia details stay in the implementation.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace rivan::audio {

enum class PlaybackState {
    Initializing,
    Idle,
    Loading,
    Stopped,
    Playing,
    Paused,
    Ended,
    Error,
    ShuttingDown,
};

struct AudioError {
    std::int64_t nativeCode{}; // HRESULT on Windows, widened without losing its bit pattern.
    std::string message;
};

struct AudioStatus {
    PlaybackState state{PlaybackState::Initializing};
    std::filesystem::path source;
    std::chrono::nanoseconds position{};
    std::chrono::nanoseconds duration{};
    float volume{1.0F};
    bool hasMedia{false};
    AudioError error;
};

// Lock-free transport snapshot for UI timer paints (no path / error strings).
struct LiveTransport {
    PlaybackState state{PlaybackState::Initializing};
    std::chrono::nanoseconds position{};
    std::chrono::nanoseconds duration{};
    float volume{1.0F};
    bool hasMedia{false};
};

enum class AudioEventType {
    StateChanged,
    Loaded,
    EndOfStream,
    Error,
};

struct AudioEvent {
    AudioEventType type{AudioEventType::StateChanged};
    AudioStatus status;
};

struct AudioEngineOptions {
    // Decoded PCM is bounded to this much audio (clamped to 0.25-8 seconds).
    std::chrono::milliseconds decodedBufferDuration{2000};
    // Recent normalized samples retained for visualizers (clamped to 0.1-30 seconds).
    // Default ~100 ms covers FFT-1024 at common rates without retaining multi-second history.
    std::chrono::milliseconds analysisDuration{100};
};

} // namespace rivan::audio
