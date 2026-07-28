// Visualization.h
// Audio-engine-independent snapshot contract and radix-2 float analyzer.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace rivan::visualization {

// Immutable-by-convention render data. Spectrum values are normalized to [0, 1]
// using a -72 dB floor; waveform samples and peak are normalized to [−1, 1]/[0, 1].
struct VisualizationSnapshot {
    std::vector<float> waveform;
    std::vector<float> spectrum;
    std::uint32_t sampleRate{};
    float peak{};
    std::uint64_t sequence{};
};

class IVisualizationSource {
public:
    virtual ~IVisualizationSource() = default;
    // Implementations must permit calls concurrent with audio submission.
    [[nodiscard]] virtual VisualizationSnapshot Snapshot() const = 0;
};

// Accepts interleaved normalized float PCM and publishes waveform plus FFT data.
// Submit is synchronous; call it from an analysis worker, not a real-time callback.
class FloatSnapshotAnalyzer final : public IVisualizationSource {
public:
    explicit FloatSnapshotAnalyzer(std::size_t fftSize = 1024);
    ~FloatSnapshotAnalyzer();
    FloatSnapshotAnalyzer(const FloatSnapshotAnalyzer&) = delete;
    FloatSnapshotAnalyzer& operator=(const FloatSnapshotAnalyzer&) = delete;
    FloatSnapshotAnalyzer(FloatSnapshotAnalyzer&&) = delete;
    FloatSnapshotAnalyzer& operator=(FloatSnapshotAnalyzer&&) = delete;

    // Downmixes channels equally and uses the newest frames. Empty or invalid input
    // publishes silence. fftSize is clamped to a power of two in [64, 8192].
    void Submit(std::span<const float> interleavedSamples,
                std::uint32_t channelCount,
                std::uint32_t sampleRate);

    [[nodiscard]] VisualizationSnapshot Snapshot() const override;
    // Fills an existing snapshot, reusing destination capacity when possible.
    void CopySnapshot(VisualizationSnapshot& out) const;
    [[nodiscard]] std::size_t FftSize() const noexcept;

    // Computes a Hann-windowed spectrum. normalizedOutput.size() must be a power of
    // two; the transform size is twice that count. Invalid sizes are filled with zero.
    static void Radix2Spectrum(std::span<const float> monoSamples,
                               std::span<float> normalizedOutput);

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace rivan::visualization
