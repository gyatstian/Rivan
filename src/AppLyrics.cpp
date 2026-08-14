#include "App.h"

namespace rivan {

void App::OnLyricsServiceUpdated() {
    // Lyrics can arrive mid-track; republish presence so the artwork tooltip shows
    // the current synced verse as soon as timed lyrics are available (deduped inside;
    // no-op when presence is disabled or nothing is playing).
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
