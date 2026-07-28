// Rivan internal fixed-capacity byte ring used to bound decoded PCM memory.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace rivan::audio::detail {

class PcmRingBuffer final {
public:
    explicit PcmRingBuffer(const std::size_t capacity = 0,
                           const std::size_t alignment = 1) {
        ResetCapacity(capacity, alignment);
    }

    void ResetCapacity(const std::size_t capacity, const std::size_t alignment = 1) {
        alignment_ = alignment == 0 ? 1 : alignment;
        // Prefer power-of-two capacity for mask indexing; keep alignment so PCM frames stay whole.
        auto rounded = RoundUpPowerOfTwo(capacity);
        if (rounded != 0) {
            // Grow until capacity is a multiple of alignment (always true when alignment is pot).
            while (rounded % alignment_ != 0) {
                if (rounded > (std::size_t{1} << (sizeof(std::size_t) * 8 - 2))) {
                    rounded -= rounded % alignment_;
                    break;
                }
                rounded <<= 1U;
            }
        }
        data_.assign(rounded, std::byte{});
        mask_ = IsPowerOfTwo(rounded) && rounded != 0 ? rounded - 1 : 0;
        useMask_ = mask_ != 0;
        Clear();
    }

    void Clear() noexcept { read_ = write_ = size_ = 0; }
    [[nodiscard]] std::size_t Size() const noexcept { return size_; }
    [[nodiscard]] std::size_t Free() const noexcept { return data_.size() - size_; }
    [[nodiscard]] std::size_t Capacity() const noexcept { return data_.size(); }

    [[nodiscard]] std::size_t Write(std::span<const std::byte> source) noexcept {
        auto count = std::min(source.size(), Free());
        count -= count % alignment_;
        if (count == 0 || data_.empty()) {
            return 0;
        }
        const auto first = std::min(count, data_.size() - write_);
        std::memcpy(data_.data() + write_, source.data(), first);
        if (count > first) {
            std::memcpy(data_.data(), source.data() + first, count - first);
        }
        Advance(write_, count);
        size_ += count;
        return count;
    }

    [[nodiscard]] std::size_t Read(std::span<std::byte> destination) noexcept {
        auto count = std::min(destination.size(), size_);
        count -= count % alignment_;
        if (count == 0 || data_.empty()) {
            return 0;
        }
        const auto first = std::min(count, data_.size() - read_);
        std::memcpy(destination.data(), data_.data() + read_, first);
        if (count > first) {
            std::memcpy(destination.data() + first, data_.data(), count - first);
        }
        Advance(read_, count);
        size_ -= count;
        return count;
    }

private:
    [[nodiscard]] static bool IsPowerOfTwo(const std::size_t value) noexcept {
        return value != 0 && (value & (value - 1)) == 0;
    }

    [[nodiscard]] static std::size_t RoundUpPowerOfTwo(std::size_t value) noexcept {
        if (value == 0) {
            return 0;
        }
        --value;
        value |= value >> 1U;
        value |= value >> 2U;
        value |= value >> 4U;
        value |= value >> 8U;
        value |= value >> 16U;
#if defined(_WIN64) || defined(__x86_64__) || defined(__aarch64__)
        value |= value >> 32U;
#endif
        return value + 1;
    }

    void Advance(std::size_t& index, const std::size_t count) noexcept {
        if (useMask_) {
            index = (index + count) & mask_;
        } else if (!data_.empty()) {
            index = (index + count) % data_.size();
        }
    }

    std::vector<std::byte> data_;
    std::size_t mask_{};
    std::size_t alignment_{1};
    bool useMask_{};
    std::size_t read_{};
    std::size_t write_{};
    std::size_t size_{};
};

} // namespace rivan::audio::detail
