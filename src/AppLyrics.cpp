#include "App.h"

namespace rivan {

void App::OnLyricsServiceUpdated() {
    // Lyrics can arrive mid-track. Presence dedupe sends only a changed active verse.
    UpdateDiscordPresence();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetLyricsCacheEnabled(bool enabled) {
    auto settings = settings_.Settings();
    if (settings.lyricsCacheEnabled == enabled) return;
    settings.lyricsCacheEnabled = enabled;
    std::string error;
    if (!settings_.SetSettings(settings, &error)) return;
    (void)settings_.SaveSettings(&error);
    lyrics_.SetCacheEnabled(enabled);
    ++revision_;
    if (window_) window_->Refresh();
}

} // namespace rivan
