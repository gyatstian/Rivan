// PlaylistManager.cpp
// Reconciles rescanned folders while retaining valid user playlist entries.
#include "PlaylistManager.h"

#include "../library/LibraryScanner.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <unordered_set>
#include <utility>

namespace rivan::playlist {
namespace {

constexpr PlaylistId UserIdMask = PlaylistId{1} << 63;

} // namespace

void PlaylistManager::ApplyScan(const library::LibraryScanResult& scan) {
    ReplaceLibrary(scan.tracks, scan.playlists);
}

void PlaylistManager::ReplaceLibrary(std::vector<library::Track> tracks,
                                     std::vector<Playlist> generatedPlaylists) {
    std::vector<Playlist> users;
    for (auto& playlist : playlists_) {
        if (playlist.kind == PlaylistKind::User) {
            users.push_back(std::move(playlist));
        }
    }

    // The UI may still hold pointers into the old catalog while a scan result is
    // being applied.  Rebuild the vector only after the old catalog is no longer
    // observable and keep the index synchronized with the new storage.
    std::unordered_map<library::TrackId, std::size_t> nextTrackIndex;
    nextTrackIndex.reserve(tracks.size());
    for (std::size_t index = 0; index < tracks.size(); ++index) {
        nextTrackIndex[tracks[index].id] = index;
    }
    tracks_ = std::move(tracks);
    trackIndex_ = std::move(nextTrackIndex);
    std::unordered_set<library::TrackId> validTracks;
    validTracks.reserve(tracks_.size());
    for (const auto& track : tracks_) {
        validTracks.insert(track.id);
    }

    for (auto& playlist : users) {
        std::erase_if(playlist.trackIds, [this, &validTracks](library::TrackId id) {
            // Keep entries the scan still knows about, plus external tracks (imports and
            // duplicated copies) that user playlists carry across rescans.
            return !validTracks.contains(id) && !externalTracks_.contains(id);
        });
    }

    playlists_.clear();
    playlists_.reserve(generatedPlaylists.size() + users.size());
    for (auto& playlist : generatedPlaylists) {
        if (playlist.kind != PlaylistKind::User) {
            playlists_.push_back(std::move(playlist));
        }
    }
    playlists_.insert(playlists_.end(),
                      std::make_move_iterator(users.begin()),
                      std::make_move_iterator(users.end()));
    RebuildIndexes();

    std::unordered_set<PlaylistId> importedIds;
    for (auto it = importedTrackIds_.begin(); it != importedTrackIds_.end();) {
        const auto found = playlistIndex_.find(it->first);
        if (found == playlistIndex_.end()) {
            it = importedTrackIds_.erase(it);
            continue;
        }
        auto& playlist = playlists_[found->second];
        std::unordered_set<library::TrackId> seen(playlist.trackIds.begin(),
                                                  playlist.trackIds.end());
        playlist.trackIds.reserve(playlist.trackIds.size() + it->second.size());
        for (const auto id : it->second) {
            if (seen.insert(id).second) playlist.trackIds.push_back(id);
        }
        importedIds.insert(it->second.begin(), it->second.end());
        ++it;
    }
    // Imported folder tracks are part of library-wide playback as well.
    if (const auto allMusic = playlistIndex_.find(AllMusicPlaylistId);
        allMusic != playlistIndex_.end() && !importedIds.empty()) {
        auto& playlist = playlists_[allMusic->second];
        std::unordered_set<library::TrackId> existing(playlist.trackIds.begin(),
                                                      playlist.trackIds.end());
        playlist.trackIds.reserve(playlist.trackIds.size() + importedIds.size());
        for (const auto id : importedIds) {
            if (existing.insert(id).second) playlist.trackIds.push_back(id);
        }
    }
    PruneExternalTracks();
    RebuildIndexes();
}

// External tracks are kept alive while user playlists or generated-playlist imports reference them.
void PlaylistManager::PruneExternalTracks() {
    if (externalTracks_.empty()) return;
    std::unordered_set<library::TrackId> referenced;
    for (const auto& playlist : playlists_) {
        if (playlist.kind != PlaylistKind::User) continue;
        for (const auto id : playlist.trackIds) referenced.insert(id);
    }
    for (const auto& [playlistId, trackIds] : importedTrackIds_) {
        (void)playlistId;
        referenced.insert(trackIds.begin(), trackIds.end());
    }
    std::erase_if(externalTracks_, [&referenced](const auto& entry) {
        return !referenced.contains(entry.first);
    });
}

const std::vector<Playlist>& PlaylistManager::Playlists() const noexcept {
    return playlists_;
}

const std::vector<library::Track>& PlaylistManager::AllTracks() const noexcept {
    return allTracks_;
}

const library::Track* PlaylistManager::FindTrack(library::TrackId id) const noexcept {
    const auto found = trackIndex_.find(id);
    if (found != trackIndex_.end() && found->second < tracks_.size()) {
        return &tracks_[found->second];
    }
    // Fall back to tracks referenced only by user playlists (imports/copies that fall
    // outside the scanned roots).
    const auto external = externalTracks_.find(id);
    return external == externalTracks_.end() ? nullptr : &external->second;
}

const Playlist* PlaylistManager::FindPlaylist(PlaylistId id) const noexcept {
    const auto found = playlistIndex_.find(id);
    if (found == playlistIndex_.end() || found->second >= playlists_.size()) {
        return nullptr;
    }
    return &playlists_[found->second];
}

std::vector<library::Track> PlaylistManager::ResolveTracks(PlaylistId id) const {
    std::vector<library::Track> resolved;
    const auto* playlist = FindPlaylist(id);
    if (playlist == nullptr) {
        return resolved;
    }
    resolved.reserve(playlist->trackIds.size());
    for (const auto trackId : playlist->trackIds) {
        if (const auto* track = FindTrack(trackId); track != nullptr) {
            resolved.push_back(*track);
        }
    }
    return resolved;
}

std::vector<library::Track> PlaylistManager::ResolveTracksRecursive(PlaylistId id) const {
    const auto* playlist = FindPlaylist(id);
    if (playlist == nullptr) return {};
    // AllMusic and User playlists already hold their full track set.
    if (playlist->kind != PlaylistKind::Directory) return ResolveTracks(id);

    auto resolved = ResolveTracks(id);
    for (const auto* child : Children(id)) {
        auto childTracks = ResolveTracksRecursive(child->id);
        resolved.insert(resolved.end(), childTracks.begin(), childTracks.end());
    }
    return resolved;
}

std::vector<const Playlist*> PlaylistManager::Children(PlaylistId id) const {
    const auto found = childrenByParent_.find(id);
    if (found == childrenByParent_.end()) return {};
    return found->second;
}

bool PlaylistManager::HasChildren(PlaylistId id) const noexcept {
    const auto found = childrenByParent_.find(id);
    return found != childrenByParent_.end() && !found->second.empty();
}

std::size_t PlaylistManager::TrackCountRecursive(PlaylistId id) const noexcept {
    const auto found = recursiveTrackCounts_.find(id);
    return found == recursiveTrackCounts_.end() ? 0 : found->second;
}

PlaylistId PlaylistManager::CreatePlaylist(std::wstring name) {
    Playlist playlist;
    playlist.id = NextUserId();
    playlist.name = std::move(name);
    playlist.kind = PlaylistKind::User;
    playlists_.push_back(std::move(playlist));
    RebuildIndexes();
    return playlists_.back().id;
}

bool PlaylistManager::RenamePlaylist(PlaylistId id, std::wstring name) {
    auto* playlist = FindMutableUserPlaylist(id);
    if (playlist == nullptr) {
        return false;
    }
    playlist->name = std::move(name);
    return true;
}

bool PlaylistManager::DeletePlaylist(PlaylistId id) {
    const auto position = std::find_if(playlists_.begin(), playlists_.end(), [id](const auto& playlist) {
        return playlist.id == id && playlist.kind == PlaylistKind::User;
    });
    if (position == playlists_.end()) {
        return false;
    }
    playlists_.erase(position);
    RebuildIndexes();
    return true;
}

bool PlaylistManager::ReplaceTrack(library::TrackId oldId, const library::Track& replacement) {
    if (oldId == 0 || replacement.id == 0) return false;
    bool found = false;
    for (auto& track : tracks_) {
        if (track.id == oldId) {
            track = replacement;
            found = true;
        }
    }
    if (const auto external = externalTracks_.find(oldId); external != externalTracks_.end()) {
        externalTracks_.erase(external);
        externalTracks_[replacement.id] = replacement;
        found = true;
    }
    if (!found) return false;

    for (auto& playlist : playlists_) {
        for (auto& id : playlist.trackIds) {
            if (id == oldId) id = replacement.id;
        }
    }
    for (auto& [playlistId, ids] : importedTrackIds_) {
        (void)playlistId;
        for (auto& id : ids) {
            if (id == oldId) id = replacement.id;
        }
    }
    RebuildIndexes();
    return true;
}

void PlaylistManager::RememberExternalTrack(const library::Track& track) {
    // Only remember tracks the scan does not already own; scanned records win in FindTrack.
    if (trackIndex_.find(track.id) != trackIndex_.end()) return;
    externalTracks_[track.id] = track;
}

bool PlaylistManager::AddExternalTrack(PlaylistId playlistId, const library::Track& track) {
    const auto found = playlistIndex_.find(playlistId);
    if (found == playlistIndex_.end() || found->second >= playlists_.size()) {
        return false;
    }
    auto& playlist = playlists_[found->second];
    if (playlist.kind == PlaylistKind::Youtube) return false;
    if (playlist.kind == PlaylistKind::User) {
        playlist.AppendTrack(track.id);
    } else {
        if (!playlist.AddTrack(track.id)) return false;
        importedTrackIds_[playlistId].push_back(track.id);
        if (playlistId != AllMusicPlaylistId) {
            const auto allMusic = playlistIndex_.find(AllMusicPlaylistId);
            if (allMusic != playlistIndex_.end()) {
                playlists_[allMusic->second].AddTrack(track.id);
            }
        }
    }
    // Remember only after the target accepted the id; a rejected duplicate Directory
    // import must not leave a stale external-track record behind.
    RememberExternalTrack(track);
    RebuildIndexes();
    return true;
}

bool PlaylistManager::RemoveTracksAt(PlaylistId playlistId,
                                     std::span<const std::size_t> indices) {
    auto* playlist = FindMutableUserPlaylist(playlistId);
    if (playlist == nullptr || indices.empty()) {
        return false;
    }
    // Erase from the highest index down so earlier positions stay valid.
    std::vector<std::size_t> sorted(indices.begin(), indices.end());
    std::sort(sorted.begin(), sorted.end(), std::greater<>{});
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    bool changed = false;
    for (const auto index : sorted) {
        changed = playlist->RemoveAt(index) || changed;
    }
    if (changed) RebuildIndexes();
    return changed;
}

bool PlaylistManager::DuplicateTrackAt(PlaylistId playlistId, std::size_t index) {
    auto* playlist = FindMutableUserPlaylist(playlistId);
    if (playlist == nullptr || !playlist->DuplicateAt(index)) {
        return false;
    }
    RebuildIndexes();
    return true;
}

bool PlaylistManager::MoveTracks(PlaylistId playlistId, std::vector<std::size_t> indices,
                                 std::size_t destination) {
    auto* playlist = FindReorderableTrackList(playlistId);
    if (playlist == nullptr || !playlist->MoveRange(std::move(indices), destination)) {
        return false;
    }
    RebuildIndexes();
    return true;
}

bool PlaylistManager::MoveUserPlaylist(PlaylistId id, PlaylistId beforeId) {
    if (id == beforeId) return false;
    const auto source = std::find_if(playlists_.begin(), playlists_.end(), [id](const auto& p) {
        return p.id == id && p.kind == PlaylistKind::User;
    });
    if (source == playlists_.end()) return false;
    if (beforeId != 0) {
        const auto* before = FindPlaylist(beforeId);
        if (before == nullptr || before->kind != PlaylistKind::User) return false;
    }
    Playlist moved = std::move(*source);
    playlists_.erase(source);
    auto target = beforeId == 0
        ? playlists_.end()
        : std::find_if(playlists_.begin(), playlists_.end(), [beforeId](const auto& p) {
              return p.id == beforeId && p.kind == PlaylistKind::User;
          });
    playlists_.insert(target, std::move(moved));
    RebuildIndexes();
    return true;
}

bool PlaylistManager::MoveFolder(PlaylistId id, PlaylistId beforeId) {
    if (id == beforeId) return false;
    const auto source = std::find_if(playlists_.begin(), playlists_.end(), [id](const auto& p) {
        return p.id == id && p.kind == PlaylistKind::Directory;
    });
    if (source == playlists_.end()) return false;
    const PlaylistId parentId = source->parentId;
    // beforeId must be a sibling (same parent); otherwise the drop is out of the group
    // and we ignore it. beforeId 0 means append to the end of this sibling group.
    if (beforeId != 0) {
        const auto* before = FindPlaylist(beforeId);
        if (before == nullptr || before->kind != PlaylistKind::Directory ||
            before->parentId != parentId) {
            return false;
        }
    }
    Playlist moved = std::move(*source);
    playlists_.erase(source);
    std::vector<Playlist>::iterator target;
    if (beforeId != 0) {
        target = std::find_if(playlists_.begin(), playlists_.end(), [beforeId](const auto& p) {
            return p.id == beforeId && p.kind == PlaylistKind::Directory;
        });
    } else {
        // Append after the last sibling sharing this parent so the tree keeps grouping
        // children contiguously under their parent.
        target = playlists_.end();
        for (auto it = playlists_.begin(); it != playlists_.end(); ++it) {
            if (it->kind == PlaylistKind::Directory && it->parentId == parentId) {
                target = std::next(it);
            }
        }
    }
    playlists_.insert(target, std::move(moved));
    RebuildIndexes();
    return true;
}

void PlaylistManager::ApplyFolderOrder(
    const std::unordered_map<PlaylistId, std::uint32_t>& order) {
    if (order.empty()) return;
    // Group each folder's direct-child Directory playlists by parent, preserving the
    // current (scanner-alphabetical) order and remembering the vector slots they occupy.
    // Slots for a group are not contiguous (the scan interleaves descendants), so we
    // reorder the folders and scatter them back into the same slots.
    std::unordered_map<PlaylistId, std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < playlists_.size(); ++i) {
        if (playlists_[i].kind == PlaylistKind::Directory) {
            groups[playlists_[i].parentId].push_back(i);
        }
    }
    for (auto& [parent, indices] : groups) {
        (void)parent;
        if (indices.size() < 2) continue;
        std::vector<std::size_t> sorted = indices;  // ascending, current order
        std::stable_sort(sorted.begin(), sorted.end(), [&](std::size_t a, std::size_t b) {
            const auto ra = order.find(playlists_[a].id);
            const auto rb = order.find(playlists_[b].id);
            const bool ka = ra != order.end();
            const bool kb = rb != order.end();
            // Unknown (newly scanned) folders sort to the top of their sibling group,
            // keeping their current alphabetical order among themselves.
            if (ka != kb) return !ka;
            if (!ka) return false;
            return ra->second < rb->second;
        });
        std::vector<Playlist> reordered;
        reordered.reserve(sorted.size());
        for (const auto idx : sorted) reordered.push_back(std::move(playlists_[idx]));
        for (std::size_t k = 0; k < indices.size(); ++k) {
            playlists_[indices[k]] = std::move(reordered[k]);
        }
    }
    RebuildIndexes();
}

std::vector<const Playlist*> PlaylistManager::FolderOrder() const {
    // Depth-first walk mirroring the visible tree so saved ranks match what the user sees.
    std::vector<const Playlist*> result;
    std::function<void(PlaylistId)> visit = [&](PlaylistId parent) {
        for (const auto* child : Children(parent)) {
            result.push_back(child);
            visit(child->id);
        }
    };
    visit(0);
    return result;
}

void PlaylistManager::ApplyTrackOrder(
    PlaylistId folderId, const std::unordered_map<library::TrackId, std::uint32_t>& order) {
    if (order.empty()) return;
    const auto found = playlistIndex_.find(folderId);
    if (found == playlistIndex_.end() || found->second >= playlists_.size()) return;
    auto& playlist = playlists_[found->second];
    if (playlist.kind != PlaylistKind::Directory) return;
    if (playlist.trackIds.size() < 2) return;
    std::stable_sort(playlist.trackIds.begin(), playlist.trackIds.end(),
                     [&](library::TrackId a, library::TrackId b) {
                         const auto ra = order.find(a);
                         const auto rb = order.find(b);
                         const bool ka = ra != order.end();
                         const bool kb = rb != order.end();
                         // Unknown (newly scanned) tracks sort to the top, keeping their
                         // current alphabetical order among themselves.
                         if (ka != kb) return !ka;
                         if (!ka) return false;
                         return ra->second < rb->second;
                     });
    RebuildIndexes();
}

void PlaylistManager::SetUserPlaylists(std::vector<Playlist> users,
                                       std::vector<library::Track> externalTracks) {
    std::erase_if(playlists_, [](const auto& p) { return p.kind == PlaylistKind::User; });
    for (auto& track : externalTracks) RememberExternalTrack(track);
    for (auto& playlist : users) {
        playlist.kind = PlaylistKind::User;
        // Persisted playlists carry no id (the INI stores order + tracks only), so assign
        // a fresh user id here. Without this every loaded playlist keeps id 0, which
        // collides in the index and blocks drag-reorder (FinishPlaylistDrag ignores id 0).
        if (playlist.id == 0) playlist.id = NextUserId();
        playlists_.push_back(std::move(playlist));
    }
    RebuildIndexes();
}

std::vector<const Playlist*> PlaylistManager::UserPlaylists() const {
    std::vector<const Playlist*> result;
    for (const auto& playlist : playlists_) {
        if (playlist.kind == PlaylistKind::User) result.push_back(&playlist);
    }
    return result;
}

Playlist* PlaylistManager::FindMutableUserPlaylist(PlaylistId id) noexcept {
    const auto found = playlistIndex_.find(id);
    if (found == playlistIndex_.end() || found->second >= playlists_.size()) {
        return nullptr;
    }
    auto& playlist = playlists_[found->second];
    return playlist.kind == PlaylistKind::User ? &playlist : nullptr;
}

Playlist* PlaylistManager::FindReorderableTrackList(PlaylistId id) noexcept {
    const auto found = playlistIndex_.find(id);
    if (found == playlistIndex_.end() || found->second >= playlists_.size()) {
        return nullptr;
    }
    auto& playlist = playlists_[found->second];
    return (playlist.kind == PlaylistKind::User || playlist.kind == PlaylistKind::Directory)
               ? &playlist
               : nullptr;
}

PlaylistId PlaylistManager::NextUserId() noexcept {
    for (;;) {
        const auto candidate = UserIdMask | nextUserId_++;
        if (nextUserId_ == 0 || nextUserId_ >= UserIdMask) {
            nextUserId_ = 1;
        }
        if (FindPlaylist(candidate) == nullptr) {
            return candidate;
        }
    }
}

void PlaylistManager::RebuildIndexes() {
    trackIndex_.clear();
    playlistIndex_.clear();
    childrenByParent_.clear();
    recursiveTrackCounts_.clear();

    trackIndex_.reserve(tracks_.size());
    for (std::size_t index = 0; index < tracks_.size(); ++index) {
        trackIndex_[tracks_[index].id] = index;
    }

    // Keep one canonical metadata catalog for consumers that need every known track,
    // including imported tracks outside current scan roots. Scanned records win.
    allTracks_ = tracks_;
    std::unordered_set<library::TrackId> catalogIds;
    catalogIds.reserve(allTracks_.size() + externalTracks_.size());
    for (const auto& track : allTracks_) catalogIds.insert(track.id);
    for (const auto& [id, track] : externalTracks_) {
        if (catalogIds.insert(id).second) allTracks_.push_back(track);
    }

    playlistIndex_.reserve(playlists_.size());
    for (std::size_t index = 0; index < playlists_.size(); ++index) {
        playlistIndex_[playlists_[index].id] = index;
        if (playlists_[index].kind == PlaylistKind::Directory) {
            childrenByParent_[playlists_[index].parentId].push_back(&playlists_[index]);
        }
    }

    for (const auto& playlist : playlists_) {
        if (playlist.kind == PlaylistKind::Directory) continue;
        recursiveTrackCounts_[playlist.id] = playlist.trackIds.size();
    }
    for (const auto& playlist : playlists_) {
        if (playlist.kind == PlaylistKind::Directory) {
            recursiveTrackCounts_[playlist.id] = ComputeTrackCountRecursive(playlist.id);
        }
    }
}

std::size_t PlaylistManager::ComputeTrackCountRecursive(PlaylistId id) const {
    if (const auto cached = recursiveTrackCounts_.find(id); cached != recursiveTrackCounts_.end()) {
        return cached->second;
    }
    const auto* playlist = FindPlaylist(id);
    if (playlist == nullptr) return 0;
    if (playlist->kind != PlaylistKind::Directory) return playlist->trackIds.size();

    std::size_t total = playlist->trackIds.size();
    const auto children = childrenByParent_.find(id);
    if (children != childrenByParent_.end()) {
        for (const auto* child : children->second) {
            total += ComputeTrackCountRecursive(child->id);
        }
    }
    return total;
}

} // namespace rivan::playlist
