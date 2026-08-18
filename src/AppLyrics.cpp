#include "App.h"

#include "core/AppPaths.h"
#include "core/IniDocument.h"
#include "core/IniValueCodec.h"
#include "core/Text.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <shellapi.h>

#include <charconv>
#include <filesystem>
#include <system_error>

namespace rivan {
namespace {

std::filesystem::path DisabledLyricsFile() {
    return core::AppPaths::LocalDataRoot() / L"lyrics-disabled.ini";
}

} // namespace

void App::OnLyricsServiceUpdated() {
    // Lyrics can arrive mid-track. Presence dedupe sends only a changed active verse.
    UpdateDiscordPresence();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::SetLyricsCacheEnabled(bool enabled) {
    if (!ApplySettingsChange([enabled](config::AppSettings& settings) {
            if (settings.lyricsCacheEnabled == enabled) return false;
            settings.lyricsCacheEnabled = enabled;
            return true;
        })) return;
    lyrics_.SetCacheEnabled(enabled);
}

void App::SetLyricsOnlineEnabled(bool enabled) {
    if (!ApplySettingsChange([enabled](config::AppSettings& settings) {
            if (settings.lyricsOnlineEnabled == enabled) return false;
            settings.lyricsOnlineEnabled = enabled;
            return true;
        })) return;
    lyrics_.SetOnlineEnabled(enabled);
    // Local-only mode must stop showing previously fetched online lyrics; online mode
    // may now fetch a track that had no local lyrics. Re-resolve the active track.
    RefreshActiveLyrics();
}

void App::SetLyricsFakeTimestampsEnabled(bool enabled) {
    if (!ApplySettingsChange([enabled](config::AppSettings& settings) {
            if (settings.lyricsFakeTimestampsEnabled == enabled) return false;
            settings.lyricsFakeTimestampsEnabled = enabled;
            return true;
        })) return;
    lyrics_.SetFakeTimestampsEnabled(enabled);
    RefreshActiveLyrics();
}

void App::RefreshActiveLyrics() {
    if (!activeTrack_) return;
    const auto metadata = LyricsMetadata(*activeTrack_);
    lyrics_.Request(activeTrack_->id, metadata.first, metadata.second,
                    activeTrack_->album, activeTrack_->durationSeconds,
                    activeTrack_->filePath);
}

void App::SetTrackLyricsDisabled(std::wstring filePath, bool disabled) {
    if (filePath.empty()) return;
    const auto key = lyrics::LyricsService::NormalizedTrackPath(filePath);
    if (disabled) disabledLyricsSongs_.insert(key);
    else disabledLyricsSongs_.erase(key);
    lyrics_.SetDisabledSongs(disabledLyricsSongs_);
    SaveDisabledLyrics();
    // The active track may be the one just toggled; re-resolve so the module reacts now.
    RefreshActiveLyrics();
    ++revision_;
    if (window_) window_->Refresh();
}

void App::LoadDisabledLyrics() {
    const auto file = DisabledLyricsFile();
    std::error_code ec;
    if (!std::filesystem::exists(file, ec) || ec) return;
    auto document = core::IniDocument::Load(file, nullptr);
    if (!document || !document->HasMetaFormat("1")) return;

    std::size_t count = 0;
    if (const auto stored = document->Get("meta", "count")) {
        const auto text = std::string(*stored);
        std::size_t value = 0;
        const auto [end, err] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (err == std::errc{} && end == text.data() + text.size()) count = value;
    }
    if (count > 100000) count = 100000;

    std::unordered_set<std::wstring> songs;
    for (std::size_t i = 0; i < count; ++i) {
        const auto encoded = document->Get("d" + std::to_string(i), "path");
        if (!encoded) continue;
        const auto pathUtf8 = core::DecodeIniValue(*encoded, false);
        if (!pathUtf8) continue;
        songs.insert(lyrics::LyricsService::NormalizedTrackPath(core::Utf8ToWide(*pathUtf8)));
    }
    disabledLyricsSongs_ = std::move(songs);
}

void App::SaveDisabledLyrics() const {
    core::IniDocument document;
    document.Set("meta", "format", "1");
    document.Set("meta", "count", std::to_string(disabledLyricsSongs_.size()));
    std::size_t i = 0;
    for (const auto& key : disabledLyricsSongs_) {
        document.Set("d" + std::to_string(i), "path", core::EncodeIniValue(core::WideToUtf8(key)));
        ++i;
    }
    (void)document.SaveAtomic(DisabledLyricsFile(), nullptr);
}

void App::SetLyricsAlignment(config::LyricsTextAlignment alignment) {
    ApplySettingsChange([alignment](config::AppSettings& settings) {
        if (settings.lyricsAlignment == alignment) return false;
        settings.lyricsAlignment = alignment;
        return true;
    });
}

void App::AddYourOwnLyrics() {
    if (!activeTrack_) return;
    const auto metadata = LyricsMetadata(*activeTrack_);
    AddLyricsToTrack(metadata.first, metadata.second, activeTrack_->album,
                     activeTrack_->durationSeconds, activeTrack_->filePath.wstring());
}

void App::AddLyricsToTrack(std::wstring title, std::wstring artist, std::wstring album,
                           double durationSeconds, std::wstring filePath) {
    if (filePath.empty()) return;
    // Authoring lyrics implicitly re-enables the song so the fresh file actually loads.
    const auto key = lyrics::LyricsService::NormalizedTrackPath(filePath);
    if (disabledLyricsSongs_.erase(key) != 0) {
        lyrics_.SetDisabledSongs(disabledLyricsSongs_);
        SaveDisabledLyrics();
    }
    const auto path = lyrics_.CreateUserLyricsFile(
        0, std::move(title), std::move(artist), std::move(album), durationSeconds,
        std::filesystem::path(std::move(filePath)));
    if (path.empty()) return;
    // Open the freshly created file in the user's default text editor (e.g. Notepad).
    (void)ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace rivan
