// Rivan thread-safe recent-audio buffer for visualizers and metering.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace rivan::audio {

struct AudioAnalysisSnapshot {
    std::uint32_t sampleRate{};
    std::uint16_t channels{};
    std::uint64_t generation{};
    // Chronological, interleaved, normalized floating-point samples in [-1, 1].
    std::vector<float> samples;
};

class AudioAnalysisBuffer final {
public:
    AudioAnalysisBuffer() = default;

    AudioAnalysisBuffer(const AudioAnalysisBuffer&) = delete;
    AudioAnalysisBuffer& operator=(const AudioAnalysisBuffer&) = delete;

    void Configure(std::uint32_t sampleRate, std::uint16_t channels,
                   std::size_t capacityFrames);
    void Clear() noexcept;
    // Best-effort: drops the push if a consumer holds the buffer lock (viz only).
    void Push(std::span<const float> interleavedSamples) noexcept;

    [[nodiscard]] AudioAnalysisSnapshot Latest(std::size_t maximumFrames) const;
    // Reuses out.samples capacity when possible to avoid per-frame heap traffic.
    void LatestInto(AudioAnalysisSnapshot& out, std::size_t maximumFrames) const;
    // Cheap generation probe for consumers that only need change detection.
    [[nodiscard]] std::uint64_t Generation() const noexcept;

private:
    mutable std::mutex mutex_;
    std::vector<float> samples_;
    std::uint32_t sampleRate_{};
    std::uint16_t channels_{};
    std::size_t capacityFrames_{};
    std::size_t writeFrame_{};
    std::size_t storedFrames_{};
    std::atomic<std::uint64_t> generation_{};
};

} // namespace rivan::audio
