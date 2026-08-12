// TrackCoverCache.h
// Stable cache-size tiers for device-dependent library cover bitmaps.
#pragma once

namespace rivan::ui {

inline constexpr unsigned int kMaximumTrackCoverDimension = 256u;

[[nodiscard]] constexpr unsigned int BucketTrackCoverDimension(
    const unsigned int requestedDimension) noexcept {
    if (requestedDimension <= 16u) return 16u;
    if (requestedDimension <= 32u) return 32u;
    if (requestedDimension <= 64u) return 64u;
    if (requestedDimension <= 128u) return 128u;
    return kMaximumTrackCoverDimension;
}

} // namespace rivan::ui
