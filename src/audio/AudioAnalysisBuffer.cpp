// Rivan thread-safe recent-audio buffer implementation.
#include "AudioAnalysisBuffer.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace rivan::audio {

void AudioAnalysisBuffer::Configure(const std::uint32_t sampleRate,
                                    const std::uint16_t channels,
                                    const std::size_t capacityFrames) {
    std::scoped_lock lock(mutex_);
    if (channels != 0 &&
        capacityFrames > (std::numeric_limits<std::size_t>::max)() / channels) {
        throw std::length_error("Audio analysis capacity is too large");
    }

    sampleRate_ = sampleRate;
    channels_ = channels;
    capacityFrames_ = capacityFrames;
    samples_.assign(capacityFrames_ * channels_, 0.0F);
    writeFrame_ = 0;
    storedFrames_ = 0;
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

void AudioAnalysisBuffer::Clear() noexcept {
    std::scoped_lock lock(mutex_);
    writeFrame_ = 0;
    storedFrames_ = 0;
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

void AudioAnalysisBuffer::Push(std::span<const float> input) noexcept {
    // Avoid blocking the audio thread if the UI is copying a snapshot.
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;
    }
    if (channels_ == 0 || capacityFrames_ == 0 || input.empty()) {
        return;
    }

    std::size_t frameCount = input.size() / channels_;
    if (frameCount == 0) {
        return;
    }
    if (frameCount > capacityFrames_) {
        const auto skippedFrames = frameCount - capacityFrames_;
        input = input.subspan(skippedFrames * channels_);
        frameCount = capacityFrames_;
    } else {
        input = input.first(frameCount * channels_);
    }

    std::size_t consumedFrames = 0;
    while (consumedFrames < frameCount) {
        const auto contiguousFrames = std::min(frameCount - consumedFrames,
                                               capacityFrames_ - writeFrame_);
        std::memcpy(samples_.data() + writeFrame_ * channels_,
                    input.data() + consumedFrames * channels_,
                    contiguousFrames * channels_ * sizeof(float));
        writeFrame_ = (writeFrame_ + contiguousFrames) % capacityFrames_;
        consumedFrames += contiguousFrames;
    }
    storedFrames_ = std::min(capacityFrames_, storedFrames_ + frameCount);
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

void AudioAnalysisBuffer::LatestInto(AudioAnalysisSnapshot& result,
                                     const std::size_t maximumFrames) const {
    std::scoped_lock lock(mutex_);
    result.sampleRate = sampleRate_;
    result.channels = channels_;
    result.generation = generation_.load(std::memory_order_acquire);

    const auto frameCount = std::min(maximumFrames, storedFrames_);
    const auto sampleCount = frameCount * channels_;
    if (result.samples.capacity() < sampleCount) {
        result.samples.reserve(sampleCount);
    }
    result.samples.resize(sampleCount);
    if (frameCount == 0 || channels_ == 0) {
        return;
    }

    const auto oldestFrame = (writeFrame_ + capacityFrames_ - frameCount) % capacityFrames_;
    const auto firstFrames = std::min(frameCount, capacityFrames_ - oldestFrame);
    std::memcpy(result.samples.data(), samples_.data() + oldestFrame * channels_,
                firstFrames * channels_ * sizeof(float));
    if (firstFrames < frameCount) {
        std::memcpy(result.samples.data() + firstFrames * channels_, samples_.data(),
                    (frameCount - firstFrames) * channels_ * sizeof(float));
    }
}

AudioAnalysisSnapshot AudioAnalysisBuffer::Latest(const std::size_t maximumFrames) const {
    AudioAnalysisSnapshot result;
    LatestInto(result, maximumFrames);
    return result;
}

std::uint64_t AudioAnalysisBuffer::Generation() const noexcept {
    return generation_.load(std::memory_order_acquire);
}

} // namespace rivan::audio
