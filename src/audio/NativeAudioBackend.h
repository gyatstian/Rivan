// Rivan internal Media Foundation decoder and event-driven WASAPI renderer.
#pragma once

#include "AudioAnalysisBuffer.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace rivan::audio::detail {

class NativeAudioException final : public std::runtime_error {
public:
    NativeAudioException(std::int64_t code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}
    [[nodiscard]] std::int64_t Code() const noexcept { return code_; }
private:
    std::int64_t code_;
};

struct RenderResult {
    bool drained{};
};

// Every method except RenderEvent() is called only by the owning audio thread.
class NativeAudioBackend final {
public:
    NativeAudioBackend(AudioAnalysisBuffer& analysis,
                       std::chrono::milliseconds decodedDuration,
                       std::chrono::milliseconds analysisDuration);
    ~NativeAudioBackend();

    NativeAudioBackend(const NativeAudioBackend&) = delete;
    NativeAudioBackend& operator=(const NativeAudioBackend&) = delete;

    void Initialize();
    void Shutdown() noexcept;
    void Open(const std::filesystem::path& file);
    void Close() noexcept;

    void PumpDecoded(std::size_t maximumReads);
    [[nodiscard]] RenderResult Render();
    void Start();
    void Pause();
    void Seek(std::chrono::nanoseconds position);
    void SetVolume(float volume);

    [[nodiscard]] std::chrono::nanoseconds Duration() const noexcept;
    [[nodiscard]] std::chrono::nanoseconds PlaybackElapsed() const noexcept;
    [[nodiscard]] bool HasMedia() const noexcept;
    [[nodiscard]] void* RenderEvent() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rivan::audio::detail
