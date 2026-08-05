// AppPersistence.cpp
// Application and playback-session persistence.
#include "App.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

namespace rivan {

void App::PersistState() {
    auto applicationSettings = settings_.Settings();
    const auto status = audio_.Status();
    applicationSettings.volumePercent = static_cast<int>(std::lround(
        std::clamp(status.volume, 0.0F, 1.0F) * 100.0F));

    auto session = settings_.Session();
    if (window_ && window_->WindowHandle()) {
        RECT rectangle{};
        if (GetWindowRect(window_->WindowHandle(), &rectangle)) {
            const RECT source = miniPlayer_ && normalWindowRect_.right > normalWindowRect_.left
                                    ? normalWindowRect_ : rectangle;
            session.window = {source.left, source.top, source.right - source.left,
                              source.bottom - source.top};
        }
    }
    session.miniMode = miniPlayer_;
    session.selectedPlaylist = std::to_string(selectedPlaylist_);
    session.selectedTrack = selectedTrack_ == 0 ? std::string{} : std::to_string(selectedTrack_);
    session.positionMilliseconds = static_cast<std::uint64_t>(std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::milliseconds>(status.position).count()));
    session.shuffle = queue_.Shuffle();
    session.repeat = ToConfigRepeat(queue_.Repeat());
    session.moduleLayout = moduleLayout_;

    std::string ignored;
    (void)settings_.SetSettings(std::move(applicationSettings), &ignored);
    (void)settings_.SetSession(std::move(session), &ignored);
    (void)settings_.Save(&ignored);
}

void App::ToggleMiniPlayer() {
    if (!window_ || !window_->WindowHandle()) return;
    HWND handle = window_->WindowHandle();
    if (!miniPlayer_) {
        GetWindowRect(handle, &normalWindowRect_);
        miniPlayer_ = true;
        SetWindowPos(handle, nullptr, 0, 0, 520, 210,
                     SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
    } else {
        miniPlayer_ = false;
        if (normalWindowRect_.right > normalWindowRect_.left) {
            SetWindowPos(handle, nullptr, normalWindowRect_.left, normalWindowRect_.top,
                         normalWindowRect_.right - normalWindowRect_.left,
                         normalWindowRect_.bottom - normalWindowRect_.top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
        }
    }
}

playlist::RepeatMode App::ToQueueRepeat(config::RepeatMode mode) noexcept {
    switch (mode) {
    case config::RepeatMode::All: return playlist::RepeatMode::All;
    case config::RepeatMode::One: return playlist::RepeatMode::One;
    default: return playlist::RepeatMode::Off;
    }
}

config::RepeatMode App::ToConfigRepeat(playlist::RepeatMode mode) noexcept {
    switch (mode) {
    case playlist::RepeatMode::All: return config::RepeatMode::All;
    case playlist::RepeatMode::One: return config::RepeatMode::One;
    default: return config::RepeatMode::Off;
    }
}

} // namespace rivan
