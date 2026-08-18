// AppPlaylists.cpp
// Playlist and track editing, filesystem operations, and file-selection dialogs.
#include "App.h"
#include "ui/Win32Ui.MetadataEditor.h"

#include "core/FileSystemUtil.h"

#include <shobjidl.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <Windows.h>

#pragma comment(lib, "shlwapi.lib")

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace rivan {
namespace {

struct AudioReleaseGuard {
    bool released{};
    std::filesystem::path filePath;
    std::chrono::nanoseconds position{};
    bool wasPlaying{};
};

HRESULT WriteCoverArt(const std::filesystem::path& track, const std::filesystem::path& image) {
    // Only try IPropertyStore for formats Windows handles natively.
    if (ui::HandledByWindowsPropertyStore(track.wstring())) {
        Microsoft::WRL::ComPtr<IPropertyStore> properties;
        HRESULT result = SHGetPropertyStoreFromParsingName(
            track.c_str(), nullptr, GPS_READWRITE, IID_PPV_ARGS(properties.GetAddressOf()));
        if (SUCCEEDED(result)) {
            Microsoft::WRL::ComPtr<IStream> stream;
            result = SHCreateStreamOnFileEx(image.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE,
                                            FILE_ATTRIBUTE_NORMAL, FALSE, nullptr,
                                            stream.GetAddressOf());
            if (SUCCEEDED(result)) {
                PROPVARIANT value{};
                value.vt = VT_UNKNOWN;
                value.punkVal = stream.Get();
                value.punkVal->AddRef();
                result = properties->SetValue(PKEY_ThumbnailStream, value);
                if (SUCCEEDED(result)) result = properties->Commit();
                PropVariantClear(&value);
                if (SUCCEEDED(result)) return S_OK;
            }
        }
    }

    // IPropertyStore unavailable or failed – try ffmpeg fallback.
    return ui::WriteCoverArtFfmpeg(track.wstring(), image.wstring()) ? S_OK : E_FAIL;
}

} // namespace

bool App::WaitForAudioRelease() const {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = audio_.Status();
        if (!status.hasMedia && status.state == audio::PlaybackState::Error) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

void App::ImportDroppedFiles(std::span<const std::wstring> paths) {
    std::vector<std::filesystem::path> importedPaths;
    importedPaths.reserve(paths.size());
    for (const auto& path : paths) importedPaths.emplace_back(path);
    const bool wasEmpty = queue_.Empty();
    const auto count = queue_.AppendDroppedFiles(importedPaths);
    if (wasEmpty && count != 0) PlayNavigation(queue_.Start());
    ++revision_;
}

std::optional<library::TrackId> App::TrackIdAtIndex(std::size_t index) const {
    const auto& tracks = queue_.Tracks();
    if (index >= tracks.size()) return std::nullopt;
    return tracks[index].id;
}

void App::CreateUserPlaylist(std::wstring name) {
    if (name.empty()) return;
    const std::filesystem::path folderName(name);
    // A playlist is a direct child folder of configured library root, never virtual state.
    // A user playlist must land in the root the session is actually using.
    if (!core::IsValidFileName(name)) {
        if (window_ && window_->WindowHandle()) {
            MessageBoxW(window_->WindowHandle(), L"Playlist name is not a valid folder name.",
                        L"Rivan", MB_OK | MB_ICONWARNING);
        }
        return;
    }

    const auto folder = EffectiveMusicRoot() / folderName;
    std::error_code ec;
    if (!std::filesystem::create_directory(folder, ec)) {
        const wchar_t* message = ec ? L"Unable to create playlist folder in Music root."
                                    : L"A folder with this playlist name already exists.";
        if (window_ && window_->WindowHandle()) {
            MessageBoxW(window_->WindowHandle(), message, L"Rivan", MB_OK | MB_ICONWARNING);
        }
        return;
    }
    selectedPlaylist_ = playlist::AllMusicPlaylistId;
    selectedTrack_ = 0;
    RefreshLibrary();
}

void App::RenameUserPlaylist(std::uint64_t id, std::wstring name) {
    const auto* playlist = playlists_.FindPlaylist(id);
    if (playlist == nullptr || playlist->kind != playlist::PlaylistKind::Directory ||
        !core::IsValidFileName(name)) {
        return;
    }
    std::error_code ec;
    std::filesystem::rename(playlist->directory, playlist->directory.parent_path() / name, ec);
    if (ec) return;
    selectedPlaylist_ = playlist::AllMusicPlaylistId;
    selectedTrack_ = 0;
    RefreshLibrary();
}

void App::DeleteUserPlaylists(std::span<const std::uint64_t> ids) {
    bool changed = false;
    bool selectionRemoved = false;
    for (const auto id : ids) {
        const auto* playlist = playlists_.FindPlaylist(id);
        if (playlist == nullptr || playlist->kind != playlist::PlaylistKind::Directory) continue;
        std::error_code ec;
        const auto removed = std::filesystem::remove_all(playlist->directory, ec);
        if (!ec && removed != 0) {
            if (id == selectedPlaylist_) selectionRemoved = true;
            changed = true;
        }
    }
    if (!changed) return;
    if (selectionRemoved) {
        selectedPlaylist_ = playlist::AllMusicPlaylistId;
        selectedTrack_ = 0;
    }
    RefreshLibrary();
}

void App::ReseedSelectedUserQueue() {
    if (playlists_.FindPlaylist(selectedPlaylist_) != nullptr) {
        queue_.SetTracks(playlists_.ResolveTracksRecursive(selectedPlaylist_), std::nullopt);
    }
}

void App::AddFilesToSelectedPlaylist() {
    const auto* selected = playlists_.FindPlaylist(selectedPlaylist_);
    if (selected == nullptr || selected->kind == playlist::PlaylistKind::Youtube) {
        if (window_ && window_->WindowHandle()) {
            MessageBoxW(window_->WindowHandle(),
                        L"Select a music playlist or folder first. ADD is unavailable in Youtube.",
                        L"Rivan", MB_OK | MB_ICONINFORMATION);
        }
        return;
    }
    auto files = PickAudioFiles();
    if (files.empty()) return;

    if (selected->kind == playlist::PlaylistKind::Directory) {
        bool copied = false;
        for (const auto& file : files) {
            if (!library::Track::IsSupportedFile(file)) continue;
            std::error_code ec;
            const auto destination = core::UniqueDestination(selected->directory, file.filename());
            std::filesystem::copy_file(file, destination, std::filesystem::copy_options::none, ec);
            copied = !ec || copied;
        }
        if (copied) RefreshLibrary();
        return;
    }

    bool changed = false;
    for (const auto& file : files) {
        if (!library::Track::IsSupportedFile(file)) continue;
        changed = playlists_.AddExternalTrack(selectedPlaylist_, library::Track::FromFile(file)) || changed;
    }
    if (!changed) return;
    ReseedSelectedUserQueue();
    SaveUserPlaylists();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::RemoveTracksAt(std::span<const std::size_t> indices) {
    const auto* selected = playlists_.FindPlaylist(selectedPlaylist_);
    if (selected != nullptr && selected->kind == playlist::PlaylistKind::Directory) {
        const auto tracks = playlists_.ResolveTracks(selectedPlaylist_);
        bool changed = false;
        for (const auto index : indices) {
            if (index >= tracks.size()) continue;
            std::error_code ec;
            changed = std::filesystem::remove(tracks[index].filePath, ec) || changed;
        }
        if (changed) RefreshLibrary();
        return;
    }
    if (!playlists_.RemoveTracksAt(selectedPlaylist_, indices)) return;
    ReseedSelectedUserQueue();
    SaveUserPlaylists();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::ReorderSelectedTracks(playlist::PlaylistId playlistId,
                                std::span<const std::size_t> indices,
                                std::size_t destination) {
    std::vector<std::size_t> ordered(indices.begin(), indices.end());
    if (!playlists_.MoveTracks(playlistId, std::move(ordered), destination)) return;
    ReseedSelectedUserQueue();
    const auto* source = playlists_.FindPlaylist(playlistId);
    if (source != nullptr && source->kind == playlist::PlaylistKind::Directory) {
        SaveTrackOrder(playlistId);
    } else {
        SaveUserPlaylists();
    }
    ++revision_;
    if (window_) window_->Refresh();
}

void App::AddTracksToPlaylist(std::uint64_t targetPlaylistId, std::span<const std::size_t> indices) {
    const auto* target = playlists_.FindPlaylist(targetPlaylistId);
    if (target == nullptr) return;
    if (target->kind == playlist::PlaylistKind::Directory) {
        bool copied = false;
        for (const auto index : indices) {
            const auto id = TrackIdAtIndex(index);
            if (!id) continue;
            const auto* track = playlists_.FindTrack(*id);
            if (track == nullptr) continue;
            std::error_code ec;
            const auto destination = core::UniqueDestination(target->directory, track->filePath.filename());
            std::filesystem::copy_file(track->filePath, destination, std::filesystem::copy_options::none, ec);
            copied = !ec || copied;
        }
        if (copied) RefreshLibrary();
        return;
    }
    if (target->kind != playlist::PlaylistKind::User) return;
    bool changed = false;
    for (const auto index : indices) {
        const auto id = TrackIdAtIndex(index);
        if (!id) continue;
        const auto* track = playlists_.FindTrack(*id);
        if (track == nullptr) continue;
        changed = playlists_.AddExternalTrack(targetPlaylistId, *track) || changed;
    }
    if (!changed) return;
    if (targetPlaylistId == selectedPlaylist_) ReseedSelectedUserQueue();
    SaveUserPlaylists();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::MoveTracksToPlaylist(std::uint64_t targetPlaylistId, std::span<const std::size_t> indices) {
    if (targetPlaylistId == selectedPlaylist_ || indices.empty()) return;
    const auto* source = playlists_.FindPlaylist(selectedPlaylist_);
    const auto* target = playlists_.FindPlaylist(targetPlaylistId);
    if (source == nullptr || target == nullptr || source->kind != playlist::PlaylistKind::User) return;

    // Add first so failed destination leaves source entries untouched.
    if (target->kind == playlist::PlaylistKind::Directory) {
        bool copied = false;
        for (const auto index : indices) {
            const auto id = TrackIdAtIndex(index);
            const auto* track = id ? playlists_.FindTrack(*id) : nullptr;
            if (track == nullptr) continue;
            std::error_code ec;
            const auto destination = core::UniqueDestination(target->directory, track->filePath.filename());
            std::filesystem::copy_file(track->filePath, destination, std::filesystem::copy_options::none, ec);
            copied = !ec || copied;
        }
        if (!copied || !playlists_.RemoveTracksAt(selectedPlaylist_, indices)) return;
        SaveUserPlaylists();
        RefreshLibrary();
        return;
    }
    if (target->kind != playlist::PlaylistKind::User) return;

    bool added = false;
    for (const auto index : indices) {
        const auto id = TrackIdAtIndex(index);
        const auto* track = id ? playlists_.FindTrack(*id) : nullptr;
        if (track != nullptr) added = playlists_.AddExternalTrack(targetPlaylistId, *track) || added;
    }
    if (!added || !playlists_.RemoveTracksAt(selectedPlaylist_, indices)) return;
    ReseedSelectedUserQueue();
    SaveUserPlaylists();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::RenameTrackAt(std::size_t index, std::wstring name) {
    const auto id = TrackIdAtIndex(index);
    const auto* track = id ? playlists_.FindTrack(*id) : nullptr;
    if (track == nullptr || !track->IsAvailable() || !core::IsValidFileName(name)) return;
    const auto oldPath = track->filePath;
    const auto newPath = oldPath.parent_path() / (name + oldPath.extension().wstring());
    if (oldPath == newPath) return;

    std::error_code ec;
    if (std::filesystem::exists(newPath, ec) || ec) return;
    std::filesystem::rename(oldPath, newPath, ec);
    if (ec) return;

    // A file rename leaves the embedded title tag untouched, so the displayed name
    // (which prefers the tag over the file name) would keep the old name in the
    // library and Rich Presence. Write the new name into the tag too so every
    // surface and future rescans agree. Best effort: formats without a writable tag
    // (or a missing ffmpeg fallback) still get the in-memory title below.
    (void)ui::WriteTrackMetadataValue(newPath.wstring(), ui::TrackMetadataField::Title, name);

    auto replacement = library::Track::FromFile(newPath);
    replacement.title = name;
    (void)playlists_.ReplaceTrack(*id, replacement);
    stats_.ApplyTrackRename(*id, replacement.id, oldPath, replacement.filePath);
    lyrics_.NotifyTrackRenamed(*id, replacement.id, oldPath, replacement.filePath);
    if (activeTrack_ && activeTrack_->id == *id) {
        activeTrack_ = replacement;
        selectedTrack_ = replacement.id;
        RefreshActiveLyrics();
    }
    ReseedSelectedUserQueue();
    SaveUserPlaylists();
    RefreshLibrary();
}

void App::ChangeTracksCover(std::span<const std::size_t> indices) {
    const auto image = PickCoverImage();
    if (image.empty()) return;

    // --- Audio release for playing track ---
    AudioReleaseGuard guard;
    bool releaseObserved = false;
    if (activeTrack_ && audio_.Live().hasMedia) {
        for (const auto index : indices) {
            const auto id = TrackIdAtIndex(index);
            if (id && activeTrack_->id == *id) {
                const auto live = audio_.Live();
                guard.wasPlaying = live.state == audio::PlaybackState::Playing;
                guard.position = live.position;
                guard.filePath = activeTrack_->filePath;
                audio_.Pause();
                audio_.Load(L"__RIVAN_EDIT__");
                guard.released = true;      // restore is owed once the placeholder load is enqueued
                releaseObserved = WaitForAudioRelease();
                break;
            }
        }
    }
    // --- End audio release ---

    bool changed = false;
    for (const auto index : indices) {
        const auto id = TrackIdAtIndex(index);
        const auto* track = id ? playlists_.FindTrack(*id) : nullptr;
        if (track == nullptr || !library::Track::IsAudioFile(track->filePath)) continue;
        // The audio worker may still hold this file open when the bounded wait
        // expired; rewriting it then would fail with a sharing violation.
        if (!releaseObserved && activeTrack_ && activeTrack_->id == *id) continue;
        if (!SUCCEEDED(WriteCoverArt(track->filePath, image))) continue;
        changed = true;
        // Cover art is not stored in the Track model — no in-memory update needed.
    }

    // --- Restore audio after edit ---
    if (guard.released) {
        audio_.Load(guard.filePath);
        audio_.Seek(guard.position);
        if (guard.wasPlaying) audio_.Play();
    }
    // --- End restore ---

    if (!changed) {
        if (window_ && window_->WindowHandle()) {
            MessageBoxW(window_->WindowHandle(),
                        L"Windows could not update cover art for selected audio file(s).",
                        L"Rivan", MB_OK | MB_ICONWARNING);
        }
        return;
    }
    // No need to ReseedSelectedUserQueue/SaveUserPlaylists — Track model unchanged.
    ++revision_;
    if (window_) window_->Refresh();
}

void App::ChangeTrackMetadata(std::span<const std::size_t> indices,
                              ui::TrackMetadataField field,
                              const std::wstring& value) {
    // --- Audio release for playing track ---
    AudioReleaseGuard guard;
    bool releaseObserved = false;
    if (activeTrack_ && audio_.Live().hasMedia) {
        for (const auto index : indices) {
            const auto id = TrackIdAtIndex(index);
            if (id && activeTrack_->id == *id) {
                const auto live = audio_.Live();
                guard.wasPlaying = live.state == audio::PlaybackState::Playing;
                guard.position = live.position;
                guard.filePath = activeTrack_->filePath;
                audio_.Pause();
                audio_.Load(L"__RIVAN_EDIT__");
                guard.released = true;      // restore is owed once the placeholder load is enqueued
                releaseObserved = WaitForAudioRelease();
                break;
            }
        }
    }
    // --- End audio release ---

    bool changed = false;
    for (const auto index : indices) {
        const auto id = TrackIdAtIndex(index);
        const auto* track = id ? playlists_.FindTrack(*id) : nullptr;
        if (track == nullptr || !library::Track::IsAudioFile(track->filePath)) continue;
        // The audio worker may still hold this file open when the bounded wait
        // expired; rewriting it then would fail with a sharing violation.
        if (!releaseObserved && activeTrack_ && activeTrack_->id == *id) continue;
        if (!ui::WriteTrackMetadataValue(track->filePath.wstring(), field, value)) continue;
        changed = true;

        // Update in-memory track: copy original, patch edited field, preserve duration/bitrate
        auto replacement = *track;
        switch (field) {
        case ui::TrackMetadataField::Author: replacement.artist = value; break;
        case ui::TrackMetadataField::Album:  replacement.album = value; break;
        default: break; // Genre/Year not stored in Track model
        }
        (void)playlists_.ReplaceTrack(*id, replacement);
        if (activeTrack_ && activeTrack_->id == *id) {
            activeTrack_ = replacement;
            selectedTrack_ = replacement.id;
        }
    }

    // --- Restore audio after edit ---
    if (guard.released) {
        audio_.Load(guard.filePath);
        audio_.Seek(guard.position);
        if (guard.wasPlaying) audio_.Play();
    }
    // --- End restore ---

    if (!changed) {
        if (window_ && window_->WindowHandle()) {
            MessageBoxW(window_->WindowHandle(),
                        L"Could not update metadata for selected audio file(s).",
                        L"Rivan", MB_OK | MB_ICONWARNING);
        }
        return;
    }
    ReseedSelectedUserQueue();
    SaveUserPlaylists();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::DuplicateTracksAt(std::span<const std::size_t> indices) {
    const auto* selected = playlists_.FindPlaylist(selectedPlaylist_);
    if (selected == nullptr) return;
    if (selected->kind == playlist::PlaylistKind::Directory) {
        const auto tracks = playlists_.ResolveTracks(selectedPlaylist_);
        bool copied = false;
        for (const auto index : indices) {
            if (index >= tracks.size()) continue;
            const auto destination = core::UniqueDestination(selected->directory, tracks[index].filePath.filename());
            std::error_code ec;
            std::filesystem::copy_file(tracks[index].filePath, destination, std::filesystem::copy_options::none, ec);
            copied = !ec || copied;
        }
        if (copied) RefreshLibrary();
        return;
    }
    if (selected->kind != playlist::PlaylistKind::User) return;
    const bool asFile = settings_.Settings().duplicateAsFile;
    bool changed = false;
    if (asFile) {
        for (const auto index : indices) {
            const auto id = TrackIdAtIndex(index);
            if (!id) continue;
            const auto* track = playlists_.FindTrack(*id);
            if (track == nullptr) continue;
            auto copy = DuplicateFileOnDisk(track->filePath);
            if (copy.empty()) continue;
            changed = playlists_.AddExternalTrack(selectedPlaylist_, library::Track::FromFile(copy)) || changed;
        }
    } else {
        std::vector<std::size_t> sorted(indices.begin(), indices.end());
        std::sort(sorted.begin(), sorted.end(), std::greater<>{});
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        for (const auto index : sorted) changed = playlists_.DuplicateTrackAt(selectedPlaylist_, index) || changed;
    }
    if (!changed) return;
    ReseedSelectedUserQueue();
    SaveUserPlaylists();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::ReorderUserPlaylist(std::uint64_t id, std::uint64_t beforeId) {
    const auto* playlist = playlists_.FindPlaylist(id);
    if (playlist == nullptr) return;
    if (playlist->kind == playlist::PlaylistKind::Directory) {
        if (!playlists_.MoveFolder(id, beforeId)) return;
        SaveFolderOrder();
        ++revision_;
        if (window_) window_->Refresh();
        return;
    }
    if (!playlists_.MoveUserPlaylist(id, beforeId)) return;
    SaveUserPlaylists();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::MovePlaylistInto(std::uint64_t id, std::uint64_t parentId) {
    const auto* source = playlists_.FindPlaylist(id);
    const auto* parent = playlists_.FindPlaylist(parentId);
    if (source == nullptr || parent == nullptr || source->kind != playlist::PlaylistKind::Directory ||
        parent->kind != playlist::PlaylistKind::Directory || id == parentId) return;

    std::error_code ec;
    const auto relativeParent = std::filesystem::relative(parent->directory, source->directory, ec);
    if (ec || relativeParent.empty() || relativeParent == L"." ||
        (!relativeParent.empty() && *relativeParent.begin() != L"..")) return;
    const auto destination = parent->directory / source->directory.filename();
    if (destination == source->directory || std::filesystem::exists(destination, ec) || ec) return;
    std::filesystem::rename(source->directory, destination, ec);
    if (ec) return;

    selectedPlaylist_ = playlist::AllMusicPlaylistId;
    selectedTrack_ = 0;
    RefreshLibrary();
}

void App::SetDuplicateAsFile(bool enabled) {
    auto settings = settings_.Settings();
    if (settings.duplicateAsFile == enabled) return;
    settings.duplicateAsFile = enabled;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    ++revision_;
    if (window_) window_->Refresh();
}

std::filesystem::path App::DuplicateFileOnDisk(const std::filesystem::path& source) const {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec) || ec) return {};
    const auto directory = source.parent_path();
    const auto stem = source.stem().wstring();
    const auto ext = source.extension().wstring();
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const std::wstring suffix = attempt == 0 ? L" - Copy"
                                                 : L" - Copy (" + std::to_wstring(attempt + 1) + L")";
        auto candidate = directory / (stem + suffix + ext);
        ec.clear();
        if (std::filesystem::exists(candidate, ec)) continue;
        ec.clear();
        std::filesystem::copy_file(source, candidate, std::filesystem::copy_options::none, ec);
        if (!ec) return candidate;
    }
    return {};
}

std::vector<std::filesystem::path> App::PickAudioFiles() const {
    std::vector<std::filesystem::path> result;
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.GetAddressOf())))) return result;
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    }
    static const COMDLG_FILTERSPEC filters[] = {
        {L"Audio / video files", L"*.mp3;*.wav;*.flac;*.mp4;*.m4a;*.opus;*.webm;*.ogg;*.aac;*.m4v"},
        {L"All files", L"*.*"},
    };
    dialog->SetFileTypes(2, filters);
    const HWND owner = window_ ? window_->WindowHandle() : nullptr;
    if (FAILED(dialog->Show(owner))) return result;
    Microsoft::WRL::ComPtr<IShellItemArray> items;
    if (FAILED(dialog->GetResults(items.GetAddressOf()))) return result;
    DWORD count = 0;
    if (FAILED(items->GetCount(&count))) return result;
    for (DWORD i = 0; i < count; ++i) {
        Microsoft::WRL::ComPtr<IShellItem> item;
        if (FAILED(items->GetItemAt(i, item.GetAddressOf()))) continue;
        PWSTR raw = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw != nullptr) {
            result.emplace_back(raw);
            CoTaskMemFree(raw);
        }
    }
    return result;
}

std::filesystem::path App::PickCoverImage() const {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(dialog.GetAddressOf())))) return {};
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    }
    static const COMDLG_FILTERSPEC filters[] = {
        {L"Image files", L"*.jpg;*.jpeg;*.png;*.bmp;*.webp"},
        {L"All files", L"*.*"},
    };
    dialog->SetFileTypes(2, filters);
    if (FAILED(dialog->Show(window_ ? window_->WindowHandle() : nullptr))) return {};
    Microsoft::WRL::ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.GetAddressOf()))) return {};
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || raw == nullptr) return {};
    std::filesystem::path result(raw);
    CoTaskMemFree(raw);
    return result;
}

} // namespace rivan
