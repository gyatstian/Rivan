// Visualization.cpp
// Thread-safe downmixing and iterative radix-2 FFT implementation.
#include "Visualization.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <mutex>
#include <numbers>

namespace rivan::visualization {
namespace {

[[nodiscard]] std::size_t ClampFftSize(std::size_t requested) noexcept {
    requested = std::clamp<std::size_t>(requested, 64, 8192);
    std::size_t power = 1;
    while ((power << 1U) <= requested) power <<= 1U;
    return power;
}

[[nodiscard]] bool IsPowerOfTwo(std::size_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

void Radix2Fft(std::vector<std::complex<float>>& values,
               std::span<const std::complex<float>> stageTwiddles) noexcept {
    const std::size_t count = values.size();
    for (std::size_t i = 1, j = 0; i < count; ++i) {
        std::size_t bit = count >> 1U;
        while ((j & bit) != 0) {
            j ^= bit;
            bit >>= 1U;
        }
        j ^= bit;
        if (i < j) std::swap(values[i], values[j]);
    }

    std::size_t twiddleBase = 0;
    for (std::size_t length = 2; length <= count; length <<= 1U) {
        const std::size_t half = length >> 1U;
        for (std::size_t base = 0; base < count; base += length) {
            for (std::size_t index = 0; index < half; ++index) {
                const auto even = values[base + index];
                const auto odd = values[base + index + half] * stageTwiddles[twiddleBase + index];
                values[base + index] = even + odd;
                values[base + index + half] = even - odd;
            }
        }
        twiddleBase += half;
    }
}

void BuildTwiddleTable(std::size_t transformSize,
                       std::vector<std::complex<float>>& twiddles) {
    twiddles.clear();
    std::size_t total = 0;
    for (std::size_t length = 2; length <= transformSize; length <<= 1U) {
        total += length >> 1U;
    }
    twiddles.reserve(total);
    for (std::size_t length = 2; length <= transformSize; length <<= 1U) {
        const float angle = -2.0F * std::numbers::pi_v<float> / static_cast<float>(length);
        const std::complex<float> step{std::cos(angle), std::sin(angle)};
        const std::size_t half = length >> 1U;
        std::complex<float> twiddle{1.0F, 0.0F};
        for (std::size_t index = 0; index < half; ++index) {
            twiddles.push_back(twiddle);
            twiddle *= step;
        }
    }
}

void BuildHannWindow(std::size_t transformSize, std::vector<float>& window) {
    window.resize(transformSize);
    if (transformSize == 0) {
        return;
    }
    if (transformSize == 1) {
        window[0] = 1.0F;
        return;
    }
    const float denominator = static_cast<float>(transformSize - 1U);
    for (std::size_t index = 0; index < transformSize; ++index) {
        window[index] = 0.5F - 0.5F * std::cos(
            2.0F * std::numbers::pi_v<float> * static_cast<float>(index) / denominator);
    }
}

void ComputeSpectrum(std::span<const float> monoSamples, std::span<float> output,
                     std::vector<std::complex<float>>& bins,
                     std::span<const float> hannWindow,
                     std::span<const std::complex<float>> twiddles) {
    std::fill(output.begin(), output.end(), 0.0F);
    if (!IsPowerOfTwo(output.size()) || hannWindow.size() != output.size() * 2U) {
        return;
    }

    const std::size_t transformSize = output.size() * 2U;
    bins.resize(transformSize);
    const std::size_t copyCount = std::min(transformSize, monoSamples.size());
    const std::size_t sourceStart = monoSamples.size() - copyCount;
    const std::size_t destinationStart = transformSize - copyCount;

    for (std::size_t index = 0; index < transformSize; ++index) {
        const float sample = index >= destinationStart
            ? std::clamp(monoSamples[sourceStart + index - destinationStart], -1.0F, 1.0F)
            : 0.0F;
        bins[index] = {sample * hannWindow[index], 0.0F};
    }

    Radix2Fft(bins, twiddles);
    const float scale = 2.0F / static_cast<float>(transformSize);
    constexpr float noiseFloorAmplitude = 0.00025118864F; // -72 dBFS
    for (std::size_t index = 0; index < output.size(); ++index) {
        const float amplitude = std::abs(bins[index]) * scale;
        const float decibels = 20.0F * std::log10(std::max(amplitude, noiseFloorAmplitude));
        output[index] = std::clamp((decibels + 72.0F) / 72.0F, 0.0F, 1.0F);
    }
}

void ComputeSpectrum(std::span<const float> monoSamples, std::span<float> output) {
    if (!IsPowerOfTwo(output.size())) {
        std::fill(output.begin(), output.end(), 0.0F);
        return;
    }
    const std::size_t transformSize = output.size() * 2U;
    std::vector<float> window;
    std::vector<std::complex<float>> twiddles;
    std::vector<std::complex<float>> bins;
    BuildHannWindow(transformSize, window);
    BuildTwiddleTable(transformSize, twiddles);
    ComputeSpectrum(monoSamples, output, bins, window, twiddles);
}

} // namespace

struct FloatSnapshotAnalyzer::State {
    explicit State(std::size_t requestedSize)
        : fftSize(ClampFftSize(requestedSize)),
          waveform(fftSize, 0.0F), spectrum(fftSize / 2U, 0.0F),
          workWaveform(fftSize, 0.0F), workSpectrum(fftSize / 2U, 0.0F) {
        workBins.reserve(fftSize);
        BuildHannWindow(fftSize, hannWindow);
        BuildTwiddleTable(fftSize, twiddles);
    }

    const std::size_t fftSize;
    mutable std::mutex mutex;
    std::vector<float> waveform;
    std::vector<float> spectrum;
    std::vector<float> workWaveform;
    std::vector<float> workSpectrum;
    std::vector<std::complex<float>> workBins;
    std::vector<float> hannWindow;
    std::vector<std::complex<float>> twiddles;
    std::uint32_t sampleRate{};
    float peak{};
    std::uint64_t sequence{};
};

FloatSnapshotAnalyzer::FloatSnapshotAnalyzer(std::size_t fftSize)
    : state_(std::make_unique<State>(fftSize)) {}

FloatSnapshotAnalyzer::~FloatSnapshotAnalyzer() = default;

void FloatSnapshotAnalyzer::Submit(std::span<const float> samples,
                                   std::uint32_t channelCount,
                                   std::uint32_t sampleRate) {
    auto& workWaveform = state_->workWaveform;
    auto& workSpectrum = state_->workSpectrum;
    float peak = 0.0F;

    if (channelCount != 0 && !samples.empty()) {
        const std::size_t frameCount = samples.size() / channelCount;
        const std::size_t copiedFrames = std::min(frameCount, state_->fftSize);
        const std::size_t sourceStart = frameCount - copiedFrames;
        const std::size_t destinationStart = state_->fftSize - copiedFrames;
        // Only clear the unused head; tail is fully overwritten below.
        if (destinationStart != 0) {
            std::fill_n(workWaveform.begin(), destinationStart, 0.0F);
        }
        for (std::size_t frame = 0; frame < copiedFrames; ++frame) {
            float mono = 0.0F;
            for (std::uint32_t channel = 0; channel < channelCount; ++channel) {
                mono += samples[(sourceStart + frame) * channelCount + channel];
            }
            mono = std::clamp(mono / static_cast<float>(channelCount), -1.0F, 1.0F);
            workWaveform[destinationStart + frame] = mono;
            peak = std::max(peak, std::abs(mono));
        }
    } else {
        std::fill(workWaveform.begin(), workWaveform.end(), 0.0F);
    }

    workSpectrum.resize(state_->fftSize / 2U);
    ComputeSpectrum(workWaveform, workSpectrum, state_->workBins, state_->hannWindow,
                    state_->twiddles);

    std::scoped_lock lock(state_->mutex);
    state_->waveform.swap(workWaveform);
    state_->spectrum.swap(workSpectrum);
    state_->sampleRate = sampleRate;
    state_->peak = peak;
    ++state_->sequence;
}

VisualizationSnapshot FloatSnapshotAnalyzer::Snapshot() const {
    std::scoped_lock lock(state_->mutex);
    return {state_->waveform, state_->spectrum, state_->sampleRate,
            state_->peak, state_->sequence};
}

void FloatSnapshotAnalyzer::CopySnapshot(VisualizationSnapshot& out) const {
    std::scoped_lock lock(state_->mutex);
    if (out.sequence == state_->sequence &&
        out.waveform.size() == state_->waveform.size() &&
        out.spectrum.size() == state_->spectrum.size()) {
        // Same published frame; skip vector assignment.
        out.sampleRate = state_->sampleRate;
        out.peak = state_->peak;
        return;
    }
    out.waveform = state_->waveform;
    out.spectrum = state_->spectrum;
    out.sampleRate = state_->sampleRate;
    out.peak = state_->peak;
    out.sequence = state_->sequence;
}

std::size_t FloatSnapshotAnalyzer::FftSize() const noexcept { return state_->fftSize; }

void FloatSnapshotAnalyzer::Radix2Spectrum(std::span<const float> monoSamples,
                                           std::span<float> normalizedOutput) {
    ComputeSpectrum(monoSamples, normalizedOutput);
}

} // namespace rivan::visualization
