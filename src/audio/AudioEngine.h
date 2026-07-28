// Rivan asynchronous native audio engine public API.
#pragma once

#include "AudioAnalysisBuffer.h"
#include "AudioTypes.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>

namespace rivan::audio {

// Commands are non-blocking and execute serially on the engine's audio thread.
// Event callbacks also run on that thread: keep them short; queuing another command is safe.
class AudioEngine final {
public:
    using EventCallback = std::function<void(const AudioEvent&)>;

    explicit AudioEngine(AudioEngineOptions options = {});
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
    AudioEngine(AudioEngine&&) = delete;
    AudioEngine& operator=(AudioEngine&&) = delete;

    void Load(std::filesystem::path file);
    void Play();
    void Pause();
    void Stop();
    void Seek(std::chrono::nanoseconds position);
    void SetVolume(float volume);

    [[nodiscard]] AudioStatus Status() const;
    // Cheap UI path: atomics only (no path/string copies, no mutex).
    [[nodiscard]] LiveTransport Live() const noexcept;
    [[nodiscard]] AudioAnalysisSnapshot Analysis(std::size_t maximumFrames) const;
    // Reuses destination capacity; prefer over Analysis() on the UI timer path.
    void AnalysisInto(AudioAnalysisSnapshot& out, std::size_t maximumFrames) const;
    // Generation of the analysis ring; use before Analysis() to skip empty copies.
    [[nodiscard]] std::uint64_t AnalysisGeneration() const noexcept;
    void SetEventCallback(EventCallback callback);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rivan::audio
