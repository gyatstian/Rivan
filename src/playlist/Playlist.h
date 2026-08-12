// Playlist.h
// Ordered track collection used by generated and user playlists.
#pragma once

#include "../library/Track.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rivan::playlist {

using PlaylistId = std::uint64_t;
inline constexpr PlaylistId AllMusicPlaylistId = 1;
// Virtual browser playlist (not from disk scan). Gated by AppSettings::youtubeEnabled.
inline constexpr PlaylistId YoutubePlaylistId = 2;

enum class PlaylistKind : std::uint8_t { AllMusic, Directory, User, Youtube };

struct Playlist final {
    PlaylistId id{};
    std::wstring name;
    PlaylistKind kind{PlaylistKind::User};
    std::filesystem::path directory;
    // Direct (non-recursive) tracks living immediately inside `directory`. All Music
    // holds the full recursive union instead.
    std::vector<library::TrackId> trackIds;
    // Folder-hierarchy links for Directory playlists. parentId is 0 for a scan root.
    PlaylistId parentId{};
    // Nesting level: scan roots are 0, their subfolders 1, and so on.
    std::uint32_t depth{};

    [[nodiscard]] bool Contains(library::TrackId trackId) const noexcept {
        return std::find(trackIds.begin(), trackIds.end(), trackId) != trackIds.end();
    }

    bool AddTrack(library::TrackId trackId) {
        if (Contains(trackId)) {
            return false;
        }
        trackIds.push_back(trackId);
        return true;
    }

    // Always appends, even when the id is already present. Enables the same track to
    // appear more than once in a user playlist (the "duplicate entry" feature).
    void AppendTrack(library::TrackId trackId) {
        trackIds.push_back(trackId);
    }

    // Removes the single entry at a position. Position-based so duplicate entries of the
    // same id can be removed independently.
    bool RemoveAt(std::size_t index) noexcept {
        if (index >= trackIds.size()) {
            return false;
        }
        trackIds.erase(trackIds.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

    // Duplicates the entry at a position, inserting the copy directly after it.
    bool DuplicateAt(std::size_t index) {
        if (index >= trackIds.size()) {
            return false;
        }
        trackIds.insert(trackIds.begin() + static_cast<std::ptrdiff_t>(index) + 1,
                        trackIds[index]);
        return true;
    }

    // Moves the entries at `indices` (any order, deduplicated) so they land contiguously
    // at the drop point `destination` (an index into the ORIGINAL list; size() == end).
    // Preserves the relative order of both the moved block and the remaining entries.
    bool MoveRange(std::vector<std::size_t> indices, std::size_t destination) {
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        if (indices.empty() || trackIds.empty() ||
            indices.back() >= trackIds.size() || destination > trackIds.size()) {
            return false;
        }
        std::vector<bool> removed(trackIds.size(), false);
        for (const auto index : indices) removed[index] = true;

        std::vector<library::TrackId> moved;
        moved.reserve(indices.size());
        for (const auto index : indices) moved.push_back(trackIds[index]);

        // Rebuild the remaining entries, remembering where the drop point falls among
        // them. The drop lands before the entry originally at `destination`.
        std::vector<library::TrackId> remaining;
        remaining.reserve(trackIds.size() - moved.size());
        std::size_t insertAt = 0;
        bool insertResolved = false;
        for (std::size_t i = 0; i < trackIds.size(); ++i) {
            if (i == destination) {
                insertAt = remaining.size();
                insertResolved = true;
            }
            if (!removed[i]) remaining.push_back(trackIds[i]);
        }
        if (!insertResolved) insertAt = remaining.size();  // destination == size(): append

        remaining.insert(remaining.begin() + static_cast<std::ptrdiff_t>(insertAt),
                         moved.begin(), moved.end());
        trackIds = std::move(remaining);
        return true;
    }
};

} // namespace rivan::playlist
