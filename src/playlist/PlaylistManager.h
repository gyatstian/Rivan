// PlaylistManager.h
// Owns the current library catalog and generated or user playlists.
#pragma once

#include "Playlist.h"

#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace rivan::library {
struct LibraryScanResult;
}

namespace rivan::playlist {

class PlaylistManager final {
public:
    void ApplyScan(const library::LibraryScanResult& scan);

    [[nodiscard]] const std::vector<Playlist>& Playlists() const noexcept;
    [[nodiscard]] const library::Track* FindTrack(library::TrackId id) const noexcept;
    [[nodiscard]] const Playlist* FindPlaylist(PlaylistId id) const noexcept;
    // Direct tracks of a playlist (non-recursive for Directory playlists).
    [[nodiscard]] std::vector<library::Track> ResolveTracks(PlaylistId id) const;
    // Every track under a playlist including descendant subfolders, in stable order.
    // Used to seed the playback queue when a folder is chosen.
    [[nodiscard]] std::vector<library::Track> ResolveTracksRecursive(PlaylistId id) const;
    // Immediate child Directory playlists of a folder, sorted like the catalog.
    [[nodiscard]] std::vector<const Playlist*> Children(PlaylistId id) const;
    [[nodiscard]] bool HasChildren(PlaylistId id) const noexcept;
    // Cached recursive track count (All Music / Directory / User). O(1) after rebuild.
    [[nodiscard]] std::size_t TrackCountRecursive(PlaylistId id) const noexcept;

    [[nodiscard]] PlaylistId CreatePlaylist(std::wstring name);
    bool RenamePlaylist(PlaylistId id, std::wstring name);
    bool DeletePlaylist(PlaylistId id);
    // Replaces a renamed file's id/path everywhere it can be persisted before rescan.
    bool ReplaceTrack(library::TrackId oldId, const library::Track& replacement);

    // Adds a full track record to any non-YouTube playlist, remembering imported library
    // entries so they survive rescans. User playlists allow duplicate entries.
    bool AddExternalTrack(PlaylistId playlistId, const library::Track& track);
    // Removes several entries (by position) in one pass; indices need not be sorted.
    bool RemoveTracksAt(PlaylistId playlistId, std::span<const std::size_t> indices);
    // Inserts a copy of the entry at `index` immediately after it.
    bool DuplicateTrackAt(PlaylistId playlistId, std::size_t index);
    // Moves the entries at `indices` so they land contiguously at the drop point.
    bool MoveTracks(PlaylistId playlistId, std::vector<std::size_t> indices,
                    std::size_t destination);
    // Reorders a user playlist among the other user playlists. `beforeId` is the user
    // playlist the moved one should land in front of; 0 means move to the end.
    bool MoveUserPlaylist(PlaylistId id, PlaylistId beforeId);
    // Reorders a Directory folder among its siblings (rows sharing the same parentId).
    // `beforeId` is the sibling folder the moved one should land in front of; 0 means
    // move to the end of the sibling group. Cross-parent moves are rejected.
    bool MoveFolder(PlaylistId id, PlaylistId beforeId);
    // Reapplies a persisted folder order after a rescan. `order` maps a Directory
    // playlist id to its saved rank; folders are reordered within their sibling group by
    // ascending rank. Ids absent from the map (newly scanned folders) sort to the top of
    // their group, keeping alphabetical order among themselves. Call after ReplaceLibrary.
    void ApplyFolderOrder(const std::unordered_map<PlaylistId, std::uint32_t>& order);
    // Directory playlists in current tree order (depth-first), for persisting folder order.
    [[nodiscard]] std::vector<const Playlist*> FolderOrder() const;
    // Reapplies a persisted track order inside one Directory folder after a rescan.
    // `order` maps a TrackId to its saved rank; the folder's trackIds are stable-sorted by
    // ascending rank. Ids absent from the map (newly scanned files) sort to the top,
    // keeping alphabetical order among themselves. Call after ReplaceLibrary.
    void ApplyTrackOrder(PlaylistId folderId,
                         const std::unordered_map<library::TrackId, std::uint32_t>& order);
    // Replaces all user playlists (used when restoring persisted playlists at startup).
    void SetUserPlaylists(std::vector<Playlist> users,
                          std::vector<library::Track> externalTracks);
    // Snapshot of user playlists in tree order, for persistence.
    [[nodiscard]] std::vector<const Playlist*> UserPlaylists() const;

private:
    void ReplaceLibrary(std::vector<library::Track> tracks,
                        std::vector<Playlist> generatedPlaylists);
    [[nodiscard]] Playlist* FindMutableUserPlaylist(PlaylistId id) noexcept;
    // Track reorder is allowed on both User playlists and Directory folders (the latter
    // persists to the folder track-order INI), so it uses a broader lookup than the
    // add/remove/duplicate ops which stay User-only.
    [[nodiscard]] Playlist* FindReorderableTrackList(PlaylistId id) noexcept;
    [[nodiscard]] PlaylistId NextUserId() noexcept;
    void RebuildIndexes();
    [[nodiscard]] std::size_t ComputeTrackCountRecursive(PlaylistId id) const;
    void RememberExternalTrack(const library::Track& track);
    void PruneExternalTracks();

    std::vector<library::Track> tracks_;
    std::vector<Playlist> playlists_;
    // Tracks referenced by user playlists that are not part of the current scan. Keyed by
    // id so FindTrack can fall back here after the scanned catalog misses.
    std::unordered_map<library::TrackId, library::Track> externalTracks_;
    std::unordered_map<library::TrackId, std::size_t> trackIndex_;
    std::unordered_map<PlaylistId, std::size_t> playlistIndex_;
    // Imports attached to generated playlists. Scanner output is replaced on rescan, so
    // these ids are reapplied after every scan.
    std::unordered_map<PlaylistId, std::vector<library::TrackId>> importedTrackIds_;
    std::unordered_map<PlaylistId, std::vector<const Playlist*>> childrenByParent_;
    std::unordered_map<PlaylistId, std::size_t> recursiveTrackCounts_;
    PlaylistId nextUserId_{1};
};

} // namespace rivan::playlist
