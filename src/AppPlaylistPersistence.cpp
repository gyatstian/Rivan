#include "App.h"

#include "core/AppPaths.h"
#include "core/IniDocument.h"
#include "core/IniValueCodec.h"
#include "core/Text.h"

#include <charconv>
#include <filesystem>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace rivan {
namespace {

std::filesystem::path UserPlaylistsFile() {
    return core::AppPaths::LocalDataRoot() / L"playlists.ini";
}

} // namespace

void App::LoadUserPlaylists() {
    const auto file = UserPlaylistsFile();
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) return;
    auto document = core::IniDocument::Load(file, nullptr);
    if (!document) return;
    if (!document->HasMetaFormat("1")) return;  // unsupported or missing format

    std::size_t count = 0;
    if (const auto stored = document->Get("meta", "count")) {
        const auto text = std::string(*stored);
        std::size_t value = 0;
        const auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (err == std::errc{} && end == text.data() + text.size()) count = value;
    }
    if (count > 4096) count = 4096;

    std::vector<playlist::Playlist> users;
    std::vector<library::Track> externalTracks;
    for (std::size_t p = 0; p < count; ++p) {
        const std::string section = "pl" + std::to_string(p);
        const auto nameEncoded = document->Get(section, "name");
        if (!nameEncoded) continue;
        const auto nameUtf8 = core::DecodeIniValue(*nameEncoded, false);
        if (!nameUtf8) continue;
        playlist::Playlist list;
        list.kind = playlist::PlaylistKind::User;
        list.name = core::Utf8ToWide(*nameUtf8);
        std::size_t trackCount = 0;
        if (const auto stored = document->Get(section, "track_count")) {
            const auto text = std::string(*stored);
            std::size_t value = 0;
            const auto [end, err] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (err == std::errc{} && end == text.data() + text.size()) trackCount = value;
        }
        if (trackCount > 100000) trackCount = 100000;
        for (std::size_t t = 0; t < trackCount; ++t) {
            const auto encoded = document->Get(section, "track" + std::to_string(t));
            if (!encoded) continue;
            const auto pathUtf8 = core::DecodeIniValue(*encoded, false);
            if (!pathUtf8) continue;
            const std::filesystem::path path(core::Utf8ToWide(*pathUtf8));
            if (path.empty()) continue;
            // Restored user playlists only need a stable id and path; metadata is
            // re-derived by the library scan.
            auto track = library::Track::FromPathOnly(path);
            list.AppendTrack(track.id);
            externalTracks.push_back(std::move(track));
        }
        users.push_back(std::move(list));
    }
    playlists_.SetUserPlaylists(std::move(users), std::move(externalTracks));
}

void App::SaveUserPlaylists() const {
    core::IniDocument document;
    document.Set("meta", "format", "1");
    const auto users = playlists_.UserPlaylists();
    document.Set("meta", "count", std::to_string(users.size()));
    for (std::size_t p = 0; p < users.size(); ++p) {
        const auto* list = users[p];
        const std::string section = "pl" + std::to_string(p);
        document.Set(section, "name", core::EncodeIniValue(core::WideToUtf8(list->name)));
        document.Set(section, "track_count", std::to_string(list->trackIds.size()));
        for (std::size_t t = 0; t < list->trackIds.size(); ++t) {
            const auto* track = playlists_.FindTrack(list->trackIds[t]);
            const std::wstring path = track ? track->filePath.wstring() : std::wstring{};
            document.Set(section, "track" + std::to_string(t),
                          core::EncodeIniValue(core::WideToUtf8(path)));
        }
    }
    (void)document.SaveAtomic(UserPlaylistsFile(), nullptr);
}

void App::SaveFolderOrder() const {
    // Dedicated INI in the music root, keyed by folder path so the order maps back onto
    // the same directories after a rescan (ids are recomputed but paths are stable).
    const auto root = EffectiveMusicRoot();
    if (root.empty()) return;
    core::IniDocument document;
    document.Set("meta", "format", "1");
    const auto folders = playlists_.FolderOrder();
    document.Set("meta", "count", std::to_string(folders.size()));
    for (std::size_t i = 0; i < folders.size(); ++i) {
        // The depth-first index is the rank; ApplyFolderOrder only compares ranks within a
        // sibling group, so a single monotonic sequence across the whole tree is enough.
        document.Set("fo" + std::to_string(i), "path",
                      core::EncodeIniValue(core::WideToUtf8(folders[i]->directory.wstring())));
    }
    (void)document.SaveAtomic(root / L"rivan-folder-order.ini", nullptr);
}

void App::ApplyFolderOrderAfterScan() {
    const auto root = EffectiveMusicRoot();
    if (root.empty()) return;
    const auto file = root / L"rivan-folder-order.ini";
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) return;
    auto document = core::IniDocument::Load(file, nullptr);
    if (!document) return;
    if (!document->HasMetaFormat("1")) return;  // unsupported or missing format

    std::size_t count = 0;
    if (const auto stored = document->Get("meta", "count")) {
        const auto text = std::string(*stored);
        std::size_t value = 0;
        const auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (err == std::errc{} && end == text.data() + text.size()) count = value;
    }
    if (count > 100000) count = 100000;

    // Map each saved folder path to the Directory playlist id the current scan produced,
    // so ranks apply even though ids are recomputed every scan.
    std::unordered_map<std::wstring, playlist::PlaylistId> idByPath;
    for (const auto& playlist : playlists_.Playlists()) {
        if (playlist.kind == playlist::PlaylistKind::Directory) {
            idByPath.emplace(playlist.directory.wstring(), playlist.id);
        }
    }

    std::unordered_map<playlist::PlaylistId, std::uint32_t> order;
    order.reserve(count);
    std::uint32_t rank = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const auto encoded = document->Get("fo" + std::to_string(i), "path");
        if (!encoded) continue;
        const auto pathUtf8 = core::DecodeIniValue(*encoded, false);
        if (!pathUtf8) continue;
        const auto found = idByPath.find(core::Utf8ToWide(*pathUtf8));
        if (found == idByPath.end()) continue;  // folder no longer exists
        order.emplace(found->second, rank++);
    }
    playlists_.ApplyFolderOrder(order);
}

void App::SaveTrackOrder(playlist::PlaylistId folderId) const {
    // Track order within Directory folders persists to a dedicated INI keyed by folder
    // path then file path. Only the folder that was just reordered changes, so merge with
    // the existing file to keep other folders' custom orders intact.
    const auto root = EffectiveMusicRoot();
    if (root.empty()) return;
    const auto* selected = playlists_.FindPlaylist(folderId);
    if (selected == nullptr || selected->kind != playlist::PlaylistKind::Directory) return;
    const auto file = root / L"rivan-track-order.ini";

    struct Entry {
        std::wstring path;
        std::vector<std::wstring> tracks;
    };
    std::vector<Entry> entries;

    // Carry forward every other folder's saved order.
    std::error_code ec;
    if (std::filesystem::exists(file, ec) && !ec) {
        if (auto document = core::IniDocument::Load(file, nullptr)) {
            // A missing or unsupported meta.format means the file cannot be parsed
            // reliably; treat it as empty carry-forward so a mismatched future file
            // is replaced by format=1 content below instead of being misread.
            if (document->HasMetaFormat("1")) {
                std::size_t count = 0;
                if (const auto stored = document->Get("meta", "count")) {
                    const auto text = std::string(*stored);
                    std::size_t value = 0;
                    const auto [end, err] =
                        std::from_chars(text.data(), text.data() + text.size(), value);
                    if (err == std::errc{} && end == text.data() + text.size()) count = value;
                }
                if (count > 100000) count = 100000;
                const auto selectedPath = selected->directory.wstring();
                for (std::size_t i = 0; i < count; ++i) {
                    const std::string section = "f" + std::to_string(i);
                    const auto encPath = document->Get(section, "path");
                    if (!encPath) continue;
                    const auto pathUtf8 = core::DecodeIniValue(*encPath, false);
                    if (!pathUtf8) continue;
                    std::wstring folderPath = core::Utf8ToWide(*pathUtf8);
                    if (folderPath == selectedPath) continue;  // rewritten below
                    std::size_t trackCount = 0;
                    if (const auto storedTracks = document->Get(section, "track_count")) {
                        const auto text = std::string(*storedTracks);
                        std::size_t value = 0;
                        const auto [end, err] =
                            std::from_chars(text.data(), text.data() + text.size(), value);
                        if (err == std::errc{} && end == text.data() + text.size()) trackCount = value;
                    }
                    if (trackCount > 1000000) trackCount = 1000000;
                    Entry entry;
                    entry.path = std::move(folderPath);
                    for (std::size_t j = 0; j < trackCount; ++j) {
                        const auto encTrack = document->Get(section, "track" + std::to_string(j));
                        if (!encTrack) continue;
                        const auto trackUtf8 = core::DecodeIniValue(*encTrack, false);
                        if (!trackUtf8) continue;
                        entry.tracks.push_back(core::Utf8ToWide(*trackUtf8));
                    }
                    entries.push_back(std::move(entry));
                }
            }
        }
    }

    // Append the selected folder's current order (resolved to stable file paths).
    Entry current;
    current.path = selected->directory.wstring();
    for (const auto id : selected->trackIds) {
        const auto* track = playlists_.FindTrack(id);
        if (track != nullptr) current.tracks.push_back(track->filePath.wstring());
    }
    entries.push_back(std::move(current));

    core::IniDocument document;
    document.Set("meta", "format", "1");
    document.Set("meta", "count", std::to_string(entries.size()));
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string section = "f" + std::to_string(i);
        document.Set(section, "path", core::EncodeIniValue(core::WideToUtf8(entries[i].path)));
        document.Set(section, "track_count", std::to_string(entries[i].tracks.size()));
        for (std::size_t j = 0; j < entries[i].tracks.size(); ++j) {
            document.Set(section, "track" + std::to_string(j),
                          core::EncodeIniValue(core::WideToUtf8(entries[i].tracks[j])));
        }
    }
    (void)document.SaveAtomic(file, nullptr);
}

void App::ApplyTrackOrderAfterScan() {
    const auto root = EffectiveMusicRoot();
    if (root.empty()) return;
    const auto file = root / L"rivan-track-order.ini";
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) return;
    auto document = core::IniDocument::Load(file, nullptr);
    if (!document) return;
    if (!document->HasMetaFormat("1")) return;  // unsupported or missing format

    std::size_t count = 0;
    if (const auto stored = document->Get("meta", "count")) {
        const auto text = std::string(*stored);
        std::size_t value = 0;
        const auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (err == std::errc{} && end == text.data() + text.size()) count = value;
    }
    if (count > 100000) count = 100000;

    // Folder path -> the Directory playlist this scan produced (ids recomputed, paths stable).
    std::unordered_map<std::wstring, const playlist::Playlist*> folderByPath;
    for (const auto& playlist : playlists_.Playlists()) {
        if (playlist.kind == playlist::PlaylistKind::Directory) {
            folderByPath.emplace(playlist.directory.wstring(), &playlist);
        }
    }

    for (std::size_t i = 0; i < count; ++i) {
        const std::string section = "f" + std::to_string(i);
        const auto encPath = document->Get(section, "path");
        if (!encPath) continue;
        const auto pathUtf8 = core::DecodeIniValue(*encPath, false);
        if (!pathUtf8) continue;
        const auto found = folderByPath.find(core::Utf8ToWide(*pathUtf8));
        if (found == folderByPath.end()) continue;  // folder no longer exists
        const auto* folder = found->second;

        // Resolve saved file paths against this folder's current tracks. Matching by path
        // avoids recomputing track-id hashes here; ids are stable but private to the scanner.
        std::unordered_map<std::wstring, library::TrackId> idByFile;
        for (const auto id : folder->trackIds) {
            const auto* track = playlists_.FindTrack(id);
            if (track != nullptr) idByFile.emplace(track->filePath.wstring(), id);
        }

        std::size_t trackCount = 0;
        if (const auto storedTracks = document->Get(section, "track_count")) {
            const auto text = std::string(*storedTracks);
            std::size_t value = 0;
            const auto [end, err] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (err == std::errc{} && end == text.data() + text.size()) trackCount = value;
        }
        if (trackCount > 1000000) trackCount = 1000000;

        std::unordered_map<library::TrackId, std::uint32_t> order;
        std::uint32_t rank = 0;
        for (std::size_t j = 0; j < trackCount; ++j) {
            const auto encTrack = document->Get(section, "track" + std::to_string(j));
            if (!encTrack) continue;
            const auto trackUtf8 = core::DecodeIniValue(*encTrack, false);
            if (!trackUtf8) continue;
            const auto it = idByFile.find(core::Utf8ToWide(*trackUtf8));
            if (it == idByFile.end()) continue;  // track no longer in folder
            order.emplace(it->second, rank++);
        }
        playlists_.ApplyTrackOrder(folder->id, order);
    }
}

} // namespace rivan
