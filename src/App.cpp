// App.cpp
// Owns Rivan's services and translates window-thread UI actions into queue, audio,
// persistence, scanning, and visualization operations.
#include "App.h"

#include "core/AppPaths.h"
#include "core/Text.h"

#include <Windows.h>

#pragma comment(lib, "advapi32.lib")

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace rivan {
namespace {

std::wstring CurrentExecutablePath() {
    std::wstring path(260, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                                static_cast<DWORD>(path.size()));
        if (length == 0) return {};
        if (length < path.size() - 1) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

// IniDocument::SaveAtomic writes "<name>.tmp.<pid>.<sequence>" next to the target
// and renames it onto the destination. A crash between the temporary write and the
// rename strands garbage that accumulates across saves; delete any leftovers at
// startup. A surviving temp is always garbage from a crashed save, regardless of age.
void SweepStaleTemporaryIniFiles() {
    std::error_code error;
    std::filesystem::directory_iterator iterator(core::AppPaths::LocalDataRoot(), error);
    if (error) return;
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        if (iterator->is_regular_file(error)) {
            const std::wstring name = iterator->path().filename().wstring();
            if (name.find(L".tmp.") != std::wstring::npos) {
                std::filesystem::remove(iterator->path(), error);
            }
        }
        error.clear();
        iterator.increment(error);
        if (error) return;  // Best effort only; never block startup.
    }
}

// Keep a restored window from never being visible again: if the persisted
// rectangle misses the nearest monitor's work area entirely, park it at the
// work-area top-left preserving its size so at least a corner ends up on-screen.
void ClampWindowToWorkArea(RECT& rectangle) {
    const HMONITOR nearest = MonitorFromRect(&rectangle, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (nearest == nullptr || !GetMonitorInfoW(nearest, &monitor)) return;
    const RECT work = monitor.rcWork;
    RECT overlap{};
    if (IntersectRect(&overlap, &rectangle, &work) && !IsRectEmpty(&overlap)) {
        return;  // Already shares at least a corner/edge with the work area.
    }
    const int width = rectangle.right - rectangle.left;
    const int height = rectangle.bottom - rectangle.top;
    rectangle.left = work.left;
    rectangle.top = work.top;
    rectangle.right = rectangle.left + width;
    rectangle.bottom = rectangle.top + height;
}

} // namespace

App::App(HINSTANCE instance)
    : instance_(instance),
      lyrics_(core::AppPaths::LocalDataRoot() / L"lyrics"),
      stats_([this] { return audio_.Live(); },
             core::AppPaths::LocalDataRoot() / L"Stats") {
    youtube_.SetNotify([this]() {
        youtubeDirty_.store(true, std::memory_order_release);
        if (window_ && window_->WindowHandle()) {
            PostMessageW(window_->WindowHandle(), WM_APP + 40, 0, 0);
        }
    });
    lyrics_.SetNotify([this]() {
        lyricsDirty_.store(true, std::memory_order_release);
        if (window_ && window_->WindowHandle()) {
            PostMessageW(window_->WindowHandle(), WM_APP + 42, 0, 0);
        }
    });
    update_.SetNotify([this]() {
        updateDirty_.store(true, std::memory_order_release);
        const HWND window = updateNotificationWindow_.load(std::memory_order_acquire);
        if (window && PostMessageW(window, ui::kUpdateServiceMessage, 0, 0)) return;
        if (uiThreadId_ != 0) {
            (void)PostThreadMessageW(uiThreadId_, ui::kUpdateServiceMessage, 0, 0);
        }
    });
}

App::~App() {
    updateNotificationWindow_.store(nullptr, std::memory_order_release);
    update_.Shutdown();
    stats_.Flush();
    audioNotificationWindow_.store(nullptr, std::memory_order_release);
    youtube_.Reset();
    lyrics_.Reset();
    lyrics_.Shutdown();
    if (scanThread_.joinable()) {
        scanThread_.request_stop();
        scanThread_.join();
    }
    if (!persistedOnClose_) PersistState();
}

bool App::Initialize() {
    uiThreadId_ = GetCurrentThreadId();
    std::wstring pathError;
    if (!core::AppPaths::EnsureDirectories(&pathError)) return false;

    SweepStaleTemporaryIniFiles();

    std::string error;
    // A corrupt settings.ini or session.ini must not destroy the other half:
    // load and reset each file independently.
    if (!settings_.LoadSettings(&error, nullptr)) {
        settings_.ResetSettings();
        // Best-effort one-time repair: rewrite defaults so a corrupt file does not
        // re-fail (and silently re-reset) on every boot. A read-only drive just fails
        // again; the in-memory defaults remain valid either way.
        (void)settings_.SaveSettings(&error);
    }
    if (!settings_.LoadSession(&error, nullptr)) settings_.ResetSession();

    auto applicationSettings = settings_.Settings();
    std::error_code filesystemError;
    if (applicationSettings.musicRoot.empty()) {
        applicationSettings.musicRoot = core::AppPaths::DefaultMusicRoot();
    }
    std::filesystem::create_directories(applicationSettings.musicRoot, filesystemError);
    if (filesystemError || !std::filesystem::is_directory(applicationSettings.musicRoot, filesystemError)) {
        applicationSettings.musicRoot = core::AppPaths::DefaultMusicRoot();
        filesystemError.clear();
        std::filesystem::create_directories(applicationSettings.musicRoot, filesystemError);
        if (filesystemError) return false;
    }
    (void)settings_.SetSettings(applicationSettings, &error);
    lyrics_.SetCacheEnabled(applicationSettings.lyricsCacheEnabled);
    stats_.SetEnabled(applicationSettings.statsEnabled);
    std::wstring startupError;
    if (!SyncStartupRegistration(applicationSettings.startAtStartup, &startupError)) {
        applicationSettings.startAtStartup = false;
        (void)settings_.SetSettings(applicationSettings, &error);
        (void)settings_.SaveSettings(&error);
    }

    (void)skins_.Refresh(&error, nullptr);
    committedSkin_ = skins_.Resolve(applicationSettings.skinId);
    activeSkin_ = committedSkin_;

    miniPlayer_ = settings_.Session().miniMode;
    moduleLayout_ = settings_.Session().moduleLayout;
    moduleLayout_.NormalizeTabState();
    if (!moduleLayout_.HasValidGeometry()) {
        moduleLayout_ = ui::ModuleLayout::Defaults();
    }
    for (std::size_t i = 0; i < moduleLayout_.items.size(); ++i) {
        auto& item = moduleLayout_.items[i];
        if (moduleLayout_.Find(moduleLayout_.snapGroup[i]) == nullptr) {
            moduleLayout_.snapGroup[i] = item.id;
        }
    }
    moduleLayoutWarning_ = moduleLayout_.DisableDuplicateIndependentModules();
    if (moduleLayoutWarning_) {
        auto session = settings_.Session();
        session.moduleLayout = moduleLayout_;
        (void)settings_.SetSession(std::move(session), &error);
        (void)settings_.SaveSession(&error);
    }
    queue_.SetShuffle(settings_.Session().shuffle);
    queue_.SetRepeat(ToQueueRepeat(settings_.Session().repeat));
    audio_.SetVolume(static_cast<float>(applicationSettings.volumePercent) / 100.0F);
    audio_.SetEventCallback([this](const audio::AudioEvent& event) {
        if (event.type == audio::AudioEventType::EndOfStream) {
            endOfStream_.store(true, std::memory_order_release);
        }
        audioChanged_.store(true, std::memory_order_release);
        NotifyAudioSignal();
    });

    youtube_.RefreshToolStatus();
    youtubeView_ = youtube_.Snapshot();

    discord_.SetEnabled(applicationSettings.discordEnabled);

    window_ = std::make_unique<ui::Win32Ui>(*this);
    ui::WindowOptions options;
    options.initialWidth = miniPlayer_ ? 520 : settings_.Session().window.width;
    options.initialHeight = miniPlayer_ ? 210 : settings_.Session().window.height;
    if (!window_->Create(instance_, options)) return false;
    if (applicationSettings.youtubeEnabled) {
        youtubeGrabberHotkeyAvailable_ = window_->UpdateYoutubeGrabberHotkey(
            applicationSettings.youtubeGrabberHotkeyModifiers,
            applicationSettings.youtubeGrabberHotkeyVirtualKey);
    }
    audioNotificationWindow_.store(window_->WindowHandle(), std::memory_order_release);
    updateNotificationWindow_.store(window_->WindowHandle(), std::memory_order_release);

    if (!miniPlayer_) {
        const auto& rectangle = settings_.Session().window;
        RECT target{rectangle.x, rectangle.y,
                    rectangle.x + rectangle.width, rectangle.y + rectangle.height};
        ClampWindowToWorkArea(target);
        SetWindowPos(window_->WindowHandle(), nullptr, target.left, target.top,
                     rectangle.width, rectangle.height, SWP_NOACTIVATE | SWP_NOZORDER);
    }
    // User playlists are restored lazily on the first scan application (see
    // ApplyCompletedScan) so per-track metadata reads do not delay the scan start.
    library::LibraryScanner::SetDurationCachePath(core::AppPaths::LocalDataRoot() / L"library-durations.cache");
    StartLibraryScan();
    // Start only after the window exists: worker notifications are always marshalled to
    // the UI thread and never delay startup or first paint.
    update_.StartCheck();
    return true;
}

int App::Run() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        ApplyCompletedScan();
        HandleAudioSignals();
        if (youtubeDirty_.exchange(false, std::memory_order_acq_rel)) {
            OnYoutubeServiceUpdated();
        }
        if (lyricsDirty_.exchange(false, std::memory_order_acq_rel)) {
            OnLyricsServiceUpdated();
        }
        if (updateDirty_.exchange(false, std::memory_order_acq_rel)) {
            OnUpdateServiceUpdated();
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void App::SetModuleLayout(ui::ModuleLayout layout) {
    for (auto& item : layout.items) {
        item.x = std::clamp(item.x, 0.0F, 1.0F);
        item.y = std::clamp(item.y, 0.0F, 1.0F);
        item.width = std::clamp(item.width, item.collapsed ? 0.001F : 0.10F, 1.0F);
        item.height = std::clamp(item.height, item.collapsed ? 0.001F : 0.10F, 1.0F);
        item.x = std::min(item.x, 1.0F - item.width);
        item.y = std::min(item.y, 1.0F - item.height);
        if (!item.collapsed) ui::ModuleLayout::SyncExpandedGeometry(item);
    }
    layout.NormalizeTabState();
    for (std::size_t i = 0; i < layout.items.size(); ++i) {
        if (layout.Find(layout.snapGroup[i]) == nullptr) {
            layout.snapGroup[i] = layout.items[i].id;
        }
    }
    moduleLayoutWarning_ = moduleLayoutWarning_ || layout.DisableDuplicateIndependentModules();
    moduleLayout_ = layout;
    auto session = settings_.Session();
    session.moduleLayout = moduleLayout_;
    std::string error;
    (void)settings_.SetSession(std::move(session), &error);
    // Layout messages stream in during interactive resize; keep the in-memory session in
    // sync every time but throttle the INI write to at most once per ~1.5 s.
    const auto now = std::chrono::steady_clock::now();
    if (now - lastSessionSave_ >= std::chrono::milliseconds(1500)) {
        lastSessionSave_ = now;
        (void)settings_.SaveSession(&error);
    }
    ++revision_;
    if (window_) window_->Refresh();
}

void App::Invoke(ui::Command command) {
    const auto status = audio_.Status();
    switch (command) {
    case ui::Command::PlayPause:
        if (status.state == audio::PlaybackState::Playing) {
            audio_.Pause();
        } else if (status.hasMedia) {
            audio_.Play();
        } else {
            PlayNavigation(queue_.Current() ? playlist::QueueNavigation{
                playlist::QueueNavigationAction::Restarted, queue_.Current()} : queue_.Start());
        }
        break;
    case ui::Command::Stop:
        audio_.Stop();
        break;
    case ui::Command::Previous:
        PlayNavigation(queue_.Previous());
        break;
    case ui::Command::Next:
        PlayNavigation(queue_.Next());
        break;
    case ui::Command::ToggleShuffle:
        queue_.SetShuffle(!queue_.Shuffle());
        break;
    case ui::Command::CycleRepeat:
        switch (queue_.Repeat()) {
        case playlist::RepeatMode::Off: queue_.SetRepeat(playlist::RepeatMode::All); break;
        case playlist::RepeatMode::All: queue_.SetRepeat(playlist::RepeatMode::One); break;
        case playlist::RepeatMode::One: queue_.SetRepeat(playlist::RepeatMode::Off); break;
        }
        break;
    case ui::Command::ToggleSettings:
        settingsVisible_ = !settingsVisible_;
        break;
    case ui::Command::ToggleSkinStudio:
        skinStudioVisible_ = !skinStudioVisible_;
        if (skinStudioVisible_) {
            settingsVisible_ = false;
            // Settings entry point forks a new skin on save; EditSkin sets the flag true.
            skinStudioEditExisting_ = false;
        }
        break;
    case ui::Command::ToggleMiniPlayer:
        ToggleMiniPlayer();
        break;
    case ui::Command::VolumeUp:
        SetVolume(status.volume + 0.05F);
        break;
    case ui::Command::VolumeDown:
        SetVolume(status.volume - 0.05F);
        break;
    case ui::Command::SeekBackward:
        audio_.Seek(std::max(status.position - std::chrono::seconds(5), std::chrono::nanoseconds{}));
        break;
    case ui::Command::SeekForward:
        audio_.Seek(status.duration.count() > 0
                        ? std::min(status.position + std::chrono::seconds(5), status.duration)
                        : status.position + std::chrono::seconds(5));
        break;
    }
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SelectPlaylist(std::uint64_t id) {
    if (id == playlist::YoutubePlaylistId) {
        if (!YoutubeFeatureOn()) return;
        selectedPlaylist_ = id;
        selectedTrack_ = 0;
        SyncPlaybackSession();
        youtubeSelectedResult_ = 0;
        if (youtubeView_.entries.empty()) ShowYoutubeLocalLibrary();
        // Prefetch yt-dlp so the first typed search skips cold process load.
        youtube_.Warm();
        ++revision_;
        if (window_) window_->Refresh();
        return;
    }
    if (playlists_.FindPlaylist(id) == nullptr) return;
    selectedPlaylist_ = id;
    selectedTrack_ = 0;
    SyncPlaybackSession();
    // Selecting a folder plays everything under it (its loose tracks plus descendants).
    queue_.SetTracks(playlists_.ResolveTracksRecursive(id), std::nullopt);
    // Selecting a folder with subfolders also reveals them, so a single click both
    // opens the folder and shows its playlists in the tree.
    if (playlists_.HasChildren(id)) expandedPlaylists_.insert(id);
    ++revision_;
    if (window_) window_->Refresh();
}

void App::TogglePlaylistExpanded(std::uint64_t id) {
    if (const auto position = expandedPlaylists_.find(id); position != expandedPlaylists_.end()) {
        expandedPlaylists_.erase(position);
    } else {
        expandedPlaylists_.insert(id);
    }
    ++revision_;
    if (window_) window_->Refresh();
}

void App::RefreshLibrary() {
    restored_ = true;  // Manual rescan must not re-apply saved session selection.
    StartLibraryScan();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::ActivateTrack(std::uint64_t id) {
    auto findInQueue = [this, id]() {
        const auto& tracks = queue_.Tracks();
        return std::find_if(tracks.begin(), tracks.end(), [id](const auto& track) {
            return track.id == id;
        });
    };

    auto found = findInQueue();
    if (found == queue_.Tracks().end()) {
        // Global-library search results may not belong to the selected playlist.
        // Move to All Music so subsequent previous/next navigation remains coherent.
        if (playlists_.FindTrack(id) == nullptr ||
            playlists_.FindPlaylist(playlist::AllMusicPlaylistId) == nullptr) {
            return;
        }
        selectedPlaylist_ = playlist::AllMusicPlaylistId;
        queue_.SetTracks(playlists_.ResolveTracks(selectedPlaylist_), std::nullopt);
        found = findInQueue();
        if (found == queue_.Tracks().end()) return;
    }

    PlayNavigation(queue_.Play(static_cast<std::size_t>(
        std::distance(queue_.Tracks().begin(), found))));
}

void App::Seek(double normalizedPosition) {
    const auto status = audio_.Status();
    if (!status.hasMedia || status.duration.count() <= 0 || !std::isfinite(normalizedPosition)) return;
    normalizedPosition = std::clamp(normalizedPosition, 0.0, 1.0);
    audio_.Seek(std::chrono::nanoseconds(static_cast<std::int64_t>(
        static_cast<double>(status.duration.count()) * normalizedPosition)));
}

void App::SetVolume(float normalizedVolume) {
    if (!std::isfinite(normalizedVolume)) return;
    audio_.SetVolume(std::clamp(normalizedVolume, 0.0F, 1.0F));
}

void App::SelectSettingsCategory(ui::SettingCategory category) {
    if (category == ui::SettingCategory::Statistics && !settings_.Settings().statsEnabled) {
        category = ui::SettingCategory::Integrations;
    }
    if (settingsCategory_ == category) return;
    settingsCategory_ = category;
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetMusicFolder(std::size_t index, std::filesystem::path folder) {
    auto settings = settings_.Settings();
    if (index == 0) {
        if (folder.empty()) return;  // Primary root is required.
        settings.musicRoot = std::move(folder);
    } else {
        const std::size_t additionalIndex = index - 1;
        auto& roots = settings.additionalMusicRoots;
        if (folder.empty()) {
            if (additionalIndex >= roots.size()) return;
            roots.erase(roots.begin() + static_cast<std::ptrdiff_t>(additionalIndex));
        } else if (additionalIndex < roots.size()) {
            roots[additionalIndex] = std::move(folder);
        } else if (additionalIndex == roots.size()) {
            roots.push_back(std::move(folder));
        } else {
            return;  // Gap not allowed.
        }
    }
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    RefreshLibrary();  // Rescan with the new folder set.
}

void App::SetFilePreviewEnabled(bool enabled) {
    auto settings = settings_.Settings();
    if (settings.filePreviewEnabled == enabled) return;
    settings.filePreviewEnabled = enabled;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetPreviewFitWindow(bool enabled) {
    auto settings = settings_.Settings();
    if (settings.previewFitWindow == enabled) return;
    settings.previewFitWindow = enabled;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetSongRowLayout(ui::SongRowLayout layout) {
    auto settings = settings_.Settings();
    if (settings.songRowLayout.rowHeight == layout.rowHeight &&
        settings.songRowLayout.fields == layout.fields) return;
    settings.songRowLayout = layout;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    ++revision_;
    if (window_) window_->Refresh();
}

void App::PreviewSongRowLayout(ui::SongRowLayout layout) {
    auto settings = settings_.Settings();
    if (settings.songRowLayout.rowHeight == layout.rowHeight &&
        settings.songRowLayout.fields == layout.fields) return;
    settings.songRowLayout = layout;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    ++revision_;
    if (window_) window_->Refresh();
}

bool App::SyncStartupRegistration(bool enabled, std::wstring* error) {
    constexpr wchar_t kRunKey[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr wchar_t kValueName[] = L"Rivan";

    HKEY key{};
    LSTATUS status = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0,
                                   KEY_SET_VALUE | KEY_QUERY_VALUE, &key);
    if (status != ERROR_SUCCESS) {
        if (error) *error = L"Unable to open the Windows startup registry key.";
        return false;
    }

    bool success = true;
    if (enabled) {
        const std::wstring exePath = CurrentExecutablePath();
        if (exePath.empty()) {
            if (error) *error = L"Unable to resolve the Rivan executable path.";
            success = false;
        } else {
            // Quote the path so it survives spaces when Windows launches it.
            const std::wstring command = L"\"" + exePath + L"\"";
            const auto bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
            status = RegSetValueExW(key, kValueName, 0, REG_SZ,
                                    reinterpret_cast<const BYTE*>(command.c_str()), bytes);
            if (status != ERROR_SUCCESS) {
                if (error) *error = L"Unable to write the Rivan startup entry.";
                success = false;
            }
        }
    } else {
        status = RegDeleteValueW(key, kValueName);
        // Absent value is not an error: the state we want (no autostart) already holds.
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
            if (error) *error = L"Unable to remove the Rivan startup entry.";
            success = false;
        }
    }
    RegCloseKey(key);
    return success;
}

void App::SetStartAtStartup(bool enabled) {
    auto settings = settings_.Settings();
    if (settings.startAtStartup == enabled) return;
    // Only persist the toggle if the registry write/removal actually succeeds.
    if (!SyncStartupRegistration(enabled)) return;
    settings.startAtStartup = enabled;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetExitToTray(bool enabled) {
    auto settings = settings_.Settings();
    if (settings.exitToTray == enabled) return;
    settings.exitToTray = enabled;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetModuleExpansionBehavior(ui::ModuleExpansionBehavior behavior) {
    auto settings = settings_.Settings();
    if (settings.moduleExpansionBehavior == behavior) return;
    settings.moduleExpansionBehavior = behavior;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetWindowResizeBehavior(ui::WindowResizeBehavior behavior) {
    auto settings = settings_.Settings();
    if (settings.windowResizeBehavior == behavior) return;
    settings.windowResizeBehavior = behavior;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetModuleResizeBehavior(ui::ModuleResizeBehavior behavior) {
    auto settings = settings_.Settings();
    if (settings.moduleResizeBehavior == behavior) return;
    settings.moduleResizeBehavior = behavior;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    ++revision_;
    if (window_) window_->Refresh();
}

} // namespace rivan
