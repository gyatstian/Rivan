// LibraryScanner.h
// Recursively discovers supported audio and groups it by folder.
#pragma once

#include "Track.h"
#include "../playlist/Playlist.h"

#include <filesystem>
#include <span>
#include <stop_token>
#include <vector>

namespace rivan::library {

struct LibraryScanResult final {
    std::filesystem::path root;
    std::vector<Track> tracks;
    std::vector<playlist::Playlist> playlists;
};

class LibraryScanner final {
public:
    [[nodiscard]] static bool IsSupported(const std::filesystem::path& path) noexcept;
    [[nodiscard]] LibraryScanResult Scan(const std::filesystem::path& root,
                                         std::stop_token stop = {}) const;
    // Scans several root folders into one catalog. Each existing root becomes a top-level
    // Directory playlist; every subfolder nests beneath it. Empty/duplicate roots are
    // skipped. All Music unions every discovered track across all roots.
    [[nodiscard]] LibraryScanResult Scan(std::span<const std::filesystem::path> roots,
                                         std::stop_token stop = {}) const;
};

} // namespace rivan::library
