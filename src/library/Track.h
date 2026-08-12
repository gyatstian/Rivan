// Track.h
// Value model for an audio file in the local library.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace rivan::library {

using TrackId = std::uint64_t;

// FNV-1a over the case-folded UTF-16 text, mixing each code unit as four bytes.
// Callers apply their own domain prefix and zero/reserved-value remapping.
[[nodiscard]] std::uint64_t FnvHashFoldCase(std::wstring_view text,
                                            std::uint64_t seed) noexcept;

struct Track final {
    TrackId id{};
    std::filesystem::path filePath;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    double durationSeconds{};
    // Average source bitrate. Zero means unavailable (for example missing duration).
    int bitrateKbps{};

    [[nodiscard]] bool IsAvailable() const noexcept;
    [[nodiscard]] static bool IsSupportedFile(const std::filesystem::path& path) noexcept;
    // Audio-only subset of supported media. Video files can play but cannot carry song art.
    [[nodiscard]] static bool IsAudioFile(const std::filesystem::path& path) noexcept;
    [[nodiscard]] static Track FromFile(const std::filesystem::path& path);
};

} // namespace rivan::library
