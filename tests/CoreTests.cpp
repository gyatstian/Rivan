// CoreTests.cpp
// Dependency-free behavioral checks for Rivan's deterministic library, queue, and FFT core.
// The test executable returns nonzero at the first failed invariant.
#include "../src/audio/AudioAnalysisBuffer.h"
#include "../src/config/SettingsManager.h"
#include "../src/core/IniDocument.h"
#include "../src/core/IniValueCodec.h"
#include "../src/library/LibraryScanner.h"
#include "../src/playlist/PlaybackQueue.h"
#include "../src/playlist/PlaylistManager.h"
#include "../src/skin/Skin.h"
#include "../src/visualization/Visualization.h"
#include "../src/lyrics/LyricsService.h"
#include "../src/ui/TrackCoverCache.h"
#include "../src/ui/SongRowLayoutGeometry.h"
#include "../src/ui/layout/ModuleLayout.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

class TemporaryLibrary final {
public:
    TemporaryLibrary() {
        root_ = std::filesystem::temp_directory_path() /
                (L"RivanTests-" + std::to_wstring(GetCurrentProcessId()));
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(root_ / L"Rock", ec);
        std::filesystem::create_directories(root_ / L"Game" / L"OST", ec);
        Touch(root_ / L"Rock" / L"one.MP3");
        Touch(root_ / L"Rock" / L"two.flac");
        WriteMinimalOpus(root_ / L"Rock" / L"four.opus", 10);
        Touch(root_ / L"Game" / L"OST" / L"three.wav");
        Touch(root_ / L"ignored.txt");
    }

    ~TemporaryLibrary() {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    [[nodiscard]] const std::filesystem::path& Root() const noexcept { return root_; }

private:
    static void Touch(const std::filesystem::path& path) {
        std::ofstream stream(path, std::ios::binary);
        stream << 'x';
    }

    static void WriteLittleEndian64(std::vector<unsigned char>& bytes, std::uint64_t value) {
        for (int index = 0; index < 8; ++index) {
            bytes.push_back(static_cast<unsigned char>((value >> (index * 8)) & 0xffu));
        }
    }

    static void AppendOggPage(std::vector<unsigned char>& bytes,
                              unsigned char headerType,
                              std::uint64_t granulePosition,
                              std::uint32_t sequence,
                              std::span<const unsigned char> payload) {
        bytes.insert(bytes.end(), {'O', 'g', 'g', 'S'});
        bytes.push_back(0);
        bytes.push_back(headerType);
        WriteLittleEndian64(bytes, granulePosition);
        bytes.insert(bytes.end(), {1, 0, 0, 0});
        bytes.push_back(static_cast<unsigned char>(sequence & 0xffu));
        bytes.push_back(static_cast<unsigned char>((sequence >> 8) & 0xffu));
        bytes.push_back(static_cast<unsigned char>((sequence >> 16) & 0xffu));
        bytes.push_back(static_cast<unsigned char>((sequence >> 24) & 0xffu));
        bytes.insert(bytes.end(), {0, 0, 0, 0});
        bytes.push_back(payload.empty() ? 0 : 1);
        if (!payload.empty()) bytes.push_back(static_cast<unsigned char>(payload.size()));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    }

    static void WriteMinimalOpus(const std::filesystem::path& path, const int seconds) {
        constexpr std::uint16_t preSkip = 312;
        std::vector<unsigned char> opusHead{'O', 'p', 'u', 's', 'H', 'e', 'a', 'd',
                                            1, 2,
                                            static_cast<unsigned char>(preSkip & 0xffu),
                                            static_cast<unsigned char>((preSkip >> 8) & 0xffu),
                                            0x80, 0xbb, 0, 0,
                                            0, 0,
                                            0};
        std::vector<unsigned char> file;
        AppendOggPage(file, 2, 0, 0, opusHead);
        AppendOggPage(file, 4, 48000ull * static_cast<std::uint64_t>(seconds) + preSkip,
                      1, {});

        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(file.data()),
                     static_cast<std::streamsize>(file.size()));
    }

    std::filesystem::path root_;
};

rivan::library::LibraryScanResult MakeScan(
    std::vector<rivan::library::Track> tracks,
    std::vector<rivan::playlist::Playlist> playlists) {
    rivan::library::LibraryScanResult scan;
    scan.tracks = std::move(tracks);
    scan.playlists = std::move(playlists);
    return scan;
}

void TestLibraryAndQueue() {
    TemporaryLibrary files;
    rivan::library::LibraryScanner scanner;
    const auto scan = scanner.Scan(files.Root());
    Check(scan.tracks.size() == 4, "scanner accepts supported audio case-insensitively");
    // All Music + every directory (root, Rock, Game, Game/OST) as a folder playlist.
    Check(scan.playlists.size() == 5, "scanner creates All Music and a playlist per folder");
    Check(scan.playlists.front().trackIds.size() == 4, "All Music contains recursive union");
    // Directory playlists hold only their direct files; hierarchy links point upward.
    const auto findFolder = [&](std::wstring_view name) -> const rivan::playlist::Playlist* {
        for (const auto& playlist : scan.playlists) {
            if (playlist.kind == rivan::playlist::PlaylistKind::Directory &&
                playlist.directory.filename() == name) {
                return &playlist;
            }
        }
        return nullptr;
    };
    const auto* rock = findFolder(L"Rock");
    const auto* game = findFolder(L"Game");
    const auto* ost = findFolder(L"OST");
    Check(rock != nullptr && rock->trackIds.size() == 3, "Rock has its three direct tracks");
    Check(game != nullptr && game->trackIds.empty(), "Game folder is an empty parent");
    Check(ost != nullptr && ost->trackIds.size() == 1 && ost->parentId == game->id,
          "OST nests under Game with one direct track");
    const auto opus = std::find_if(scan.tracks.begin(), scan.tracks.end(), [](const auto& track) {
        return track.filePath.extension() == L".opus";
    });
    Check(opus != scan.tracks.end() && std::abs(opus->durationSeconds - 10.0) < 0.01,
          "scanner reads Ogg Opus duration without Media Foundation metadata");

    // Directory playlists, including folders shown through a parent-folder view, keep
    // their own direct order and accept the same reorder operation as user playlists.
    if (rock != nullptr) {
        rivan::playlist::PlaylistManager manager;
        manager.ApplyScan(scan);
        const auto before = manager.ResolveTracks(rock->id);
        Check(before.size() == 3, "directory reorder test resolves direct folder tracks");
        Check(manager.MoveTracks(rock->id, {0}, before.size()),
              "directory playlist reorders a track from a folder view");
        const auto after = manager.ResolveTracks(rock->id);
        Check(after.size() == before.size() && !after.empty() &&
                  after.back().id == before.front().id,
              "directory reorder moves the selected track within its owning folder");
    }

    rivan::playlist::PlaybackQueue queue(7);
    queue.SetTracks(scan.tracks, std::nullopt);
    queue.SetShuffle(true);
    queue.SetRepeat(rivan::playlist::RepeatMode::All);
    Check(queue.Start().track != nullptr, "queue starts with a playable track");
    Check(queue.Next().track != nullptr, "queue advances while shuffled");
    Check(queue.Previous().track != nullptr, "queue retains previous history");
}

void TestUserPlaylistEditing() {
    using namespace rivan;
    playlist::Playlist list;
    list.kind = playlist::PlaylistKind::User;
    // Duplicate-allowing append + index removal / duplication / reorder.
    list.AppendTrack(10);
    list.AppendTrack(20);
    list.AppendTrack(10);  // same id twice is allowed for user playlists
    Check(list.trackIds.size() == 3, "AppendTrack allows duplicate ids");
    Check(list.DuplicateAt(1), "DuplicateAt inserts a copy after the entry");
    // 10,20,20,10
    Check(list.trackIds.size() == 4 && list.trackIds[1] == 20 && list.trackIds[2] == 20,
          "DuplicateAt copies the entry in place");
    Check(list.RemoveAt(2), "RemoveAt drops a single positional entry");
    // 10,20,10
    Check(list.trackIds.size() == 3 && list.trackIds[1] == 20,
          "RemoveAt keeps the other duplicate");
    // Move the first entry to the end.
    Check(list.MoveRange({0}, 3), "MoveRange moves an entry to the tail");
    Check(list.trackIds[0] == 20 && list.trackIds[2] == 10,
          "MoveRange preserves relative order of the rest");

    // PlaylistManager: user playlist survives a rescan and external tracks resolve.
    playlist::PlaylistManager manager;
    library::Track a = library::Track::FromFile(std::filesystem::path(L"C:/music/a.mp3"));
    library::Track b = library::Track::FromFile(std::filesystem::path(L"C:/music/b.mp3"));
    const auto id = manager.CreatePlaylist(L"Mine");
    Check(manager.AddExternalTrack(id, a), "AddExternalTrack appends to a user playlist");
    Check(manager.AddExternalTrack(id, b), "AddExternalTrack appends a second track");
    Check(manager.AddExternalTrack(id, a), "AddExternalTrack allows a duplicate entry");
    Check(manager.ResolveTracks(id).size() == 3,
          "external tracks resolve even without a scan");
    // Empty rescan must not wipe the user playlist or its external tracks.
    manager.ApplyScan(MakeScan({}, {}));
    Check(manager.FindPlaylist(id) != nullptr, "user playlist survives a rescan");
    Check(manager.ResolveTracks(id).size() == 3,
          "external track entries survive a rescan");
    Check(manager.MoveTracks(id, {2}, 0), "MoveTracks reorders by index");
    Check(manager.RemoveTracksAt(id, std::vector<std::size_t>{0}),
          "RemoveTracksAt removes a positional entry");
    Check(manager.ResolveTracks(id).size() == 2, "removal leaves the remaining entries");
    // Second user playlist + reorder among user playlists.
    const auto id2 = manager.CreatePlaylist(L"Other");
    Check(manager.UserPlaylists().size() == 2, "two user playlists tracked");
    Check(manager.MoveUserPlaylist(id2, id), "MoveUserPlaylist reorders the tree order");
    Check(manager.UserPlaylists().front()->id == id2,
          "moved user playlist lands before the target");

    playlist::Playlist folder;
    folder.id = 42;
    folder.name = L"Folder";
    folder.kind = playlist::PlaylistKind::Directory;
    manager.ApplyScan(MakeScan({}, {folder}));
    Check(manager.AddExternalTrack(folder.id, a),
          "AddExternalTrack imports into a generated folder playlist");
    Check(manager.ResolveTracks(folder.id).size() == 1,
          "generated folder resolves its imported track");
    manager.ApplyScan(MakeScan({}, {folder}));
    Check(manager.ResolveTracks(folder.id).size() == 1,
          "generated folder imports survive a rescan");
}

void TestSpectrum() {
    constexpr std::size_t fftSize = 1024;
    constexpr std::size_t outputSize = fftSize / 2;
    constexpr float cycles = 32.0F;
    std::vector<float> samples(fftSize);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        samples[index] = std::sin(2.0F * std::numbers::pi_v<float> * cycles *
                                  static_cast<float>(index) / static_cast<float>(fftSize));
    }
    std::vector<float> spectrum(outputSize);
    rivan::visualization::FloatSnapshotAnalyzer::Radix2Spectrum(samples, spectrum);
    const auto peak = static_cast<std::size_t>(std::distance(
        spectrum.begin(), std::max_element(spectrum.begin(), spectrum.end())));
    Check(std::abs(static_cast<int>(peak) - static_cast<int>(cycles)) <= 1,
          "FFT peak maps to the generated tone bin");

    rivan::visualization::FloatSnapshotAnalyzer analyzer(fftSize);
    analyzer.Submit(samples, 1, 48000);
    rivan::visualization::VisualizationSnapshot snap;
    analyzer.CopySnapshot(snap);
    Check(snap.waveform.size() == fftSize && snap.spectrum.size() == outputSize,
          "analyzer snapshot sizes match FFT configuration");
    Check(snap.sequence == 1, "analyzer advances sequence on Submit");
}

void TestAnalysisBufferReuse() {
    rivan::audio::AudioAnalysisBuffer buffer;
    buffer.Configure(48000, 2, 256);
    std::vector<float> block(128 * 2, 0.25F);
    buffer.Push(block);
    Check(buffer.Generation() != 0, "analysis buffer generation advances on push");
    Check(buffer.Latest(256).samples.size() == 128 * 2,
          "analysis buffer stores pushed frames");

    rivan::audio::AudioAnalysisSnapshot first;
    buffer.LatestInto(first, 64);
    Check(first.samples.size() == 64 * 2, "LatestInto returns requested frames");
    Check(first.channels == 2 && first.sampleRate == 48000, "LatestInto preserves format");
    const auto capacityAfterFirst = first.samples.capacity();

    buffer.Push(block);
    buffer.LatestInto(first, 64);
    Check(first.samples.size() == 64 * 2, "LatestInto reuses destination vector");
    Check(first.samples.capacity() >= capacityAfterFirst,
          "LatestInto does not shrink reserved capacity");
}

void TestSkinCustomizationRoundTrip() {
    auto skin = rivan::skin::Skin::BuiltInDarkPurple();
    Check(skin.appearance.transparentButtons,
          "the default skin uses text-only controls");
    Check(!skin.appearance.showTitleBars,
          "the default skin draws titles directly on the panel background");
    Check(!skin.typography.fontFamily.empty() && skin.typography.baseSize >= 10.0F,
          "the default skin provides readable typography");

    skin.id = "test-studio-skin";
    skin.name = "Studio test";
    skin.builtIn = false;
    skin.typography.fontFamily = "Segoe UI";
    skin.typography.customFontFile = std::filesystem::path(L"fonts\\test.ttf");
    skin.typography.baseSize = 14.0F;
    skin.appearance.backgroundImage = std::filesystem::path(L"images\\background.png");
    skin.appearance.backgroundImageOpacity = 0.42F;
    skin.appearance.panelOpacity = 0.88F;
    skin.appearance.screenOpacity = 0.54F;
    skin.colors.hoverBackground = {24, 48, 72, 255};
    skin.colors.screenBackground = {6, 12, 18, 255};
    skin.appearance.centeredTitles = true;
    skin.appearance.decorAbovePanels = true;
    skin.shapes.push_back({
        .kind = rivan::skin::ShapeKind::Ellipse,
        .x = 0.12F,
        .y = 0.18F,
        .width = 0.35F,
        .height = 0.28F,
        .filled = false,
        .color = {12, 34, 56, 190},
        .strokeWidth = 3.0F,
        .rotation = 27.0F,
        .priority = 7,
    });
    skin.images.push_back({
        .file = std::filesystem::path(L"images\\overlay.png"),
        .x = 0.20F,
        .y = 0.30F,
        .width = 0.40F,
        .height = 0.25F,
        .opacity = 0.65F,
        .rotation = 35.0F,
        .flipHorizontal = true,
        .flipVertical = false,
        .priority = 3,
        .tint = {200, 80, 255, 140},
    });

    const auto root = std::filesystem::temp_directory_path() /
                      (L"RivanSkinTests-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    const auto manifest = root / L"skin.ini";
    std::string error;
    Check(skin.SaveManifestAtomic(manifest, &error),
          "skin studio settings save to a manifest");
    const auto loaded = rivan::skin::Skin::LoadManifest(manifest, &error);
    Check(loaded.has_value(), "saved skin studio manifest loads again");
    if (loaded) {
        Check(loaded->colors == skin.colors,
              "semantic UI colors round-trip");
        Check(loaded->typography.fontFamily == skin.typography.fontFamily &&
                  loaded->typography.customFontFile == skin.typography.customFontFile &&
                  std::abs(loaded->typography.baseSize - skin.typography.baseSize) < 0.01F,
              "font family, custom font path, and size round-trip");
        Check(loaded->appearance == skin.appearance,
              "background image, opacity, panel layering, buttons, and title options round-trip");
        Check(loaded->shapes == skin.shapes,
              "filled and outlined decorative shapes round-trip");
        Check(loaded->images == skin.images,
              "positioned decorative images round-trip");
    }
    std::filesystem::remove_all(root, ec);
}

void TestSkinRejectsUnsafeAssets() {
    auto skin = rivan::skin::Skin::BuiltInDarkPurple();
    skin.id = "unsafe-assets";
    skin.appearance.backgroundImage = std::filesystem::path(L"..\\outside.png");
    std::string error;
    Check(!rivan::skin::Skin::Validate(skin, &error),
          "skin assets cannot escape their skin directory");
}

void TestUserPlaylistScanFlow() {
    TemporaryLibrary files;
    rivan::library::LibraryScanner scanner;
    const auto scan = scanner.Scan(files.Root());

    rivan::playlist::PlaylistManager manager;
    manager.ApplyScan(scan);

    // Build a user playlist and add known library tracks to it.
    const auto id = manager.CreatePlaylist(L"Mix");
    std::vector<rivan::library::TrackId> ids;
    for (const auto& track : scan.tracks) ids.push_back(track.id);
    Check(ids.size() == 4, "test library yields four tracks");
    for (const auto trackId : ids) {
        const auto* track = manager.FindTrack(trackId);
        Check(track != nullptr, "library track resolves before adding");
        if (track) Check(manager.AddExternalTrack(id, *track), "AddExternalTrack appends to user playlist");
    }
    Check(manager.ResolveTracks(id).size() == 4, "user playlist holds four entries");

    // Duplicate the first entry: same id now appears twice, adjacent.
    Check(manager.DuplicateTrackAt(id, 0), "DuplicateTrackAt inserts a copy");
    {
        const auto resolved = manager.ResolveTracks(id);
        Check(resolved.size() == 5, "duplicate grows the entry count");
        Check(resolved[0].id == resolved[1].id, "duplicate copy sits right after the source");
    }

    // Move the block [0,1] to the end; remaining order preserved.
    Check(manager.MoveTracks(id, {0, 1}, 5), "MoveTracks relocates a contiguous block");
    {
        const auto resolved = manager.ResolveTracks(id);
        Check(resolved.size() == 5, "move keeps the entry count");
        Check(resolved[3].id == resolved[4].id, "moved duplicate pair lands at the end together");
    }

    // Remove two entries by position.
    Check(manager.RemoveTracksAt(id, std::vector<std::size_t>{0, 3}), "RemoveTracksAt drops entries by index");
    Check(manager.ResolveTracks(id).size() == 3, "remove shrinks the entry count");

    // External track survives a rescan even though it is not a scan playlist member.
    const auto externalTrack = rivan::library::Track::FromFile(files.Root() / L"Rock" / L"one.MP3");
    const auto id2 = manager.CreatePlaylist(L"Keep");
    Check(manager.AddExternalTrack(id2, externalTrack), "add a track to a second user playlist");
    manager.ApplyScan(scan);  // rescan
    const auto* still = manager.FindPlaylist(id2);
    Check(still != nullptr && still->trackIds.size() == 1,
          "user playlist entries survive a library rescan");

    // Reorder user playlists: move the second in front of the first.
    Check(manager.MoveUserPlaylist(id2, id), "MoveUserPlaylist reorders user playlists");
    const auto users = manager.UserPlaylists();
    Check(users.size() == 2 && users.front()->id == id2,
          "reordered user playlist comes first");
}

void TestSongRowLayoutSettingRoundTrip() {
    const auto root = std::filesystem::temp_directory_path() /
                      (L"RivanSettingsTests-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    const auto settingsFile = root / L"settings.ini";
    const auto sessionFile = root / L"session.ini";

    rivan::config::SettingsManager writer(settingsFile, sessionFile);
    auto settings = rivan::config::AppSettings::Defaults();
    settings.musicRoot = root / L"Music";
    std::filesystem::create_directories(settings.musicRoot, ec);
    settings.songRowLayout.rowHeight = 64.0F;
    settings.songRowLayout.Field(rivan::ui::SongRowField::Title).x = 0.20F;
    settings.songRowLayout.Field(rivan::ui::SongRowField::Title).snap =
        rivan::ui::SongRowSnap{rivan::ui::SongRowField::Cover,
                               rivan::ui::SongRowSnapSide::Right, 3};
    settings.songRowLayout.Field(rivan::ui::SongRowField::Title).fluid = false;
    settings.songRowLayout.Field(rivan::ui::SongRowField::Title).fontSizeDelta = 3;
    settings.songRowLayout.Field(rivan::ui::SongRowField::Title).fontWeight =
        rivan::ui::SongRowFontWeight::Bold;
    settings.songRowLayout.Field(rivan::ui::SongRowField::Artist).textColor =
        rivan::ui::SongRowTextColor::Secondary;
    settings.filePreviewEnabled = false;
    settings.previewFitWindow = true;
    settings.startAtStartup = true;
    settings.exitToTray = true;
    settings.discordShowGithubButton = true;
    settings.youtubeGrabberHotkeyModifiers = 0x0001u | 0x0002u;
    settings.youtubeGrabberHotkeyVirtualKey = VK_F8;
    settings.windowResizeBehavior = rivan::ui::WindowResizeBehavior::GrowTrailingModule;
    settings.moduleResizeBehavior = rivan::ui::ModuleResizeBehavior::Overlap;
    std::string error;
    Check(writer.SetSettings(settings, &error), "file preview setting accepts false");
    Check(writer.SaveSettings(&error), "file preview setting saves");

    std::string persistedSettings;
    {
        std::ifstream savedSettings(settingsFile, std::ios::binary);
        persistedSettings.assign(std::istreambuf_iterator<char>(savedSettings),
                                 std::istreambuf_iterator<char>());
    }
    Check(persistedSettings.find("width_fraction") == std::string::npos,
          "song row width is not persisted");
    constexpr std::string_view songRowSection = "[song_row_layout]\n";
    const auto songRowSectionOffset = persistedSettings.find(songRowSection);
    Check(songRowSectionOffset != std::string::npos,
          "song row layout section is persisted");
    if (songRowSectionOffset != std::string::npos) {
        persistedSettings.insert(songRowSectionOffset + songRowSection.size(),
                                 "width_fraction=0.30\n");
        std::ofstream legacySettings(settingsFile, std::ios::binary | std::ios::trunc);
        legacySettings << persistedSettings;
    }

    rivan::config::SettingsManager reader(settingsFile, sessionFile);
    Check(reader.LoadSettings(&error), "file preview setting reloads");
    Check(reader.Settings().filePreviewEnabled == false,
           "file preview disabled state survives settings round-trip");
    const auto& songRowLayout = reader.Settings().songRowLayout;
    Check(std::abs(songRowLayout.rowHeight - 64.0F) < 0.01F &&
               std::abs(songRowLayout.Field(rivan::ui::SongRowField::Title).x - 0.20F) < 0.01F &&
               songRowLayout.Field(rivan::ui::SongRowField::Title).snap.has_value() &&
               songRowLayout.Field(rivan::ui::SongRowField::Title).snap->target ==
                   rivan::ui::SongRowField::Cover &&
               songRowLayout.Field(rivan::ui::SongRowField::Title).snap->gapPixels == 3 &&
               !songRowLayout.Field(rivan::ui::SongRowField::Title).fluid &&
              songRowLayout.Field(rivan::ui::SongRowField::Title).fontSizeDelta == 3 &&
              songRowLayout.Field(rivan::ui::SongRowField::Title).fontWeight ==
                  rivan::ui::SongRowFontWeight::Bold &&
              songRowLayout.Field(rivan::ui::SongRowField::Artist).textColor ==
                  rivan::ui::SongRowTextColor::Secondary,
           "song row layout survives settings round-trip");
    Check(reader.SaveSettings(&error), "legacy song row width setting resaves");
    std::string resaved;
    {
        std::ifstream resavedSettings(settingsFile, std::ios::binary);
        resaved.assign(std::istreambuf_iterator<char>(resavedSettings),
                       std::istreambuf_iterator<char>());
    }
    Check(resaved.find("width_fraction") == std::string::npos,
          "legacy song row width setting is ignored on load and removed on save");
    Check(reader.Settings().startAtStartup,
          "start at startup survives settings round-trip");
    Check(reader.Settings().previewFitWindow,
           "preview fit-to-window setting survives settings round-trip");
    Check(reader.Settings().exitToTray,
          "exit to tray survives settings round-trip");
    Check(reader.Settings().discordShowGithubButton,
           "Discord GitHub button setting survives settings round-trip");
    Check(reader.Settings().youtubeGrabberHotkeyModifiers == (0x0001u | 0x0002u) &&
              reader.Settings().youtubeGrabberHotkeyVirtualKey == VK_F8,
          "YouTube link grabber hotkey survives settings round-trip");
    Check(reader.Settings().windowResizeBehavior == rivan::ui::WindowResizeBehavior::GrowTrailingModule,
           "window resize behavior survives settings round-trip");
    Check(reader.Settings().moduleResizeBehavior == rivan::ui::ModuleResizeBehavior::Overlap,
           "module resize collision behavior survives settings round-trip");
    settings.lyricsCacheEnabled = true;
    Check(writer.SetSettings(settings, &error) && writer.SaveSettings(&error) &&
              reader.LoadSettings(&error) && reader.Settings().lyricsCacheEnabled,
          "lyrics cache setting survives settings round-trip");

    settings.filePreviewEnabled = true;
    Check(writer.SetSettings(settings, &error) && writer.SaveSettings(&error),
           "file preview setting accepts true");
    Check(reader.LoadSettings(&error) && reader.Settings().filePreviewEnabled,
           "file preview setting enables after round-trip");
    auto invalidLayout = reader.Settings();
    invalidLayout.songRowLayout.Field(rivan::ui::SongRowField::Title).x = 0.95F;
    invalidLayout.songRowLayout.Field(rivan::ui::SongRowField::Title).width = 0.10F;
    Check(!reader.SetSettings(invalidLayout, &error),
           "song row layout rejects fields outside the row canvas");
    auto cyclicLayout = reader.Settings();
    cyclicLayout.songRowLayout.Field(rivan::ui::SongRowField::Title).snap =
        rivan::ui::SongRowSnap{rivan::ui::SongRowField::Artist,
                               rivan::ui::SongRowSnapSide::Right, 1};
    cyclicLayout.songRowLayout.Field(rivan::ui::SongRowField::Artist).snap =
        rivan::ui::SongRowSnap{rivan::ui::SongRowField::Title,
                               rivan::ui::SongRowSnapSide::Right, 1};
    Check(!reader.SetSettings(cyclicLayout, &error),
          "song row layout rejects cyclic field snaps");
    std::filesystem::remove_all(root, ec);
}

void TestSongRowLayoutDefaults() {
    const auto layout = rivan::ui::SongRowLayout::Defaults();
    const auto& title = layout.Field(rivan::ui::SongRowField::Title);
    const auto& artist = layout.Field(rivan::ui::SongRowField::Artist);

    Check(std::abs(title.y - 0.03F) < 0.001F &&
              std::abs(title.height - 0.53F) < 0.001F &&
              std::abs(artist.y - 0.58F) < 0.001F &&
              std::abs(artist.height - 0.35F) < 0.001F &&
              title.y + title.height <= artist.y,
           "default song name gains lower text room without overlapping artist");
    Check(layout.Field(rivan::ui::SongRowField::Number).fluid &&
              layout.Field(rivan::ui::SongRowField::Title).fluid &&
              layout.Field(rivan::ui::SongRowField::Duration).fluid &&
              layout.Field(rivan::ui::SongRowField::Artist).fluid &&
              layout.Field(rivan::ui::SongRowField::Bitrate).fluid &&
              !layout.Field(rivan::ui::SongRowField::Cover).fluid,
          "text song-row fields are fluid while cover keeps explicit dimensions");

    constexpr float canvasLeft = 10.0F;
    constexpr float canvasWidth = 400.0F;
    Check(std::abs(rivan::ui::SongRowSnappedFieldX(
                       100.0F, 140.0F, canvasLeft, canvasWidth, 0.10F, 1.0F,
                       rivan::ui::SongRowSnapSide::Right) - 0.325F) < 0.001F,
          "song row right snap leaves the default one-pixel gap");
    Check(std::abs(rivan::ui::SongRowSnappedFieldX(
                       100.0F, 140.0F, canvasLeft, canvasWidth, 0.10F, 0.0F,
                       rivan::ui::SongRowSnapSide::Left) - 0.1275F) < 0.001F,
          "song row left snap abuts the target edge");
    Check(std::abs(rivan::ui::SongRowSnappedFieldX(
                       100.0F, 140.0F, canvasLeft, canvasWidth, 0.10F, -2.0F,
                       rivan::ui::SongRowSnapSide::Right) - 0.3175F) < 0.001F,
          "song row snap supports negative gaps for intentional overlap");
    Check(rivan::ui::SongRowSnapHoverSide(100.0F, 50.0F, 100.0F, 140.0F, 20.0F, 80.0F) ==
              rivan::ui::SongRowSnapSide::Left &&
              rivan::ui::SongRowSnapHoverSide(140.0F, 50.0F, 100.0F, 140.0F, 20.0F, 80.0F) ==
              rivan::ui::SongRowSnapSide::Right,
          "song row snap hover identifies left and right target edges");
    Check(!rivan::ui::SongRowSnapHoverSide(100.0F, 10.0F, 100.0F, 140.0F, 20.0F, 80.0F) &&
              !rivan::ui::SongRowSnapHoverSide(109.0F, 50.0F, 100.0F, 140.0F, 20.0F, 80.0F),
          "song row snap hover ignores outside vertical span and distant edges");
    Check(rivan::ui::SongRowSnapHoverSide(108.0F, 50.0F, 100.0F, 140.0F, 20.0F, 80.0F) ==
              rivan::ui::SongRowSnapSide::Left &&
              !rivan::ui::SongRowSnapHoverSide(108.1F, 50.0F, 100.0F, 140.0F, 20.0F, 80.0F),
           "song row snap hover accepts the edge threshold and rejects beyond it");

    Check(std::abs(rivan::ui::SongRowFluidFieldWidth(8.0F, 400.0F) - 0.030F) < 0.001F,
          "fluid song fields reserve inset and rasterization room around measured text");
    Check(std::abs(rivan::ui::SongRowFluidFieldWidth(8.0F, 400.0F, 1.0F, 4.0F) - 0.045F) < 0.001F,
          "fluid song fields reserve caller-supplied italic or outline overhang room");

    auto coverClearance = rivan::ui::SongRowLayout::Defaults();
    std::array<float, rivan::ui::kSongRowFieldCount> coverClearanceWidths{};
    const auto numberIndex = static_cast<std::size_t>(rivan::ui::SongRowField::Number);
    const auto coverIndex = static_cast<std::size_t>(rivan::ui::SongRowField::Cover);
    const auto titleIndex = static_cast<std::size_t>(rivan::ui::SongRowField::Title);
    coverClearanceWidths[numberIndex] = 0.12F;
    coverClearanceWidths[coverIndex] = coverClearance.Field(rivan::ui::SongRowField::Cover).width;
    coverClearanceWidths[titleIndex] = 0.20F;
    coverClearance.Field(rivan::ui::SongRowField::Title).snap =
        rivan::ui::SongRowSnap{rivan::ui::SongRowField::Cover,
                               rivan::ui::SongRowSnapSide::Right, 3};
    const auto coverResolved = rivan::ui::SongRowResolvedFieldXs(
        coverClearance, 10.0F, 400.0F, coverClearanceWidths);
    const float clearedNumberRight = 10.0F +
        (coverResolved[numberIndex] + coverClearanceWidths[numberIndex]) * 400.0F - 1.0F;
    const float clearedCoverLeft = 10.0F + coverResolved[coverIndex] * 400.0F + 1.0F;
    const float clearedCoverRight = 10.0F +
        (coverResolved[coverIndex] + coverClearanceWidths[coverIndex]) * 400.0F - 1.0F;
    const float clearedTitleLeft = 10.0F + coverResolved[titleIndex] * 400.0F + 1.0F;
    Check(std::abs(clearedCoverLeft - clearedNumberRight - 1.0F) < 0.01F &&
              std::abs(clearedTitleLeft - clearedCoverRight - 3.0F) < 0.01F,
          "independent cover clears fluid number and repositions attached fields");

    auto multiSnap = rivan::ui::SongRowLayout::Defaults();
    std::array<float, rivan::ui::kSongRowFieldCount> widths{};
    widths[static_cast<std::size_t>(rivan::ui::SongRowField::Number)] = 0.05F;
    widths[static_cast<std::size_t>(rivan::ui::SongRowField::Cover)] = 0.10F;
    widths[static_cast<std::size_t>(rivan::ui::SongRowField::Title)] = 0.20F;
    widths[static_cast<std::size_t>(rivan::ui::SongRowField::Artist)] = 0.15F;
    multiSnap.Field(rivan::ui::SongRowField::Number).x = 0.10F;
    multiSnap.Field(rivan::ui::SongRowField::Cover).snap =
        rivan::ui::SongRowSnap{rivan::ui::SongRowField::Number,
                               rivan::ui::SongRowSnapSide::Right, 2};
    multiSnap.Field(rivan::ui::SongRowField::Title).snap =
        rivan::ui::SongRowSnap{rivan::ui::SongRowField::Cover,
                               rivan::ui::SongRowSnapSide::Right, 3};
    multiSnap.Field(rivan::ui::SongRowField::Artist).snap =
        rivan::ui::SongRowSnap{rivan::ui::SongRowField::Cover,
                               rivan::ui::SongRowSnapSide::Right, 4};
    const auto resolved = rivan::ui::SongRowResolvedFieldXs(multiSnap, 0.0F, 400.0F, widths);
    const float numberRight = (resolved[0] + widths[0]) * 400.0F - 1.0F;
    const float coverLeft = resolved[3] * 400.0F + 1.0F;
    const float coverRight = (resolved[3] + widths[3]) * 400.0F - 1.0F;
    const float titleLeft = resolved[1] * 400.0F + 1.0F;
    const float artistLeft = resolved[4] * 400.0F + 1.0F;
    Check(std::abs(coverLeft - numberRight - 2.0F) < 0.01F &&
              std::abs(titleLeft - coverRight - 3.0F) < 0.01F &&
              std::abs(artistLeft - coverRight - 4.0F) < 0.01F,
          "multiple song fields retain independent snap gaps to one target and chains");

auto invalid = multiSnap;
    invalid.Field(rivan::ui::SongRowField::Number).snap =
        rivan::ui::SongRowSnap{rivan::ui::SongRowField::Title,
                               rivan::ui::SongRowSnapSide::Left, 1};
    Check(rivan::ui::SongRowHasSnapCycle(invalid),
          "song row snap cycle detection rejects circular attachments");

    // Playing indicator shifts the number right during rendering. Snap followers
    // must track the transient position, not the stored one.
    auto shifted = rivan::ui::SongRowLayout::Defaults();
    std::array<float, rivan::ui::kSongRowFieldCount> shiftedWidths{};
    shiftedWidths[numberIndex] = 0.12F;
    shiftedWidths[coverIndex] = 0.10F;
    shifted.Field(rivan::ui::SongRowField::Cover).snap =
        rivan::ui::SongRowSnap{rivan::ui::SongRowField::Number,
                               rivan::ui::SongRowSnapSide::Right, 2};
    const auto shiftedResolved = rivan::ui::SongRowResolvedFieldXs(
        shifted, 0.0F, 400.0F, shiftedWidths,
        rivan::ui::SongRowTransientLayout{0.20F});
    const float shiftedNumberRight =
        0.0F + (shiftedResolved[numberIndex] + shiftedWidths[numberIndex]) * 400.0F - 1.0F;
    const float shiftedCoverLeft = 0.0F + shiftedResolved[coverIndex] * 400.0F + 1.0F;
    Check(std::abs(shiftedResolved[numberIndex] - 0.20F) < 0.001F &&
              std::abs(shiftedCoverLeft - shiftedNumberRight - 2.0F) < 0.01F,
          "snapped cover tracks the transmitted number minimum at render time");

    // A wide multi-digit number shifted by the indicator must still force an
    // independent cover to clear it instead of painting over the digits.
    auto indicatorClearance = rivan::ui::SongRowLayout::Defaults();
    std::array<float, rivan::ui::kSongRowFieldCount> clearanceWidths{};
    clearanceWidths[numberIndex] = 0.12F;
    clearanceWidths[coverIndex] = 0.10F;
    indicatorClearance.Field(rivan::ui::SongRowField::Cover).x = 0.30F;
    const auto clearanceResolved = rivan::ui::SongRowResolvedFieldXs(
        indicatorClearance, 0.0F, 400.0F, clearanceWidths,
        rivan::ui::SongRowTransientLayout{0.10F});
    const float clearanceNumberRight =
        0.0F + (clearanceResolved[numberIndex] + clearanceWidths[numberIndex]) * 400.0F - 1.0F;
    const float clearanceCoverLeft = 0.0F + clearanceResolved[coverIndex] * 400.0F + 1.0F;
    Check(std::abs(clearanceResolved[numberIndex] - 0.10F) < 0.001F &&
              clearanceCoverLeft >= clearanceNumberRight + 1.0F - 0.01F,
          "transmitted number minimum keeps independent cover from hiding digits");
}

void TestTrackCoverCacheDimensions() {
    using rivan::ui::BucketTrackCoverDimension;

    Check(BucketTrackCoverDimension(1u) == 16u &&
              BucketTrackCoverDimension(16u) == 16u &&
              BucketTrackCoverDimension(17u) == 32u &&
              BucketTrackCoverDimension(33u) == 64u &&
              BucketTrackCoverDimension(129u) == 256u &&
              BucketTrackCoverDimension(999u) == rivan::ui::kMaximumTrackCoverDimension,
          "track cover cache dimensions use capped reusable tiers");
}

void TestUiModuleRegistry() {
    using rivan::ui::ModuleId;
    using rivan::ui::ModuleCollapseMode;
    using rivan::ui::ModuleCollapseSide;
    using rivan::ui::ModuleLayout;
    using rivan::ui::UiModuleRegistry;

    const auto modules = UiModuleRegistry::Modules();
    Check(modules.size() == 6, "the built-in UI module registry contains six sections");
    Check(UiModuleRegistry::Get(ModuleId::Rivan).Key() == "rivan",
           "the Rivan section has a stable key");
    Check(UiModuleRegistry::Get(ModuleId::Rivan).Title() == L"PLAYER",
          "the player section has the Player title");
    Check(UiModuleRegistry::Get(ModuleId::AllMusic).Title() == L"ALL MUSIC",
          "the All Music section has a stable title");
    Check(UiModuleRegistry::Get(ModuleId::GraphicEqualizer).Key() == "graphic_equalizer",
          "the graphic equalizer section has a stable key");
    Check(UiModuleRegistry::Find(ModuleId::RivanLibrary) != nullptr,
           "the Rivan Library section is discoverable by identity");
    Check(UiModuleRegistry::Get(ModuleId::RivanLibrary).Title() == L"LIBRARY",
          "the library section uses the short title");
    Check(UiModuleRegistry::Get(ModuleId::VideoPreview).Key() == "video_preview",
          "the video preview section has a stable key");
    Check(UiModuleRegistry::Get(ModuleId::VideoPreview).Title() == L"VIDEO PREVIEW",
          "the video preview section has a stable title");
    Check(UiModuleRegistry::Get(ModuleId::Lyrics).Key() == "lyrics" &&
              UiModuleRegistry::Get(ModuleId::Lyrics).Title() == L"LYRICS",
          "the lyrics section has stable identity metadata");

    auto layout = rivan::ui::ModuleLayout::Defaults();
    Check(layout.Find(ModuleId::Rivan)->width > 0.0F,
          "module defaults provide normalized geometry");
    const auto* videoPreview = layout.Find(ModuleId::VideoPreview);
    Check(videoPreview != nullptr && std::abs(videoPreview->x - 0.46F) < 0.001F &&
              std::abs(videoPreview->y - 0.49F) < 0.001F &&
              std::abs(videoPreview->width - 0.54F) < 0.001F &&
              std::abs(videoPreview->height - 0.24F) < 0.001F,
           "module defaults reserve a standalone video preview section");
    const auto* lyrics = layout.Find(ModuleId::Lyrics);
    Check(lyrics != nullptr && lyrics->visible && lyrics->width > 0.0F,
          "module defaults reserve a lyrics section");
    layout.MakeTab(ModuleId::Rivan, ModuleId::AllMusic);
    Check(layout.tabCount == 2 && layout.IsTabbed(ModuleId::AllMusic),
          "module layout can create a tab group");
    layout.TabWith(ModuleId::GraphicEqualizer, ModuleId::Rivan);
    Check(layout.tabCount == 3 && layout.activeTab == 2,
          "dropping a module on a tabbed module extends the tab group");
    layout.RemoveTab(ModuleId::AllMusic);
    Check(layout.tabCount == 2 && !layout.IsTabbed(ModuleId::AllMusic),
          "module layout removes a tab cleanly");

    auto nestedTabs = rivan::ui::ModuleLayout::Defaults();
    nestedTabs.MakeTab(ModuleId::RivanLibrary, ModuleId::Rivan);
    nestedTabs.TabWith(ModuleId::GraphicEqualizer, ModuleId::Rivan);
    Check(nestedTabs.tabCount == 3 &&
              nestedTabs.tabOrder[0] == ModuleId::RivanLibrary &&
              nestedTabs.activeTab == 2,
          "adding a module to an existing tab group preserves its root");
    const auto libraryGeometry = *nestedTabs.Find(ModuleId::RivanLibrary);
    nestedTabs.RemoveTab(ModuleId::Rivan);
    const auto equalizerGeometry = *nestedTabs.Find(ModuleId::GraphicEqualizer);
    Check(nestedTabs.tabCount == 2 &&
              std::abs(equalizerGeometry.x - libraryGeometry.x) < 0.001F &&
              std::abs(equalizerGeometry.width - libraryGeometry.width) < 0.001F,
           "removing a module leaves the remaining tab group at its visible size");

    auto independentTabs = ModuleLayout::Defaults();
    independentTabs.MakeTab(ModuleId::Rivan, ModuleId::AllMusic);
    independentTabs.MakeTab(ModuleId::GraphicEqualizer, ModuleId::RivanLibrary);
    Check(independentTabs.tabCount == 4 &&
              independentTabs.IsTabbed(ModuleId::Rivan) &&
              independentTabs.IsTabbed(ModuleId::AllMusic) &&
              independentTabs.IsTabbed(ModuleId::GraphicEqualizer) &&
              independentTabs.IsTabbed(ModuleId::RivanLibrary) &&
              independentTabs.TabRoot(ModuleId::Rivan) == ModuleId::Rivan &&
              independentTabs.TabRoot(ModuleId::AllMusic) == ModuleId::Rivan &&
              independentTabs.TabRoot(ModuleId::GraphicEqualizer) == ModuleId::GraphicEqualizer &&
              independentTabs.TabRoot(ModuleId::RivanLibrary) == ModuleId::GraphicEqualizer &&
              independentTabs.GroupTabCount(ModuleId::Rivan) == 2 &&
              independentTabs.GroupTabCount(ModuleId::GraphicEqualizer) == 2,
          "multiple tab groups retain independent membership and roots");
    independentTabs.RemoveTab(ModuleId::RivanLibrary);
    Check(independentTabs.IsTabbed(ModuleId::Rivan) &&
              independentTabs.IsTabbed(ModuleId::AllMusic) &&
              independentTabs.GroupTabCount(ModuleId::Rivan) == 2 &&
              !independentTabs.IsTabbed(ModuleId::GraphicEqualizer) &&
              !independentTabs.IsTabbed(ModuleId::RivanLibrary),
          "removing one tab affects only its selected tab group");

    auto mergedTabGroups = ModuleLayout::Defaults();
    mergedTabGroups.MakeTab(ModuleId::Rivan, ModuleId::AllMusic);
    mergedTabGroups.MakeTab(ModuleId::GraphicEqualizer, ModuleId::RivanLibrary);
    mergedTabGroups.MakeTab(ModuleId::VideoPreview, ModuleId::Lyrics);
    mergedTabGroups.TabWith(ModuleId::AllMusic, ModuleId::GraphicEqualizer);
    Check(mergedTabGroups.tabCount == 6 &&
              mergedTabGroups.TabRoot(ModuleId::Rivan) == ModuleId::GraphicEqualizer &&
              mergedTabGroups.TabRoot(ModuleId::AllMusic) == ModuleId::GraphicEqualizer &&
              mergedTabGroups.TabRoot(ModuleId::GraphicEqualizer) == ModuleId::GraphicEqualizer &&
              mergedTabGroups.TabRoot(ModuleId::RivanLibrary) == ModuleId::GraphicEqualizer &&
              mergedTabGroups.GroupTabCount(ModuleId::GraphicEqualizer) == 4 &&
              mergedTabGroups.TabRoot(ModuleId::VideoPreview) == ModuleId::VideoPreview &&
              mergedTabGroups.TabRoot(ModuleId::Lyrics) == ModuleId::VideoPreview &&
              mergedTabGroups.GroupTabCount(ModuleId::VideoPreview) == 2 &&
              mergedTabGroups.GroupActiveMember(ModuleId::GraphicEqualizer) == ModuleId::AllMusic,
          "tabbing two existing groups merges only target and source groups");

    using rivan::ui::ModuleDropZone;
    Check(rivan::ui::ResolveModuleDropZone(50.0F, 50.0F, 0.0F, 0.0F, 100.0F, 100.0F) ==
              ModuleDropZone::Center,
          "module drop resolver uses the center for tab merging");
    Check(rivan::ui::ResolveModuleDropZone(5.0F, 50.0F, 0.0F, 0.0F, 100.0F, 100.0F) ==
              ModuleDropZone::Left &&
              rivan::ui::ResolveModuleDropZone(95.0F, 50.0F, 0.0F, 0.0F, 100.0F, 100.0F) ==
              ModuleDropZone::Right,
          "module drop resolver distinguishes horizontal side drops");
    Check(rivan::ui::ResolveModuleDropZone(50.0F, 5.0F, 0.0F, 0.0F, 100.0F, 100.0F) ==
              ModuleDropZone::Top &&
              rivan::ui::ResolveModuleDropZone(50.0F, 95.0F, 0.0F, 0.0F, 100.0F, 100.0F) ==
              ModuleDropZone::Bottom,
          "module drop resolver distinguishes vertical side drops");

    auto snapped = rivan::ui::ModuleLayout::Defaults();
    Check(snapped.SnapTo(ModuleId::AllMusic, ModuleId::Rivan, ModuleDropZone::Right) &&
               snapped.IsSnapped(ModuleId::AllMusic) && snapped.IsSnapped(ModuleId::Rivan) &&
               snapped.IsSnapGrouped(ModuleId::AllMusic) &&
               snapped.SnapRoot(ModuleId::AllMusic) == ModuleId::Rivan &&
               std::abs(snapped.Find(ModuleId::AllMusic)->x - 0.22F) < 0.001F &&
               std::abs(snapped.Find(ModuleId::Rivan)->width - 0.22F) < 0.001F,
           "side module drops split the target and mark both modules snapped");
    snapped.DetachSnapModule(ModuleId::AllMusic);
    Check(!snapped.IsSnapGrouped(ModuleId::AllMusic) &&
              snapped.Find(ModuleId::AllMusic)->dockState == rivan::ui::ModuleDockState::Floating &&
              snapped.Find(ModuleId::Rivan)->dockState == rivan::ui::ModuleDockState::Floating,
          "dragging a snapped child detaches only that module");

    auto snappedRoot = rivan::ui::ModuleLayout::Defaults();
    Check(snappedRoot.SnapTo(ModuleId::AllMusic, ModuleId::Rivan, ModuleDropZone::Right),
          "root snap group can be created");
    snappedRoot.Find(ModuleId::Rivan)->x = 0.3F;
    snappedRoot.Find(ModuleId::AllMusic)->x = 0.52F;
    const float snappedChildX = snappedRoot.Find(ModuleId::AllMusic)->x;
    const float snappedRootX = snappedRoot.Find(ModuleId::Rivan)->x;
    snappedRoot.Find(ModuleId::Rivan)->x += 0.05F;
    snappedRoot.Find(ModuleId::AllMusic)->x += 0.05F;
    Check(std::abs(snappedRoot.Find(ModuleId::AllMusic)->x - (snappedChildX + 0.05F)) < 0.001F &&
              std::abs(snappedRoot.Find(ModuleId::Rivan)->x - (snappedRootX + 0.05F)) < 0.001F,
          "moving a snapped root preserves the module group");

    auto resized = rivan::ui::ModuleLayout::Defaults();
    Check(resized.SnapTo(ModuleId::AllMusic, ModuleId::Rivan, ModuleDropZone::Right),
          "snap group can be resized");
    const float oldRivanWidth = resized.Find(ModuleId::Rivan)->width;
    const float oldMusicWidth = resized.Find(ModuleId::AllMusic)->width;
    (void)resized.ResizeSnapGroup(ModuleId::Rivan, 0.60F, 0.15F, true, false, false, false);
    Check(resized.Find(ModuleId::Rivan)->width > oldRivanWidth &&
              resized.Find(ModuleId::AllMusic)->width > oldMusicWidth &&
              std::abs(resized.Find(ModuleId::Rivan)->width -
                       resized.Find(ModuleId::AllMusic)->width) < 0.001F,
          "resizing a snapped group updates every member");

    auto collapsibleResize = rivan::ui::ModuleLayout::Defaults();
    for (auto& item : collapsibleResize.items) item.visible = false;
    collapsibleResize.Find(ModuleId::Rivan)->visible = true;
    collapsibleResize.Find(ModuleId::Rivan)->x = 0.0F;
    collapsibleResize.Find(ModuleId::Rivan)->y = 0.0F;
    collapsibleResize.Find(ModuleId::Rivan)->width = 0.50F;
    collapsibleResize.Find(ModuleId::Rivan)->height = 1.0F;
    collapsibleResize.Find(ModuleId::AllMusic)->visible = true;
    collapsibleResize.Find(ModuleId::AllMusic)->x = 0.50F;
    collapsibleResize.Find(ModuleId::AllMusic)->y = 0.0F;
    collapsibleResize.Find(ModuleId::AllMusic)->width = 0.50F;
    collapsibleResize.Find(ModuleId::AllMusic)->height = 1.0F;
    Check(collapsibleResize.CollapseToModule(ModuleId::Rivan, ModuleId::AllMusic,
                                              rivan::ui::ModuleCollapseSide::Left,
                                              rivan::ui::ModuleCollapseMode::Outside),
          "a module can collapse outside another module before resizing");
    Check(collapsibleResize.ToggleCollapsedModule(ModuleId::Rivan),
          "an outside-collapsed module can be expanded before resizing");
    Check(collapsibleResize.SnapTo(ModuleId::Rivan, ModuleId::AllMusic,
                                   ModuleDropZone::Left),
          "an expanded collapsible module can join a snap group");
    const auto oldExpandedWidth = collapsibleResize.Find(ModuleId::Rivan)->expandedWidth;
    (void)collapsibleResize.ResizeSnapGroup(ModuleId::AllMusic, 0.40F, 0.5F,
                                             false, false, true, false);
    const auto* resizedCollapsible = collapsibleResize.Find(ModuleId::Rivan);
    Check(resizedCollapsible != nullptr &&
              std::abs(resizedCollapsible->width - oldExpandedWidth) > 0.001F,
          "resizing a snapped group changes the collapsible member's visible width");
    Check(resizedCollapsible != nullptr &&
              std::abs(resizedCollapsible->expandedX - resizedCollapsible->x) < 0.001F &&
              std::abs(resizedCollapsible->expandedY - resizedCollapsible->y) < 0.001F &&
              std::abs(resizedCollapsible->expandedWidth - resizedCollapsible->width) < 0.001F &&
              std::abs(resizedCollapsible->expandedHeight - resizedCollapsible->height) < 0.001F,
           "resizing a snapped group keeps an expanded collapsible member's geometry in sync");

    auto snappedInsideChildren = rivan::ui::ModuleLayout::Defaults();
    for (auto& item : snappedInsideChildren.items) item.visible = false;
    auto* sourceRoot = snappedInsideChildren.Find(ModuleId::Rivan);
    auto* sourceChild = snappedInsideChildren.Find(ModuleId::AllMusic);
    auto* targetRoot = snappedInsideChildren.Find(ModuleId::GraphicEqualizer);
    auto* targetChild = snappedInsideChildren.Find(ModuleId::RivanLibrary);
    sourceRoot->visible = true;
    sourceRoot->x = 0.0F;
    sourceRoot->y = 0.0F;
    sourceRoot->width = 0.40F;
    sourceRoot->height = 1.0F;
    sourceChild->visible = true;
    sourceChild->x = 0.40F;
    sourceChild->y = 0.0F;
    sourceChild->width = 0.10F;
    sourceChild->height = 1.0F;
    targetRoot->visible = true;
    targetRoot->x = 0.60F;
    targetRoot->y = 0.0F;
    targetRoot->width = 0.40F;
    targetRoot->height = 1.0F;
    targetChild->visible = true;
    targetChild->x = 0.50F;
    targetChild->y = 0.0F;
    targetChild->width = 0.10F;
    targetChild->height = 1.0F;
    Check(snappedInsideChildren.CollapseToModule(
              ModuleId::AllMusic, ModuleId::Rivan, ModuleCollapseSide::Right,
              ModuleCollapseMode::Inside) &&
              snappedInsideChildren.CollapseToModule(
                  ModuleId::RivanLibrary, ModuleId::GraphicEqualizer,
                  ModuleCollapseSide::Right, ModuleCollapseMode::Inside),
          "snap re-anchor fixture creates inside-collapsed children on both roots");
    sourceRoot = snappedInsideChildren.Find(ModuleId::Rivan);
    sourceChild = snappedInsideChildren.Find(ModuleId::AllMusic);
    targetRoot = snappedInsideChildren.Find(ModuleId::GraphicEqualizer);
    targetChild = snappedInsideChildren.Find(ModuleId::RivanLibrary);
    const auto oldSourceBounds = ModuleLayout::Bounds(*sourceRoot);
    const auto oldSourceChild = *sourceChild;
    const auto oldTargetBounds = ModuleLayout::Bounds(*targetRoot);
    const auto oldTargetChild = *targetChild;
    Check(snappedInsideChildren.SnapTo(ModuleId::Rivan, ModuleId::GraphicEqualizer,
                                       ModuleDropZone::Left),
          "snapping roots with inside-collapsed children succeeds");
    sourceRoot = snappedInsideChildren.Find(ModuleId::Rivan);
    sourceChild = snappedInsideChildren.Find(ModuleId::AllMusic);
    targetRoot = snappedInsideChildren.Find(ModuleId::GraphicEqualizer);
    targetChild = snappedInsideChildren.Find(ModuleId::RivanLibrary);
    const auto newSourceBounds = ModuleLayout::Bounds(*sourceRoot);
    const auto newTargetBounds = ModuleLayout::Bounds(*targetRoot);
    const float sourceScaleX = (newSourceBounds.right - newSourceBounds.left) /
        (oldSourceBounds.right - oldSourceBounds.left);
    const float targetScaleX = (newTargetBounds.right - newTargetBounds.left) /
        (oldTargetBounds.right - oldTargetBounds.left);
    Check(sourceChild != nullptr && targetChild != nullptr &&
              std::abs(sourceChild->expandedX -
                       (newSourceBounds.left +
                        (oldSourceChild.expandedX - oldSourceBounds.left) * sourceScaleX)) <
                  0.001F &&
              std::abs(sourceChild->handleX -
                       (newSourceBounds.left +
                        (oldSourceChild.handleX - oldSourceBounds.left) * sourceScaleX)) <
                  0.001F &&
              std::abs(targetChild->expandedX -
                       (newTargetBounds.left +
                        (oldTargetChild.expandedX - oldTargetBounds.left) * targetScaleX)) <
                  0.001F &&
              std::abs(targetChild->handleX -
                       (newTargetBounds.left +
                        (oldTargetChild.handleX - oldTargetBounds.left) * targetScaleX)) <
                  0.001F,
           "snapping re-anchors existing inside-collapsed child geometry");

    auto collapsedSnapResize = rivan::ui::ModuleLayout::Defaults();
    for (auto& item : collapsedSnapResize.items) item.visible = false;
    auto* collapsedSnapRoot = collapsedSnapResize.Find(ModuleId::Rivan);
    auto* collapsedSnapMember = collapsedSnapResize.Find(ModuleId::AllMusic);
    collapsedSnapRoot->visible = true;
    collapsedSnapRoot->x = 0.0F;
    collapsedSnapRoot->y = 0.0F;
    collapsedSnapRoot->width = 0.50F;
    collapsedSnapRoot->height = 1.0F;
    collapsedSnapMember->visible = true;
    collapsedSnapMember->x = 0.50F;
    collapsedSnapMember->y = 0.0F;
    collapsedSnapMember->width = 0.50F;
    collapsedSnapMember->height = 1.0F;
    Check(collapsedSnapResize.SnapTo(ModuleId::AllMusic, ModuleId::Rivan,
                                     ModuleDropZone::Right),
          "collapsed snap-group resize fixture is ready");
    const auto collapsedExpandedBounds =
        ModuleLayout::Bounds(*collapsedSnapResize.Find(ModuleId::AllMusic));
    collapsedSnapResize.SetCollapsedGeometry(
        *collapsedSnapResize.Find(ModuleId::AllMusic),
        {collapsedExpandedBounds.right - 0.06F, 0.40F, collapsedExpandedBounds.right, 0.60F},
        collapsedExpandedBounds, ModuleCollapseMode::Outside, ModuleCollapseSide::Right,
        ModuleId::AllMusic, true);
    const float oldCollapsedSnapRootWidth =
        collapsedSnapResize.Find(ModuleId::Rivan)->width;
    Check(collapsedSnapResize.ResizeSnapGroup(
              ModuleId::Rivan, 0.10F, 0.5F, true, false, false, false, false),
          "a snapped group with a collapsed member accepts a shrink");
    Check(collapsedSnapResize.Find(ModuleId::Rivan)->width < oldCollapsedSnapRootWidth &&
              collapsedSnapResize.Find(ModuleId::AllMusic)->collapsed &&
              collapsedSnapResize.Find(ModuleId::AllMusic)->expandedWidth >
                  collapsedSnapResize.Find(ModuleId::AllMusic)->width,
          "collapsed snap-group resize shrinks from expanded member dimensions");
}

void TestDuplicateModuleGeometryRepair() {
    using rivan::ui::ModuleId;
    using rivan::ui::ModuleLayout;

    auto layout = ModuleLayout::Defaults();
    auto* first = layout.Find(ModuleId::Rivan);
    auto* second = layout.Find(ModuleId::AllMusic);
    second->x = first->x;
    second->y = first->y;
    second->width = first->width;
    second->height = first->height;
    Check(layout.HasConflictingGeometry(),
          "independent modules with identical bounds are conflicting");
    Check(layout.DisableDuplicateIndependentModules() && !second->visible && first->visible,
          "duplicate independent geometry disables the later module");

    auto tabbed = ModuleLayout::Defaults();
    tabbed.MakeTab(ModuleId::Rivan, ModuleId::AllMusic);
    Check(!tabbed.DisableDuplicateIndependentModules() &&
              tabbed.Find(ModuleId::Rivan)->visible && tabbed.Find(ModuleId::AllMusic)->visible,
          "tabbed modules may intentionally share identical bounds");

    auto tabbedConflict = ModuleLayout::Defaults();
    tabbedConflict.MakeTab(ModuleId::AllMusic, ModuleId::GraphicEqualizer);
    first = tabbedConflict.Find(ModuleId::Rivan);
    second = tabbedConflict.Find(ModuleId::AllMusic);
    second->x = first->x;
    second->y = first->y;
    second->width = first->width;
    second->height = first->height;
    Check(tabbedConflict.DisableDuplicateIndependentModules() && !second->visible &&
              !tabbedConflict.IsTabbed(ModuleId::AllMusic) &&
              !tabbedConflict.IsTabbed(ModuleId::GraphicEqualizer),
          "disabling a conflicting tab member also repairs its tab group");
}

void TestWindowSnapping() {
    using rivan::ui::ModuleId;
    using rivan::ui::ModuleWindowDropZone;
    using rivan::ui::ModuleLayout;

    // Zone resolution from normalized window coordinates.
    Check(rivan::ui::ResolveModuleWindowDropZone(0.75F, 0.1F) == ModuleWindowDropZone::RightTop &&
          rivan::ui::ResolveModuleWindowDropZone(0.9F, 0.9F) == ModuleWindowDropZone::RightBottom &&
          rivan::ui::ResolveModuleWindowDropZone(0.5F, 0.5F) == ModuleWindowDropZone::Center &&
          rivan::ui::ResolveModuleWindowDropZone(0.1F, 0.4F) == ModuleWindowDropZone::LeftMiddle,
          "window drop resolver selects the seven targets by pointer quadrant");

    // Free-region placement: Rivan occupies the left half, the whole right half is
    // empty, so a right-middle snap fills the entire right section.
    auto freeRight = rivan::ui::ModuleLayout::Defaults();
    for (auto& item : freeRight.items) item.visible = false;
    freeRight.Find(ModuleId::Rivan)->visible = true;
    freeRight.Find(ModuleId::Rivan)->x = 0.0F; freeRight.Find(ModuleId::Rivan)->y = 0.0F;
    freeRight.Find(ModuleId::Rivan)->width = 0.5F; freeRight.Find(ModuleId::Rivan)->height = 1.0F;
    Check(freeRight.SnapToWindow(ModuleId::AllMusic, ModuleWindowDropZone::RightMiddle,
                                 0.75F, 0.5F),
          "a module can snap into a free window part");
    Check(std::abs(freeRight.Find(ModuleId::AllMusic)->x - 0.5F) < 0.001F &&
              std::abs(freeRight.Find(ModuleId::AllMusic)->width - 0.5F) < 0.001F &&
              std::abs(freeRight.Find(ModuleId::AllMusic)->height - 1.0F) < 0.001F,
          "a free half-window snap fills the target rectangle");

    // The nesting requirement: RivanLibrary occupies the right side. Snap AllMusic to
    // the right-top corner, then snap GraphicEqualizer to the right-middle target. The
    // second snap must consume only the remaining right-side space, never the whole
    // right section, and never overlap anything.
    auto nested = rivan::ui::ModuleLayout::Defaults();
    for (auto& item : nested.items) item.visible = false;
    nested.Find(ModuleId::Rivan)->visible = true;
    nested.Find(ModuleId::Rivan)->x = 0.0F; nested.Find(ModuleId::Rivan)->y = 0.0F;
    nested.Find(ModuleId::Rivan)->width = 0.5F; nested.Find(ModuleId::Rivan)->height = 1.0F;
    nested.Find(ModuleId::RivanLibrary)->visible = true;
    nested.Find(ModuleId::RivanLibrary)->x = 0.5F; nested.Find(ModuleId::RivanLibrary)->y = 0.0F;
    nested.Find(ModuleId::RivanLibrary)->width = 0.5F; nested.Find(ModuleId::RivanLibrary)->height = 1.0F;
    Check(nested.SnapToWindow(ModuleId::AllMusic, ModuleWindowDropZone::RightTop,
                              0.75F, 0.1F),
          "snapping to an occupied right-top splits the occupying module");
    Check(std::abs(nested.Find(ModuleId::AllMusic)->x - 0.5F) < 0.001F &&
              std::abs(nested.Find(ModuleId::AllMusic)->width - 0.5F) < 0.001F &&
              std::abs(nested.Find(ModuleId::AllMusic)->height - 0.5F) < 0.001F &&
              std::abs(nested.Find(ModuleId::RivanLibrary)->y - 0.5F) < 0.001F,
          "the occupying right module keeps the remaining bottom half");
    Check(!nested.HasConflictingGeometry(),
          "splitting an occupied right-top introduces no overlap");

    Check(nested.SnapToWindow(ModuleId::GraphicEqualizer, ModuleWindowDropZone::RightMiddle,
                              0.75F, 0.6F),
          "a later right-middle snap still applies");
    const auto* allMusic = nested.Find(ModuleId::AllMusic);
    const auto* library = nested.Find(ModuleId::RivanLibrary);
    const auto* equalizer = nested.Find(ModuleId::GraphicEqualizer);
    Check(std::abs(allMusic->height - 0.5F) < 0.001F,
          "the right-top module keeps its original size when right-middle is snapped");
    Check(std::abs(equalizer->width - 0.25F) < 0.001F &&
              std::abs(library->width - 0.25F) < 0.001F &&
              std::abs(library->x + library->width - equalizer->x) < 0.001F,
          "right-middle consumes only the remaining right-bottom space");
    Check(!nested.HasConflictingGeometry(),
          "nested window snaps never overlap");

    auto resizeCanvas = rivan::ui::ModuleLayout::Defaults();
    for (auto& item : resizeCanvas.items) item.visible = false;
    auto* canvasModule = resizeCanvas.Find(ModuleId::Rivan);
    canvasModule->visible = true;
    canvasModule->x = 0.20F;
    canvasModule->y = 0.10F;
    canvasModule->width = 0.50F;
    canvasModule->height = 0.40F;
    Check(resizeCanvas.PreservePixelGeometry(1000.0F, 800.0F, 700.0F, 800.0F),
          "trailing-module resize preserves module pixels while surrounding space remains");
    canvasModule = resizeCanvas.Find(ModuleId::Rivan);
    Check(std::abs(canvasModule->x - (200.0F / 700.0F)) < 0.001F &&
              std::abs(canvasModule->width - (500.0F / 700.0F)) < 0.001F &&
              std::abs(canvasModule->height - 0.40F) < 0.001F,
          "trailing-module resize removes unused space before squeezing modules");
    Check(!resizeCanvas.PreservePixelGeometry(700.0F, 800.0F, 400.0F, 800.0F),
          "trailing-module resize leaves layout unchanged once modules no longer fit");

    auto edgeResize = rivan::ui::ModuleLayout::Defaults();
    for (auto& item : edgeResize.items) item.visible = false;
    auto* edgeModule = edgeResize.Find(ModuleId::Rivan);
    edgeModule->visible = true;
    edgeModule->x = 0.0F;
    edgeModule->y = 0.20F;
    edgeModule->width = 0.50F;
    edgeModule->height = 0.40F;
    Check(edgeResize.PreservePixelGeometry(1000.0F, 800.0F, 900.0F, 800.0F,
                                           false, false, true, false),
          "left-edge resize preserves an attached module's right pixel edge");
    edgeModule = edgeResize.Find(ModuleId::Rivan);
    Check(std::abs(edgeModule->x) < 0.001F &&
              std::abs(edgeModule->width - (400.0F / 900.0F)) < 0.001F,
          "left-edge resize changes module width to fit the new canvas");

    auto scaleAllCollapsed = ModuleLayout::Defaults();
    for (auto& item : scaleAllCollapsed.items) item.visible = false;
    auto* scaleAllTarget = scaleAllCollapsed.Find(ModuleId::Rivan);
    auto* scaleAllSource = scaleAllCollapsed.Find(ModuleId::AllMusic);
    scaleAllTarget->visible = true;
    scaleAllTarget->x = 0.0F;
    scaleAllTarget->y = 0.0F;
    scaleAllTarget->width = 0.10F;
    scaleAllTarget->height = 1.0F;
    scaleAllSource->visible = true;
    scaleAllSource->x = 0.10F;
    scaleAllSource->y = 0.20F;
    scaleAllSource->width = 0.80F;
    scaleAllSource->height = 0.40F;
    Check(scaleAllCollapsed.CollapseToModule(
              ModuleId::AllMusic, ModuleId::Rivan,
              rivan::ui::ModuleCollapseSide::Right,
              rivan::ui::ModuleCollapseMode::Outside),
          "right-edge collapse prepares ScaleAll geometry preservation");
    const auto scaleAllBefore = *scaleAllSource;
    const auto scaleAllTargetBefore = *scaleAllTarget;
    const float scaleAllExpandedWidthPixels = scaleAllBefore.expandedWidth * 1000.0F;
    const float scaleAllExpandedXPixels = scaleAllBefore.expandedX * 1000.0F;
    Check(scaleAllCollapsed.PreserveCollapsedExpandedGeometry(
              1000.0F, 800.0F, 700.0F, 800.0F, true),
          "ScaleAll resize preserves collapsed expanded geometry");
    scaleAllSource = scaleAllCollapsed.Find(ModuleId::AllMusic);
    scaleAllTarget = scaleAllCollapsed.Find(ModuleId::Rivan);
    Check(scaleAllSource != nullptr && scaleAllTarget != nullptr &&
              std::abs(scaleAllSource->expandedWidth * 700.0F -
                       scaleAllExpandedWidthPixels) < 0.01F &&
              std::abs(scaleAllSource->expandedX * 700.0F - scaleAllExpandedXPixels) < 0.01F &&
              scaleAllSource->x == scaleAllBefore.x &&
              scaleAllSource->width == scaleAllBefore.width &&
              scaleAllSource->handleX == scaleAllBefore.handleX &&
              scaleAllSource->handleWidth == scaleAllBefore.handleWidth &&
              scaleAllTarget->x == scaleAllTargetBefore.x &&
              scaleAllTarget->width == scaleAllTargetBefore.width &&
              std::abs(scaleAllSource->handleX -
                       (scaleAllTarget->x + scaleAllTarget->width)) < 0.001F,
          "ScaleAll resize leaves collapsed handle and non-collapsed layout unchanged");
    float scaleAllExpandedCanvasWidth = 0.0F;
    float scaleAllExpandedCanvasHeight = 0.0F;
    Check(scaleAllCollapsed.ToggleCollapsedModule(
              ModuleId::AllMusic, rivan::ui::ModuleExpansionBehavior::Squash,
              700.0F, 800.0F, &scaleAllExpandedCanvasWidth, &scaleAllExpandedCanvasHeight) &&
              scaleAllExpandedCanvasWidth > 700.0F &&
              std::abs(scaleAllCollapsed.Find(ModuleId::AllMusic)->width *
                       scaleAllExpandedCanvasWidth - scaleAllExpandedWidthPixels) < 0.01F,
          "later right-edge expansion requests larger canvas after ScaleAll shrink");

    auto leadingCollapsed = ModuleLayout::Defaults();
    for (auto& item : leadingCollapsed.items) item.visible = false;
    auto* leadingTarget = leadingCollapsed.Find(ModuleId::Rivan);
    auto* leadingSource = leadingCollapsed.Find(ModuleId::AllMusic);
    leadingTarget->visible = true;
    leadingTarget->x = 0.65F;
    leadingTarget->y = 0.0F;
    leadingTarget->width = 0.10F;
    leadingTarget->height = 1.0F;
    leadingSource->visible = true;
    leadingSource->x = 0.0F;
    leadingSource->y = 0.20F;
    leadingSource->width = 0.50F;
    leadingSource->height = 0.40F;
    Check(leadingCollapsed.CollapseToModule(
              ModuleId::AllMusic, ModuleId::Rivan,
              rivan::ui::ModuleCollapseSide::Left,
              rivan::ui::ModuleCollapseMode::Outside),
          "leading-edge collapse prepares edge-shift validation");
    leadingSource = leadingCollapsed.Find(ModuleId::AllMusic);
    const auto leadingBefore = *leadingSource;
    Check(leadingCollapsed.PreserveCollapsedExpandedGeometry(
              1000.0F, 800.0F, 900.0F, 800.0F, false, false, true, false),
          "leading-edge resize shifts collapsed expanded geometry");
    leadingSource = leadingCollapsed.Find(ModuleId::AllMusic);
    Check(std::abs(leadingSource->expandedX * 900.0F -
                       (leadingBefore.expandedX * 1000.0F - 100.0F)) < 0.01F &&
              std::abs(leadingSource->expandedWidth * 900.0F -
                       leadingBefore.expandedWidth * 1000.0F) < 0.01F,
          "leading-edge resize preserves collapsed expanded screen position and size");

    auto collapsedTrailing = ModuleLayout::Defaults();
    for (auto& item : collapsedTrailing.items) item.visible = false;
    auto* collapsedTrailingItem = collapsedTrailing.Find(ModuleId::Rivan);
    collapsedTrailingItem->visible = true;
    collapsedTrailingItem->x = 0.60F;
    collapsedTrailingItem->y = 0.20F;
    collapsedTrailingItem->width = 0.40F;
    collapsedTrailingItem->height = 0.40F;
    Check(collapsedTrailing.CollapseToWindow(ModuleId::Rivan,
                                             rivan::ui::ModuleCollapseSide::Right),
          "a right-window collapse can be prepared for trailing resize");
    const float expandedTrailingWidthPixels =
        collapsedTrailing.Find(ModuleId::Rivan)->expandedWidth * 1000.0F;
    Check(collapsedTrailing.PreservePixelGeometry(1000.0F, 800.0F, 700.0F, 800.0F,
                                                   true),
          "trailing resize preserves a collapsed right-window handle");
    collapsedTrailingItem = collapsedTrailing.Find(ModuleId::Rivan);
    Check(collapsedTrailingItem != nullptr && collapsedTrailingItem->collapsed &&
              collapsedTrailingItem->width > 0.0F &&
              collapsedTrailingItem->x + collapsedTrailingItem->width <= 1.0F + 0.001F &&
              std::abs(collapsedTrailingItem->expandedWidth * 700.0F -
                       expandedTrailingWidthPixels) < 0.01F,
          "trailing resize keeps collapsed expanded physical width unchanged");
    float expandedTrailingCanvasWidth = 0.0F;
    float expandedTrailingCanvasHeight = 0.0F;
    Check(collapsedTrailing.ToggleCollapsedModule(
              ModuleId::Rivan, rivan::ui::ModuleExpansionBehavior::Squash,
              700.0F, 800.0F, &expandedTrailingCanvasWidth, &expandedTrailingCanvasHeight) &&
              expandedTrailingCanvasWidth > 700.0F,
          "overflow collapsed expansion grows the canvas with squash behavior");

    auto outsideHandleShrink = ModuleLayout::Defaults();
    for (auto& item : outsideHandleShrink.items) item.visible = false;
    auto* outsideHandleTarget = outsideHandleShrink.Find(ModuleId::Rivan);
    auto* outsideHandleSource = outsideHandleShrink.Find(ModuleId::AllMusic);
    outsideHandleTarget->visible = true;
    outsideHandleTarget->x = 0.30F;
    outsideHandleTarget->y = 0.20F;
    outsideHandleTarget->width = 0.30F;
    outsideHandleTarget->height = 0.40F;
    outsideHandleSource->visible = true;
    outsideHandleSource->x = 0.05F;
    outsideHandleSource->y = 0.20F;
    outsideHandleSource->width = 0.15F;
    outsideHandleSource->height = 0.40F;
    Check(outsideHandleShrink.CollapseToModule(
              ModuleId::AllMusic, ModuleId::Rivan,
              rivan::ui::ModuleCollapseSide::Right,
              rivan::ui::ModuleCollapseMode::Outside) &&
              outsideHandleShrink.PreservePixelGeometry(
                  1000.0F, 800.0F, 650.0F, 800.0F, true),
          "shrinking canvas preserves an outside module-collapse handle");
    outsideHandleTarget = outsideHandleShrink.Find(ModuleId::Rivan);
    outsideHandleSource = outsideHandleShrink.Find(ModuleId::AllMusic);
    Check(outsideHandleTarget != nullptr && outsideHandleSource != nullptr &&
              outsideHandleSource->collapsed &&
              std::abs(outsideHandleSource->handleX -
                       (outsideHandleTarget->x + outsideHandleTarget->width)) < 0.001F &&
              std::abs(outsideHandleSource->handleX + outsideHandleSource->handleWidth -
                       1.0F) < 0.001F &&
              outsideHandleTarget->x + outsideHandleTarget->width < 1.0F - 0.001F,
          "outside handle reserves canvas space instead of being clamped into its target");

    auto windowHandleShrink = ModuleLayout::Defaults();
    for (auto& item : windowHandleShrink.items) item.visible = false;
    auto* windowHandleSource = windowHandleShrink.Find(ModuleId::Rivan);
    windowHandleSource->visible = true;
    windowHandleSource->x = 0.30F;
    windowHandleSource->y = 0.20F;
    windowHandleSource->width = 0.30F;
    windowHandleSource->height = 0.40F;
    Check(windowHandleShrink.CollapseToWindow(ModuleId::Rivan,
                                              rivan::ui::ModuleCollapseSide::Right) &&
              windowHandleShrink.PreservePixelGeometry(
                  1000.0F, 800.0F, 650.0F, 800.0F, true),
          "shrinking canvas preserves a window-edge collapse handle");
    windowHandleSource = windowHandleShrink.Find(ModuleId::Rivan);
    Check(windowHandleSource != nullptr && windowHandleSource->collapseTargetIsWindow &&
              std::abs(windowHandleSource->x + windowHandleSource->width - 1.0F) < 0.001F,
          "window-edge collapse remains attached to the application edge");

    auto minimizedResize = ModuleLayout::Defaults();
    const auto beforeMinimize = minimizedResize;
    Check(!minimizedResize.PreservePixelGeometry(1000.0F, 800.0F, 1.0F, 1.0F),
          "a minimized canvas cannot preserve module pixel geometry");
    Check(std::abs(minimizedResize.Find(ModuleId::Rivan)->width -
                   beforeMinimize.Find(ModuleId::Rivan)->width) < 0.0001F &&
              std::abs(minimizedResize.Find(ModuleId::Rivan)->height -
                       beforeMinimize.Find(ModuleId::Rivan)->height) < 0.0001F,
          "failed pixel preservation leaves layout unchanged");

    auto expansion = ModuleLayout::Defaults();
    for (auto& item : expansion.items) item.visible = false;
    auto* expandable = expansion.Find(ModuleId::Rivan);
    auto* blocker = expansion.Find(ModuleId::AllMusic);
    expandable->visible = true;
    expandable->x = 0.0F;
    expandable->y = 0.0F;
    expandable->width = 0.50F;
    expandable->height = 1.0F;
    blocker->visible = true;
    blocker->x = 0.50F;
    blocker->y = 0.0F;
    blocker->width = 0.50F;
    blocker->height = 1.0F;
    Check(expansion.CollapseToWindow(ModuleId::Rivan, rivan::ui::ModuleCollapseSide::Left),
          "an edge module can collapse before overflow expansion tests");
    blocker = expansion.Find(ModuleId::AllMusic);
    blocker->x = 0.25F;
    blocker->width = 0.50F;
    Check(expansion.ToggleCollapsedModule(ModuleId::Rivan,
                                          rivan::ui::ModuleExpansionBehavior::Squash,
                                          1000.0F, 800.0F) &&
              !expansion.HasConflictingGeometry() &&
              expansion.Find(ModuleId::AllMusic)->x >= 0.50F,
          "squash expansion relocates blocking modules instead of refusing to open");

    auto resizeExpansion = ModuleLayout::Defaults();
    for (auto& item : resizeExpansion.items) item.visible = false;
    expandable = resizeExpansion.Find(ModuleId::Rivan);
    blocker = resizeExpansion.Find(ModuleId::AllMusic);
    expandable->visible = true;
    expandable->x = 0.0F;
    expandable->y = 0.0F;
    expandable->width = 0.50F;
    expandable->height = 1.0F;
    blocker->visible = true;
    blocker->x = 0.50F;
    blocker->y = 0.0F;
    blocker->width = 0.50F;
    blocker->height = 1.0F;
    Check(resizeExpansion.CollapseToWindow(ModuleId::Rivan, rivan::ui::ModuleCollapseSide::Left),
          "resize behavior test can prepare an edge collapse");
    blocker = resizeExpansion.Find(ModuleId::AllMusic);
    blocker->x = 0.25F;
    blocker->width = 0.50F;
    float expandedWidth = 0.0F;
    float expandedHeight = 0.0F;
    Check(resizeExpansion.ToggleCollapsedModule(ModuleId::Rivan,
                                                rivan::ui::ModuleExpansionBehavior::Resize,
                                                1000.0F, 800.0F,
                                                &expandedWidth, &expandedHeight) &&
              expandedWidth > 1000.0F &&
              std::abs(resizeExpansion.Find(ModuleId::Rivan)->width * expandedWidth - 500.0F) < 0.01F &&
              std::abs(resizeExpansion.Find(ModuleId::AllMusic)->width * expandedWidth - 500.0F) < 0.01F &&
              !resizeExpansion.HasConflictingGeometry(),
          "resize expansion grows the canvas while preserving module pixel widths");

    const auto prepareShrunkCollapsed = [] {
        auto layout = ModuleLayout::Defaults();
        for (auto& item : layout.items) item.visible = false;
        auto* target = layout.Find(ModuleId::Rivan);
        auto* source = layout.Find(ModuleId::AllMusic);
        target->visible = true;
        target->x = 0.0F;
        target->y = 0.0F;
        target->width = 0.10F;
        target->height = 1.0F;
        source->visible = true;
        source->x = 0.10F;
        source->y = 0.20F;
        source->width = 0.80F;
        source->height = 0.40F;
        Check(layout.CollapseToModule(ModuleId::AllMusic, ModuleId::Rivan,
                                      rivan::ui::ModuleCollapseSide::Right,
                                      rivan::ui::ModuleCollapseMode::Outside),
              "an outside collapse can retain geometry beyond a smaller canvas");
        Check(layout.PreservePixelGeometry(1000.0F, 800.0F, 700.0F, 800.0F),
              "shrinking canvas preserves collapsed expansion geometry");
        const auto* collapsed = layout.Find(ModuleId::AllMusic);
        Check(collapsed != nullptr && collapsed->expandedWidth > 1.0F &&
                  std::abs(collapsed->expandedWidth * 700.0F - 800.0F) < 0.01F,
              "collapsed expansion retains its pixel width outside normalized canvas");
        return layout;
    };

    auto squashExpansion = prepareShrunkCollapsed();
    float squashWidth = 0.0F;
    float squashHeight = 0.0F;
    Check(squashExpansion.ToggleCollapsedModule(
              ModuleId::AllMusic, rivan::ui::ModuleExpansionBehavior::Squash,
              700.0F, 800.0F, &squashWidth, &squashHeight) &&
              squashWidth > 700.0F &&
              std::abs(squashExpansion.Find(ModuleId::AllMusic)->width * squashWidth - 800.0F) < 0.01F,
          "overflow expansion grows canvas despite squash behavior");

    auto resizeExpansionAfterShrink = prepareShrunkCollapsed();
    float resizeWidthAfterShrink = 0.0F;
    float resizeHeightAfterShrink = 0.0F;
    Check(resizeExpansionAfterShrink.ToggleCollapsedModule(
              ModuleId::AllMusic, rivan::ui::ModuleExpansionBehavior::Resize,
              700.0F, 800.0F, &resizeWidthAfterShrink, &resizeHeightAfterShrink) &&
              resizeWidthAfterShrink > 700.0F &&
              std::abs(resizeExpansionAfterShrink.Find(ModuleId::AllMusic)->width *
                       resizeWidthAfterShrink - 800.0F) < 0.01F,
          "overflow expansion grows canvas despite resize behavior");
}

void TestIniMetaFormat() {
    using rivan::core::IniDocument;

    // format=1 -> HasMetaFormat("1") true, HasMetaFormat("2") false
    {
        auto doc = IniDocument::Parse("[meta]\nformat=1");
        Check(doc.has_value(), "INI parse with meta.format=1 succeeds");
        if (doc) {
            Check(doc->HasMetaFormat("1"), "HasMetaFormat(\"1\") true for format=1");
            Check(!doc->HasMetaFormat("2"), "HasMetaFormat(\"2\") false for format=1");
        }
    }

    // format=2 -> HasMetaFormat("1") false
    {
        auto doc = IniDocument::Parse("[meta]\nformat=2");
        Check(doc.has_value(), "INI parse with meta.format=2 succeeds");
        if (doc) {
            Check(!doc->HasMetaFormat("1"), "HasMetaFormat(\"1\") false for format=2");
        }
    }

    // No meta section -> HasMetaFormat("1") false
    {
        auto doc = IniDocument::Parse("[other]\nkey=value");
        Check(doc.has_value(), "INI parse without meta section succeeds");
        if (doc) {
            Check(!doc->HasMetaFormat("1"), "HasMetaFormat(\"1\") false when meta section missing");
        }
    }

    // meta section exists but no format key -> HasMetaFormat("1") false
    {
        auto doc = IniDocument::Parse("[meta]\ncount=5");
        Check(doc.has_value(), "INI parse with meta section but no format key succeeds");
        if (doc) {
            Check(!doc->HasMetaFormat("1"), "HasMetaFormat(\"1\") false when meta.format missing");
        }
    }
}

void TestIniValueCodec() {
    const std::string value = "folder name=100%";
    const auto encoded = rivan::core::EncodeIniValue(value);
    Check(encoded == "folder%20name%3D100%25",
          "INI codec percent-encodes reserved bytes");
    Check(rivan::core::DecodeIniValue(encoded) == value,
          "INI codec restores encoded UTF-8 values");
    Check(!rivan::core::DecodeIniValue("%Q0"),
          "INI codec rejects malformed percent escapes");
    Check(!rivan::core::DecodeIniValue("%FF"),
          "strict INI codec rejects invalid UTF-8");
    Check(rivan::core::DecodeIniValue("%FF", false) == std::string{"\xFF", 1},
          "relaxed INI codec preserves legacy playlist bytes");
}

void TestCollapsibleSnapping() {
    using rivan::ui::ModuleCollapseMode;
    using rivan::ui::ModuleCollapseSide;
    using rivan::ui::ModuleDropZone;
    using rivan::ui::ModuleId;
    using rivan::ui::ModuleLayout;
    using rivan::ui::ModuleWindowDropZone;

    auto window = ModuleLayout::Defaults();
    for (auto& item : window.items) item.visible = false;
    window.Find(ModuleId::Rivan)->visible = true;
    window.Find(ModuleId::Rivan)->x = 0.20F;
    window.Find(ModuleId::Rivan)->y = 0.20F;
    window.Find(ModuleId::Rivan)->width = 0.40F;
    window.Find(ModuleId::Rivan)->height = 0.40F;
    Check(window.CollapseToWindow(ModuleId::Rivan, ModuleCollapseSide::Right),
          "a module can collapse against the right window edge");
    Check(window.IsCollapsed(ModuleId::Rivan) &&
              window.Find(ModuleId::Rivan)->width < 0.10F &&
              window.Find(ModuleId::Rivan)->collapseTargetIsWindow,
          "window collapse stores a narrow edge handle");
    Check(window.Find(ModuleId::Rivan)->handleWidth < window.Find(ModuleId::Rivan)->handleHeight,
          "left and right window collapse handles use vertical geometry");
    Check(window.ToggleCollapsedModule(ModuleId::Rivan) &&
              !window.IsCollapsed(ModuleId::Rivan) &&
              std::abs(window.Find(ModuleId::Rivan)->x - 0.60F) < 0.001F &&
              std::abs(window.Find(ModuleId::Rivan)->width - 0.40F) < 0.001F,
          "clicking a collapse handle restores the expanded rectangle");
    Check(window.CollapseToWindow(ModuleId::Rivan, ModuleCollapseSide::Left) &&
              window.Find(ModuleId::Rivan)->handleWidth < window.Find(ModuleId::Rivan)->handleHeight,
          "a left-edge collapse also stores vertical handle geometry");

    auto positionedWindow = ModuleLayout::Defaults();
    for (auto& item : positionedWindow.items) item.visible = false;
    auto* positionedWindowItem = positionedWindow.Find(ModuleId::Rivan);
    positionedWindowItem->visible = true;
    positionedWindowItem->x = 0.20F;
    positionedWindowItem->y = 0.20F;
    positionedWindowItem->width = 0.40F;
    positionedWindowItem->height = 0.30F;
    Check(positionedWindow.CollapseToWindow(ModuleId::Rivan, ModuleCollapseSide::Right, 0.75F),
          "window-edge collapse accepts a requested edge position");
    Check(std::abs(positionedWindowItem->expandedY - 0.60F) < 0.001F,
          "window-edge collapse follows pointer along edge");

    auto collapsedTarget = ModuleLayout::Defaults();
    for (auto& item : collapsedTarget.items) item.visible = false;
    collapsedTarget.Find(ModuleId::Rivan)->visible = true;
    collapsedTarget.Find(ModuleId::Rivan)->x = 0.5F;
    collapsedTarget.Find(ModuleId::Rivan)->width = 0.5F;
    collapsedTarget.Find(ModuleId::Rivan)->height = 1.0F;
    Check(collapsedTarget.CollapseToWindow(ModuleId::Rivan, ModuleCollapseSide::Right),
          "a collapsed target can be prepared for snap filtering");
    collapsedTarget.Find(ModuleId::AllMusic)->visible = true;
    Check(!collapsedTarget.SnapTo(ModuleId::AllMusic, ModuleId::Rivan, ModuleDropZone::Right),
          "collapsed handles are not treated as normal snap targets");

    auto inside = ModuleLayout::Defaults();
    for (auto& item : inside.items) item.visible = false;
    inside.Find(ModuleId::Rivan)->visible = true;
    inside.Find(ModuleId::Rivan)->x = 0.0F;
    inside.Find(ModuleId::Rivan)->y = 0.0F;
    inside.Find(ModuleId::Rivan)->width = 0.50F;
    inside.Find(ModuleId::Rivan)->height = 1.0F;
    inside.Find(ModuleId::AllMusic)->visible = true;
    inside.Find(ModuleId::AllMusic)->x = 0.50F;
    inside.Find(ModuleId::AllMusic)->y = 0.0F;
    inside.Find(ModuleId::AllMusic)->width = 0.50F;
    inside.Find(ModuleId::AllMusic)->height = 1.0F;
    Check(inside.CollapseToModule(ModuleId::AllMusic, ModuleId::Rivan,
                                  ModuleCollapseSide::Right, ModuleCollapseMode::Inside),
          "a module can collapse inside the right side of another module");
    Check(inside.IsCollapsed(ModuleId::AllMusic) &&
              inside.Find(ModuleId::AllMusic)->collapseTarget == ModuleId::Rivan &&
              inside.Find(ModuleId::AllMusic)->collapseMode == ModuleCollapseMode::Inside,
          "inside collapse records its target and mode");
    Check(std::abs(inside.Find(ModuleId::AllMusic)->handleWidth - 0.12F) < 0.001F &&
              std::abs(inside.Find(ModuleId::AllMusic)->handleHeight - 0.20F) < 0.001F,
          "inside collapse stores compact handle thickness and target-relative length");
    Check(std::abs(inside.Find(ModuleId::Rivan)->x + inside.Find(ModuleId::Rivan)->width -
                   inside.Find(ModuleId::AllMusic)->handleX) < 0.001F,
          "inside collapse keeps its arrow on the target edge");
    Check(inside.ToggleCollapsedModule(ModuleId::AllMusic) &&
              !inside.IsCollapsed(ModuleId::AllMusic) &&
              std::abs(inside.Find(ModuleId::AllMusic)->x - 0.25F) < 0.001F &&
              std::abs(inside.Find(ModuleId::Rivan)->width - 0.13F) < 0.001F &&
              !inside.HasConflictingGeometry(),
          "inside collapse expands without an artificial target gap or overlap");
    Check(inside.ToggleCollapsedModule(ModuleId::AllMusic) &&
              std::abs(inside.Find(ModuleId::Rivan)->x + inside.Find(ModuleId::Rivan)->width -
                       inside.Find(ModuleId::AllMusic)->handleX) < 0.001F,
          "re-collapsing restores target space while retaining its edge handle");

    auto insideTop = ModuleLayout::Defaults();
    for (auto& item : insideTop.items) item.visible = false;
    insideTop.Find(ModuleId::Rivan)->visible = true;
    insideTop.Find(ModuleId::Rivan)->x = 0.0F;
    insideTop.Find(ModuleId::Rivan)->y = 0.0F;
    insideTop.Find(ModuleId::Rivan)->width = 1.0F;
    insideTop.Find(ModuleId::Rivan)->height = 0.50F;
    insideTop.Find(ModuleId::AllMusic)->visible = true;
    insideTop.Find(ModuleId::AllMusic)->x = 0.0F;
    insideTop.Find(ModuleId::AllMusic)->y = 0.50F;
    insideTop.Find(ModuleId::AllMusic)->width = 1.0F;
    insideTop.Find(ModuleId::AllMusic)->height = 0.50F;
    Check(insideTop.CollapseToModule(ModuleId::AllMusic, ModuleId::Rivan,
                                     ModuleCollapseSide::Top, ModuleCollapseMode::Inside) &&
              insideTop.ToggleCollapsedModule(ModuleId::AllMusic) &&
              std::abs(insideTop.Find(ModuleId::Rivan)->y - 0.37F) < 0.001F &&
              std::abs(insideTop.Find(ModuleId::AllMusic)->height - 0.25F) < 0.001F &&
              !insideTop.HasConflictingGeometry(),
          "top inside collapse expands without an artificial target gap or overlap");

    auto outside = ModuleLayout::Defaults();
    for (auto& item : outside.items) item.visible = false;
    outside.Find(ModuleId::Rivan)->visible = true;
    outside.Find(ModuleId::Rivan)->x = 0.30F;
    outside.Find(ModuleId::Rivan)->y = 0.20F;
    outside.Find(ModuleId::Rivan)->width = 0.30F;
    outside.Find(ModuleId::Rivan)->height = 0.30F;
    outside.Find(ModuleId::AllMusic)->visible = true;
    outside.Find(ModuleId::AllMusic)->x = 0.0F;
    outside.Find(ModuleId::AllMusic)->y = 0.20F;
    outside.Find(ModuleId::AllMusic)->width = 0.25F;
    outside.Find(ModuleId::AllMusic)->height = 0.30F;
    Check(outside.CollapseToModule(ModuleId::AllMusic, ModuleId::Rivan,
                                   ModuleCollapseSide::Right, ModuleCollapseMode::Outside),
          "an outside collapse can reserve adjacent space");
    Check(outside.Find(ModuleId::AllMusic)->x > outside.Find(ModuleId::Rivan)->x +
                outside.Find(ModuleId::Rivan)->width - 0.001F &&
               std::abs(outside.Find(ModuleId::AllMusic)->handleX -
                        (outside.Find(ModuleId::Rivan)->x + outside.Find(ModuleId::Rivan)->width)) < 0.001F &&
               !outside.HasConflictingGeometry(),
          "outside collapse places its handle beside the target without overlap");

    const auto prepareOutsideResize = [](ModuleCollapseSide side) {
        auto layout = ModuleLayout::Defaults();
        for (auto& item : layout.items) item.visible = false;
        auto* target = layout.Find(ModuleId::Rivan);
        auto* source = layout.Find(ModuleId::AllMusic);
        target->visible = true;
        target->x = 0.30F;
        target->y = 0.30F;
        target->width = 0.30F;
        target->height = 0.30F;
        source->visible = true;
        source->x = 0.05F;
        source->y = 0.05F;
        source->width = 0.15F;
        source->height = 0.15F;
        Check(layout.CollapseToModule(ModuleId::AllMusic, ModuleId::Rivan, side,
                                      ModuleCollapseMode::Outside),
              "outside collapse prepares target resize attachment test");
        return layout;
    };
    const auto checkOutsideAttachment = [](const ModuleLayout& layout,
                                           ModuleCollapseSide side,
                                           const char* message) {
        const auto* target = layout.Find(ModuleId::Rivan);
        const auto* source = layout.Find(ModuleId::AllMusic);
        if (!target || !source) {
            Check(false, message);
            return;
        }
        const float targetRight = target->x + target->width;
        const float targetBottom = target->y + target->height;
        const float handleRight = source->handleX + source->handleWidth;
        const float handleBottom = source->handleY + source->handleHeight;
        const bool attached = side == ModuleCollapseSide::Left
            ? std::abs(handleRight - target->x) < 0.001F && handleRight <= target->x + 0.001F
            : side == ModuleCollapseSide::Right
                ? std::abs(source->handleX - targetRight) < 0.001F &&
                      source->handleX >= targetRight - 0.001F
                : side == ModuleCollapseSide::Top
                    ? std::abs(handleBottom - target->y) < 0.001F &&
                          handleBottom <= target->y + 0.001F
                    : std::abs(source->handleY - targetBottom) < 0.001F &&
                          source->handleY >= targetBottom - 0.001F;
        Check(source->collapsed && attached, message);
    };

    auto outsideSnapTo = prepareOutsideResize(ModuleCollapseSide::Right);
    auto* snapToSource = outsideSnapTo.Find(ModuleId::GraphicEqualizer);
    snapToSource->visible = true;
    snapToSource->x = 0.05F;
    snapToSource->y = 0.05F;
    snapToSource->width = 0.15F;
    snapToSource->height = 0.15F;
    const float snapToTargetRight = outsideSnapTo.Find(ModuleId::Rivan)->x +
        outsideSnapTo.Find(ModuleId::Rivan)->width;
    Check(outsideSnapTo.SnapTo(ModuleId::GraphicEqualizer, ModuleId::Rivan,
                               ModuleDropZone::Right),
          "side snapping can split an outside-collapse target");
    Check(outsideSnapTo.Find(ModuleId::Rivan)->x + outsideSnapTo.Find(ModuleId::Rivan)->width <
              snapToTargetRight - 0.001F,
          "side snapping changes the outside-collapse target edge");
    checkOutsideAttachment(outsideSnapTo, ModuleCollapseSide::Right,
                           "outside handle follows target after side snapping changes its edge");

    auto outsideWindowSplit = ModuleLayout::Defaults();
    for (auto& item : outsideWindowSplit.items) item.visible = false;
    auto* windowTarget = outsideWindowSplit.Find(ModuleId::Rivan);
    auto* windowCollapseSource = outsideWindowSplit.Find(ModuleId::AllMusic);
    auto* windowSnapSource = outsideWindowSplit.Find(ModuleId::GraphicEqualizer);
    windowTarget->visible = true;
    windowTarget->x = 0.40F;
    windowTarget->y = 0.20F;
    windowTarget->width = 0.40F;
    windowTarget->height = 0.60F;
    windowCollapseSource->visible = true;
    windowCollapseSource->x = 0.05F;
    windowCollapseSource->y = 0.05F;
    windowCollapseSource->width = 0.15F;
    windowCollapseSource->height = 0.15F;
    windowSnapSource->visible = true;
    windowSnapSource->x = 0.05F;
    windowSnapSource->y = 0.70F;
    windowSnapSource->width = 0.15F;
    windowSnapSource->height = 0.15F;
    Check(outsideWindowSplit.CollapseToModule(ModuleId::AllMusic, ModuleId::Rivan,
                                              ModuleCollapseSide::Right,
                                              ModuleCollapseMode::Outside),
          "outside collapse prepares window-split attachment test");
    const auto addWindowBlocker = [&outsideWindowSplit](ModuleId id, float x, float y,
                                                        float width, float height) {
        auto* item = outsideWindowSplit.Find(id);
        item->visible = true;
        item->x = x;
        item->y = y;
        item->width = width;
        item->height = height;
    };
    // Fill the right-middle drop region after collapse so SnapToWindow must split Rivan.
    addWindowBlocker(ModuleId::RivanLibrary, 0.50F, 0.00F, 0.50F, 0.20F);
    addWindowBlocker(ModuleId::VideoPreview, 0.50F, 0.80F, 0.50F, 0.20F);
    addWindowBlocker(ModuleId::Lyrics, 0.86F, 0.20F, 0.14F, 0.60F);
    const float windowSplitTargetRight = windowTarget->x + windowTarget->width;
    Check(outsideWindowSplit.SnapToWindow(ModuleId::GraphicEqualizer,
                                          ModuleWindowDropZone::RightMiddle,
                                          0.60F, 0.50F),
          "window snapping splits an outside-collapse target when no free rectangle exists");
    Check(outsideWindowSplit.Find(ModuleId::Rivan)->x +
              outsideWindowSplit.Find(ModuleId::Rivan)->width <
              windowSplitTargetRight - 0.001F,
          "window split changes the outside-collapse target edge");
    checkOutsideAttachment(outsideWindowSplit, ModuleCollapseSide::Right,
                           "outside handle follows target after window snapping splits its edge");

    auto outsideCrossAxis = ModuleLayout::Defaults();
    for (auto& item : outsideCrossAxis.items) item.visible = false;
    auto* crossAxisTarget = outsideCrossAxis.Find(ModuleId::Rivan);
    auto* crossAxisSource = outsideCrossAxis.Find(ModuleId::AllMusic);
    crossAxisTarget->visible = true;
    crossAxisTarget->x = 0.30F;
    crossAxisTarget->y = 0.30F;
    crossAxisTarget->width = 0.30F;
    crossAxisTarget->height = 0.30F;
    crossAxisSource->visible = true;
    crossAxisSource->x = 0.05F;
    crossAxisSource->y = 0.05F;
    crossAxisSource->width = 0.15F;
    crossAxisSource->height = 0.15F;
    Check(outsideCrossAxis.CollapseToModule(ModuleId::AllMusic, ModuleId::Rivan,
                                             ModuleCollapseSide::Right,
                                             ModuleCollapseMode::Outside,
                                             0.12F, 0.95F) &&
              outsideCrossAxis.ResizeModule(ModuleId::Rivan, 0.45F, 0.95F,
                                             false, true, false, false, false),
          "cross-axis target resize applies to an outside collapse near canvas edge");
    checkOutsideAttachment(outsideCrossAxis, ModuleCollapseSide::Right,
                           "cross-axis resize keeps outside handle on target edge");
    crossAxisSource = outsideCrossAxis.Find(ModuleId::AllMusic);
    Check(crossAxisSource->expandedY >= -0.001F &&
              crossAxisSource->expandedY + crossAxisSource->expandedHeight <= 1.001F &&
              outsideCrossAxis.ToggleCollapsedModule(ModuleId::AllMusic),
          "cross-axis resize clamps outside expansion and still opens collapse");

    auto outsideRightResize = prepareOutsideResize(ModuleCollapseSide::Right);
    Check(outsideRightResize.ResizeModule(ModuleId::Rivan, 0.75F, 0.45F,
                                           true, false, false, false, false),
          "outside right target resize applies with overlap mode");
    checkOutsideAttachment(outsideRightResize, ModuleCollapseSide::Right,
                           "outside right handle follows target right edge during overlap resize");

    auto outsideLeftResize = prepareOutsideResize(ModuleCollapseSide::Left);
    Check(outsideLeftResize.ResizeModule(ModuleId::Rivan, 0.15F, 0.45F,
                                          false, false, true, false, false),
          "outside left target resize applies");
    checkOutsideAttachment(outsideLeftResize, ModuleCollapseSide::Left,
                           "outside left handle follows target left edge during resize");

    auto outsideTopResize = prepareOutsideResize(ModuleCollapseSide::Top);
    Check(outsideTopResize.ResizeModule(ModuleId::Rivan, 0.45F, 0.15F,
                                         false, false, false, true, false),
          "outside top target resize applies");
    checkOutsideAttachment(outsideTopResize, ModuleCollapseSide::Top,
                           "outside top handle follows target top edge during resize");

    auto outsideBottomResize = prepareOutsideResize(ModuleCollapseSide::Bottom);
    Check(outsideBottomResize.ResizeModule(ModuleId::Rivan, 0.45F, 0.75F,
                                            false, true, false, false, false),
          "outside bottom target resize applies");
    checkOutsideAttachment(outsideBottomResize, ModuleCollapseSide::Bottom,
                           "outside bottom handle follows target bottom edge during resize");

    auto outsideMove = prepareOutsideResize(ModuleCollapseSide::Right);
    outsideMove.ClearInsideCollapseReferences(ModuleId::Rivan);
    const auto outsideMoveBefore = outsideMove;
    auto* movedTarget = outsideMove.Find(ModuleId::Rivan);
    movedTarget->x += 0.12F;
    movedTarget->y += 0.10F;
    ModuleLayout::SyncExpandedGeometry(*movedTarget);
    Check(outsideMove.ReattachOutsideCollapseHandles(outsideMoveBefore),
          "outside handle reattachment applies after target movement");
    checkOutsideAttachment(outsideMove, ModuleCollapseSide::Right,
                           "outside handle remains attached when target moves");
    Check(outsideMove.Find(ModuleId::AllMusic)->collapsed &&
              outsideMove.Find(ModuleId::AllMusic)->collapseMode == ModuleCollapseMode::Outside,
          "moving a target preserves its outside collapse reference");

    auto outsideSnapGroupMove = ModuleLayout::Defaults();
    for (auto& item : outsideSnapGroupMove.items) item.visible = false;
    auto* movingSnapTarget = outsideSnapGroupMove.Find(ModuleId::Rivan);
    auto* movingSnapPeer = outsideSnapGroupMove.Find(ModuleId::GraphicEqualizer);
    auto* movingSnapSource = outsideSnapGroupMove.Find(ModuleId::AllMusic);
    movingSnapTarget->visible = true;
    movingSnapTarget->x = 0.30F;
    movingSnapTarget->y = 0.30F;
    movingSnapTarget->width = 0.30F;
    movingSnapTarget->height = 0.30F;
    movingSnapPeer->visible = true;
    movingSnapPeer->x = 0.05F;
    movingSnapPeer->y = 0.05F;
    movingSnapPeer->width = 0.15F;
    movingSnapPeer->height = 0.15F;
    movingSnapSource->visible = true;
    movingSnapSource->x = 0.05F;
    movingSnapSource->y = 0.05F;
    movingSnapSource->width = 0.15F;
    movingSnapSource->height = 0.15F;
    Check(outsideSnapGroupMove.SnapTo(ModuleId::GraphicEqualizer, ModuleId::Rivan,
                                      ModuleDropZone::Left) &&
              outsideSnapGroupMove.CollapseToModule(
                  ModuleId::AllMusic, ModuleId::Rivan, ModuleCollapseSide::Right,
                  ModuleCollapseMode::Outside),
          "outside collapse prepares snap-group movement attachment test");
    const auto outsideSnapGroupMoveBefore = outsideSnapGroupMove;
    for (auto& item : outsideSnapGroupMove.items) {
        if (outsideSnapGroupMove.SnapRoot(item.id) == ModuleId::Rivan) {
            item.x += 0.10F;
            item.y += 0.08F;
            ModuleLayout::SyncExpandedGeometry(item);
        }
    }
    Check(outsideSnapGroupMove.ReattachOutsideCollapseHandles(outsideSnapGroupMoveBefore),
          "outside handle reattachment applies after snap-group movement");
    checkOutsideAttachment(outsideSnapGroupMove, ModuleCollapseSide::Right,
                           "outside handle remains attached when target snap group moves");

    Check(outsideRightResize.ToggleCollapsedModule(ModuleId::AllMusic),
          "resized outside module can open");
    Check(outsideRightResize.ResizeModule(ModuleId::Rivan, 0.80F, 0.45F,
                                           true, false, false, false, false) &&
              outsideRightResize.ToggleCollapsedModule(ModuleId::AllMusic),
          "opened outside module can re-collapse after its target moves");
    checkOutsideAttachment(outsideRightResize, ModuleCollapseSide::Right,
                           "re-collapsing uses current target edge instead of stale handle position");

    auto outsideSnapGroupResize = ModuleLayout::Defaults();
    for (auto& item : outsideSnapGroupResize.items) item.visible = false;
    auto* snapTarget = outsideSnapGroupResize.Find(ModuleId::Rivan);
    auto* snapPeer = outsideSnapGroupResize.Find(ModuleId::GraphicEqualizer);
    auto* snapSource = outsideSnapGroupResize.Find(ModuleId::AllMusic);
    snapTarget->visible = true;
    snapTarget->x = 0.30F;
    snapTarget->y = 0.30F;
    snapTarget->width = 0.30F;
    snapTarget->height = 0.30F;
    snapPeer->visible = true;
    snapPeer->x = 0.05F;
    snapPeer->y = 0.05F;
    snapPeer->width = 0.15F;
    snapPeer->height = 0.15F;
    snapSource->visible = true;
    snapSource->x = 0.05F;
    snapSource->y = 0.05F;
    snapSource->width = 0.15F;
    snapSource->height = 0.15F;
    Check(outsideSnapGroupResize.SnapTo(ModuleId::GraphicEqualizer, ModuleId::Rivan,
                                        ModuleDropZone::Left) &&
              outsideSnapGroupResize.CollapseToModule(
                  ModuleId::AllMusic, ModuleId::Rivan, ModuleCollapseSide::Right,
                  ModuleCollapseMode::Outside) &&
              outsideSnapGroupResize.ResizeSnapGroup(
                  ModuleId::Rivan, 0.75F, 0.45F, true, false, false, false, false),
          "outside collapse target can resize with its snap group in overlap mode");
    checkOutsideAttachment(outsideSnapGroupResize, ModuleCollapseSide::Right,
                           "outside handle follows its target edge when target snap group resizes");

    auto outsidePositioned = ModuleLayout::Defaults();
    for (auto& item : outsidePositioned.items) item.visible = false;
    auto* positionedTarget = outsidePositioned.Find(ModuleId::Rivan);
    auto* positionedSource = outsidePositioned.Find(ModuleId::AllMusic);
    positionedTarget->visible = true;
    positionedTarget->x = 0.20F;
    positionedTarget->y = 0.20F;
    positionedTarget->width = 0.40F;
    positionedTarget->height = 0.40F;
    positionedSource->visible = true;
    positionedSource->x = 0.0F;
    positionedSource->y = 0.0F;
    positionedSource->width = 0.35F;
    positionedSource->height = 0.30F;
    Check(outsidePositioned.CollapseToModule(ModuleId::AllMusic, ModuleId::Rivan,
                                             ModuleCollapseSide::Right,
                                             ModuleCollapseMode::Outside,
                                             0.12F, 0.74F),
          "an outside collapse accepts a requested target-edge position");
    positionedSource = outsidePositioned.Find(ModuleId::AllMusic);
    Check(std::abs(positionedSource->expandedY - 0.59F) < 0.001F &&
              std::abs(positionedSource->handleX - 0.60F) < 0.001F,
          "outside collapse follows the pointer along its target edge");

    auto outsideFit = ModuleLayout::Defaults();
    for (auto& item : outsideFit.items) item.visible = false;
    auto* fitTarget = outsideFit.Find(ModuleId::Rivan);
    auto* fitSource = outsideFit.Find(ModuleId::AllMusic);
    fitTarget->visible = true;
    fitTarget->x = 0.20F;
    fitTarget->y = 0.20F;
    fitTarget->width = 0.60F;
    fitTarget->height = 0.40F;
    fitSource->visible = true;
    fitSource->x = 0.0F;
    fitSource->y = 0.0F;
    fitSource->width = 0.35F;
    fitSource->height = 0.30F;
    Check(outsideFit.CollapseToModule(ModuleId::AllMusic, ModuleId::Rivan,
                                      ModuleCollapseSide::Right,
                                      ModuleCollapseMode::Outside),
          "outside collapse resizes a module to fit remaining target-side space");
    fitSource = outsideFit.Find(ModuleId::AllMusic);
    Check(std::abs(fitSource->expandedX - 0.80F) < 0.001F &&
              std::abs(fitSource->expandedWidth - 0.20F) < 0.001F,
          "outside collapse stores the fitted expanded geometry");

    auto resizedCollapsible = ModuleLayout::Defaults();
    for (auto& item : resizedCollapsible.items) item.visible = false;
    auto* collapsible = resizedCollapsible.Find(ModuleId::Rivan);
    collapsible->visible = true;
    collapsible->x = 0.20F;
    collapsible->y = 0.20F;
    collapsible->width = 0.40F;
    collapsible->height = 0.40F;
    Check(resizedCollapsible.CollapseToWindow(ModuleId::Rivan, ModuleCollapseSide::Right) &&
              resizedCollapsible.ToggleCollapsedModule(ModuleId::Rivan),
          "a collapsed module can be reopened before resizing");
    collapsible = resizedCollapsible.Find(ModuleId::Rivan);
    collapsible->x = 0.58F;
    collapsible->y = 0.12F;
    collapsible->width = 0.32F;
    collapsible->height = 0.46F;
    ModuleLayout::SyncExpandedGeometry(*collapsible);
    Check(resizedCollapsible.ToggleCollapsedModule(ModuleId::Rivan) &&
              resizedCollapsible.ToggleCollapsedModule(ModuleId::Rivan),
          "a resized collapsible module can be collapsed and reopened");
    collapsible = resizedCollapsible.Find(ModuleId::Rivan);
    Check(std::abs(collapsible->x - 0.58F) < 0.001F &&
              std::abs(collapsible->y - 0.12F) < 0.001F &&
              std::abs(collapsible->width - 0.32F) < 0.001F &&
              std::abs(collapsible->height - 0.46F) < 0.001F,
          "collapsible geometry survives a collapse and expand cycle after resizing");

    auto nestedResize = ModuleLayout::Defaults();
    for (auto& item : nestedResize.items) item.visible = false;
    auto* nestedTarget = nestedResize.Find(ModuleId::Rivan);
    nestedTarget->visible = true;
    nestedTarget->x = 0.0F;
    nestedTarget->y = 0.0F;
    nestedTarget->width = 0.50F;
    nestedTarget->height = 1.0F;
    auto* nestedSource = nestedResize.Find(ModuleId::AllMusic);
    nestedSource->visible = true;
    nestedSource->x = 0.50F;
    nestedSource->y = 0.0F;
    nestedSource->width = 0.50F;
    nestedSource->height = 1.0F;
    Check(nestedResize.CollapseToModule(ModuleId::AllMusic, ModuleId::Rivan,
                                        ModuleCollapseSide::Right,
                                        ModuleCollapseMode::Inside),
          "a target with an embedded collapsed module can be resized");
    const auto oldTargetBounds = rivan::ui::ModuleNormalizedRect{0.0F, 0.0F, 0.50F, 1.0F};
    const auto newTargetBounds = rivan::ui::ModuleNormalizedRect{0.0F, 0.0F, 0.40F, 1.0F};
    nestedResize.ScaleCollapsedInsideModules(ModuleId::Rivan, oldTargetBounds,
                                             newTargetBounds);
    nestedSource = nestedResize.Find(ModuleId::AllMusic);
    Check(nestedSource != nullptr &&
              std::abs(nestedSource->handleX + nestedSource->handleWidth - 0.40F) < 0.001F &&
              nestedSource->expandedX + nestedSource->expandedWidth <= 0.40F + 0.001F,
           "resizing a collapse target scales its nested handle and expanded bounds");

    auto insideTargetShrink = ModuleLayout::Defaults();
    for (auto& item : insideTargetShrink.items) item.visible = false;
    auto* shrinkTarget = insideTargetShrink.Find(ModuleId::Rivan);
    auto* firstInsideChild = insideTargetShrink.Find(ModuleId::AllMusic);
    auto* secondInsideChild = insideTargetShrink.Find(ModuleId::GraphicEqualizer);
    shrinkTarget->visible = true;
    shrinkTarget->x = 0.0F;
    shrinkTarget->y = 0.0F;
    shrinkTarget->width = 0.60F;
    shrinkTarget->height = 0.80F;
    firstInsideChild->visible = true;
    firstInsideChild->x = 0.60F;
    firstInsideChild->y = 0.0F;
    firstInsideChild->width = 0.20F;
    firstInsideChild->height = 0.80F;
    secondInsideChild->visible = true;
    secondInsideChild->x = 0.80F;
    secondInsideChild->y = 0.0F;
    secondInsideChild->width = 0.20F;
    secondInsideChild->height = 0.80F;
    Check(insideTargetShrink.CollapseToModule(
              ModuleId::AllMusic, ModuleId::Rivan, ModuleCollapseSide::Right,
              ModuleCollapseMode::Inside),
          "inside target shrink fixture creates first collapsed child");
    const auto oldFirstChild = *insideTargetShrink.Find(ModuleId::AllMusic);
    const auto oldShrinkTarget = ModuleLayout::Bounds(
        *insideTargetShrink.Find(ModuleId::Rivan));
    Check(insideTargetShrink.CollapseToModule(
              ModuleId::GraphicEqualizer, ModuleId::Rivan, ModuleCollapseSide::Right,
              ModuleCollapseMode::Inside),
          "inside target shrinks around an existing inside-collapsed child");
    firstInsideChild = insideTargetShrink.Find(ModuleId::AllMusic);
    const auto newShrinkTarget = ModuleLayout::Bounds(
        *insideTargetShrink.Find(ModuleId::Rivan));
    const float shrinkScaleX = (newShrinkTarget.right - newShrinkTarget.left) /
        (oldShrinkTarget.right - oldShrinkTarget.left);
    Check(firstInsideChild != nullptr &&
              std::abs(firstInsideChild->expandedX -
                       (newShrinkTarget.left +
                        (oldFirstChild.expandedX - oldShrinkTarget.left) * shrinkScaleX)) <
                  0.001F &&
              std::abs(firstInsideChild->handleX -
                       (newShrinkTarget.left +
                        (oldFirstChild.handleX - oldShrinkTarget.left) * shrinkScaleX)) <
                  0.001F,
          "inside target shrink re-anchors existing child geometry");

    auto tabbedCollapse = ModuleLayout::Defaults();
    for (auto& item : tabbedCollapse.items) item.visible = false;
    auto* tabbedRoot = tabbedCollapse.Find(ModuleId::Rivan);
    auto* tabbedChild = tabbedCollapse.Find(ModuleId::AllMusic);
    tabbedRoot->visible = true;
    tabbedRoot->x = 0.0F;
    tabbedRoot->y = 0.0F;
    tabbedRoot->width = 0.50F;
    tabbedRoot->height = 1.0F;
    tabbedChild->visible = false;
    tabbedChild->x = 0.0F;
    tabbedChild->y = 0.0F;
    tabbedChild->width = 0.50F;
    tabbedChild->height = 1.0F;
    Check(tabbedCollapse.CollapseToWindow(ModuleId::Rivan, ModuleCollapseSide::Left) &&
              tabbedCollapse.ToggleCollapsedModule(ModuleId::Rivan),
          "tab collapse fixture restores a previously collapsible root");
    tabbedChild->visible = true;
    tabbedCollapse.MakeTab(ModuleId::Rivan, ModuleId::AllMusic);
    Check(tabbedCollapse.ToggleCollapsedModule(ModuleId::Rivan) &&
              tabbedCollapse.IsEffectivelyCollapsed(ModuleId::AllMusic) &&
              tabbedCollapse.IsCollapseHandleVisible(ModuleId::Rivan),
          "collapsing a tab root hides whole tab group but keeps one handle");
    const bool tabbedExpanded = tabbedCollapse.ToggleCollapsedModule(ModuleId::Rivan);
    Check(tabbedExpanded, "expanding a tab root succeeds");
    Check(!tabbedCollapse.IsEffectivelyCollapsed(ModuleId::AllMusic),
          "expanding a tab root clears effective collapse");
    Check(std::abs(tabbedCollapse.Find(ModuleId::AllMusic)->width - 0.50F) < 0.001F,
          "expanding a tab root restores every tab geometry");

    auto nestedCollapse = ModuleLayout::Defaults();
    for (auto& item : nestedCollapse.items) item.visible = false;
    auto* nestedParent = nestedCollapse.Find(ModuleId::Rivan);
    auto* nestedChildCollapse = nestedCollapse.Find(ModuleId::AllMusic);
    nestedParent->visible = true;
    nestedParent->x = 0.0F;
    nestedParent->y = 0.0F;
    nestedParent->width = 0.50F;
    nestedParent->height = 1.0F;
    nestedChildCollapse->visible = true;
    nestedChildCollapse->x = 0.50F;
    nestedChildCollapse->y = 0.0F;
    nestedChildCollapse->width = 0.50F;
    nestedChildCollapse->height = 1.0F;
    Check(nestedCollapse.CollapseToWindow(ModuleId::Rivan, ModuleCollapseSide::Left) &&
              nestedCollapse.ToggleCollapsedModule(ModuleId::Rivan) &&
              nestedCollapse.CollapseToModule(ModuleId::AllMusic, ModuleId::Rivan,
                                               ModuleCollapseSide::Right,
                                               ModuleCollapseMode::Inside) &&
              nestedCollapse.ToggleCollapsedModule(ModuleId::Rivan) &&
              !nestedCollapse.IsCollapseHandleVisible(ModuleId::AllMusic),
          "parent collapse hides nested child handle");
    Check(nestedCollapse.ToggleCollapsedModule(ModuleId::Rivan) &&
              nestedCollapse.IsCollapseHandleVisible(ModuleId::AllMusic),
          "parent expansion restores nested child handle");

    auto snappedCollapse = ModuleLayout::Defaults();
    for (auto& item : snappedCollapse.items) item.visible = false;
    auto* snappedParent = snappedCollapse.Find(ModuleId::Rivan);
    auto* snappedChild = snappedCollapse.Find(ModuleId::AllMusic);
    snappedParent->visible = true;
    snappedParent->x = 0.0F;
    snappedParent->y = 0.0F;
    snappedParent->width = 0.50F;
    snappedParent->height = 1.0F;
    snappedChild->visible = true;
    snappedChild->x = 0.50F;
    snappedChild->y = 0.0F;
    snappedChild->width = 0.50F;
    snappedChild->height = 1.0F;
    Check(snappedCollapse.CollapseToWindow(ModuleId::Rivan, ModuleCollapseSide::Left) &&
              snappedCollapse.ToggleCollapsedModule(ModuleId::Rivan) &&
              snappedCollapse.SnapTo(ModuleId::AllMusic, ModuleId::Rivan,
                                    ModuleDropZone::Right) &&
              snappedCollapse.ToggleCollapsedModule(ModuleId::Rivan) &&
              snappedCollapse.IsEffectivelyCollapsed(ModuleId::AllMusic),
          "collapsing a snapped root hides complete snap group");
    Check(snappedCollapse.ToggleCollapsedModule(ModuleId::Rivan) &&
              !snappedCollapse.IsEffectivelyCollapsed(ModuleId::AllMusic),
          "expanding a snapped root restores complete snap group");
    Check(std::abs(snappedCollapse.Find(ModuleId::AllMusic)->width - 0.25F) < 0.001F &&
              std::abs(snappedCollapse.Find(ModuleId::Rivan)->width - 0.25F) < 0.001F,
          "snapped peer geometry survives a collapse and expand cycle");

    auto cyclicCollapse = ModuleLayout::Defaults();
    for (auto& item : cyclicCollapse.items) item.visible = false;
    auto* firstCycleMember = cyclicCollapse.Find(ModuleId::Rivan);
    auto* secondCycleMember = cyclicCollapse.Find(ModuleId::AllMusic);
    firstCycleMember->visible = true;
    firstCycleMember->x = 0.0F;
    firstCycleMember->y = 0.0F;
    firstCycleMember->width = 0.50F;
    firstCycleMember->height = 1.0F;
    secondCycleMember->visible = true;
    secondCycleMember->x = 0.50F;
    secondCycleMember->y = 0.0F;
    secondCycleMember->width = 0.50F;
    secondCycleMember->height = 1.0F;
    Check(cyclicCollapse.CollapseToModule(ModuleId::AllMusic, ModuleId::Rivan,
                                          ModuleCollapseSide::Right,
                                          ModuleCollapseMode::Inside) &&
              cyclicCollapse.ToggleCollapsedModule(ModuleId::AllMusic) &&
              !cyclicCollapse.CollapseToModule(ModuleId::Rivan, ModuleId::AllMusic,
                                                ModuleCollapseSide::Left,
                                                ModuleCollapseMode::Inside),
          "inside collapse rejects target ancestry cycles");
}

void TestSnappingWithExistingLayoutConflict() {
    using rivan::ui::ModuleCollapseMode;
    using rivan::ui::ModuleCollapseSide;
    using rivan::ui::ModuleDropZone;
    using rivan::ui::ModuleId;
    using rivan::ui::ModuleLayout;
    using rivan::ui::ModuleWindowDropZone;

    const auto conflictedLayout = [] {
        auto layout = ModuleLayout::Defaults();
        for (auto& item : layout.items) item.visible = false;

        auto configure = [&layout](ModuleId id, float x, float y, float width, float height) {
            auto* item = layout.Find(id);
            item->visible = true;
            item->x = x;
            item->y = y;
            item->width = width;
            item->height = height;
        };
        configure(ModuleId::Rivan, 0.0F, 0.0F, 0.50F, 0.50F);
        configure(ModuleId::AllMusic, 0.0F, 0.50F, 0.50F, 0.50F);
        // This matches a stale persisted layout where unrelated modules already
        // occupy the same rectangle. New docking must not be disabled solely by
        // that historical conflict, but it must still reject a new one.
        configure(ModuleId::GraphicEqualizer, 0.50F, 0.0F, 0.50F, 0.50F);
        configure(ModuleId::RivanLibrary, 0.50F, 0.0F, 0.50F, 0.50F);
        return layout;
    };

    auto side = conflictedLayout();
    Check(side.HasConflictingGeometry(),
          "the regression fixture starts with an unrelated persisted overlap");
    Check(side.SnapTo(ModuleId::AllMusic, ModuleId::Rivan, ModuleDropZone::Left),
          "side snapping still works when another persisted module pair overlaps");
    Check(std::abs(side.Find(ModuleId::AllMusic)->x) < 0.001F &&
              std::abs(side.Find(ModuleId::AllMusic)->width - 0.25F) < 0.001F,
          "side snapping keeps its expected split geometry with a stale overlap");

    auto window = conflictedLayout();
    Check(window.SnapToWindow(ModuleId::AllMusic, ModuleWindowDropZone::LeftTop,
                              0.10F, 0.10F),
          "window snapping still works when another persisted module pair overlaps");

    auto moduleCollapse = conflictedLayout();
    Check(moduleCollapse.CollapseToModule(ModuleId::AllMusic, ModuleId::Rivan,
                                          ModuleCollapseSide::Bottom,
                                          ModuleCollapseMode::Outside),
          "collapsing beside a module still works when another persisted pair overlaps");

    auto windowCollapse = conflictedLayout();
    Check(windowCollapse.CollapseToWindow(ModuleId::AllMusic, ModuleCollapseSide::Left),
          "collapsing to an application edge still works with a stale overlap");

    auto rejectsNewConflict = conflictedLayout();
    auto* equalizer = rejectsNewConflict.Find(ModuleId::GraphicEqualizer);
    equalizer->x = 0.0F;
    equalizer->y = 0.0F;
    equalizer->width = 0.25F;
    equalizer->height = 0.50F;
    Check(!rejectsNewConflict.SnapTo(ModuleId::AllMusic, ModuleId::Rivan,
                                     ModuleDropZone::Left),
          "docking still rejects a new overlap even when the layout already has one");
}

void TestOutsideCollapseTransactions() {
    using rivan::ui::ModuleCollapseMode;
    using rivan::ui::ModuleCollapseSide;
    using rivan::ui::ModuleId;
    using rivan::ui::ModuleLayout;
    using rivan::ui::ModuleNormalizedRect;
    using rivan::ui::ModuleWindowDropZone;
    const float nan = std::numeric_limits<float>::quiet_NaN();

    const auto layoutsEqual = [](const ModuleLayout& first, const ModuleLayout& second) {
        const auto valuesEqual = [](float left, float right) {
            return left == right || (std::isnan(left) && std::isnan(right));
        };
        if (first.tabCount != second.tabCount || first.activeTab != second.activeTab ||
            first.tabOrder != second.tabOrder || first.snapGroup != second.snapGroup ||
            first.tabGroupRoot != second.tabGroupRoot ||
            first.groupActiveTab != second.groupActiveTab) {
            return false;
        }
        for (std::size_t index = 0; index < first.items.size(); ++index) {
            const auto& left = first.items[index];
            const auto& right = second.items[index];
            if (left.id != right.id || !valuesEqual(left.x, right.x) ||
                !valuesEqual(left.y, right.y) || !valuesEqual(left.width, right.width) ||
                !valuesEqual(left.height, right.height) ||
                left.visible != right.visible || left.dockState != right.dockState ||
                left.collapseMode != right.collapseMode || left.collapseSide != right.collapseSide ||
                left.collapseTarget != right.collapseTarget ||
                left.collapseTargetIsWindow != right.collapseTargetIsWindow ||
                left.collapsed != right.collapsed || !valuesEqual(left.expandedX, right.expandedX) ||
                !valuesEqual(left.expandedY, right.expandedY) ||
                !valuesEqual(left.expandedWidth, right.expandedWidth) ||
                !valuesEqual(left.expandedHeight, right.expandedHeight) ||
                !valuesEqual(left.handleX, right.handleX) ||
                !valuesEqual(left.handleY, right.handleY) ||
                !valuesEqual(left.handleWidth, right.handleWidth) ||
                !valuesEqual(left.handleHeight, right.handleHeight)) {
                return false;
            }
        }
        return true;
    };

    auto recollapse = ModuleLayout::Defaults();
    for (auto& item : recollapse.items) item.visible = false;
    auto* recollapseTarget = recollapse.Find(ModuleId::Rivan);
    auto* recollapseSource = recollapse.Find(ModuleId::AllMusic);
    recollapseTarget->visible = true;
    recollapseTarget->x = 0.30F;
    recollapseTarget->y = 0.30F;
    recollapseTarget->width = 0.30F;
    recollapseTarget->height = 0.30F;
    recollapseSource->visible = true;
    recollapseSource->x = 0.05F;
    recollapseSource->y = 0.05F;
    recollapseSource->width = 0.15F;
    recollapseSource->height = 0.15F;
    Check(recollapse.CollapseToModule(ModuleId::AllMusic, ModuleId::Rivan,
                                      ModuleCollapseSide::Right,
                                      ModuleCollapseMode::Outside) &&
              recollapse.ToggleCollapsedModule(ModuleId::AllMusic) &&
              recollapse.ResizeModule(ModuleId::Rivan, 0.98F, 0.45F,
                                      true, false, false, false, false) &&
              recollapse.ToggleCollapsedModule(ModuleId::AllMusic),
          "outside collapse re-closes after target resize reaches its handle strip");
    recollapseTarget = recollapse.Find(ModuleId::Rivan);
    recollapseSource = recollapse.Find(ModuleId::AllMusic);
    Check(recollapseSource != nullptr && recollapseTarget != nullptr &&
              recollapseSource->collapsed &&
              std::abs(recollapseSource->handleX -
                       (recollapseTarget->x + recollapseTarget->width)) < 0.001F &&
              std::abs(recollapseSource->handleX + recollapseSource->handleWidth - 1.0F) < 0.001F &&
              recollapseSource->expandedX + recollapseSource->expandedWidth > 1.0F &&
              recollapse.HasValidGeometry() && !recollapse.HasConflictingGeometry(),
          "re-collapse reserves its edge handle while retaining attached expanded geometry");

    auto noSpace = ModuleLayout::Defaults();
    for (auto& item : noSpace.items) item.visible = false;
    auto* target = noSpace.Find(ModuleId::Rivan);
    auto* source = noSpace.Find(ModuleId::AllMusic);
    target->visible = true;
    target->x = 0.50F;
    target->y = 0.0F;
    target->width = 0.10F;
    target->height = 1.0F;
    source->visible = true;
    source->x = 0.05F;
    source->y = 0.40F;
    source->width = 0.15F;
    source->height = 0.20F;
    Check(noSpace.CollapseToModule(ModuleId::AllMusic, ModuleId::Rivan,
                                   ModuleCollapseSide::Right,
                                   ModuleCollapseMode::Outside) &&
              noSpace.ToggleCollapsedModule(ModuleId::AllMusic),
          "outside collapse opens before an impossible snap");
    noSpace.ClearModuleCollapse(ModuleId::AllMusic);
    source = noSpace.Find(ModuleId::AllMusic);
    ModuleLayout::SyncExpandedGeometry(*source);
    const auto setBlocker = [&noSpace](ModuleId id, float x, float width) {
        auto* item = noSpace.Find(id);
        item->visible = true;
        item->x = x;
        item->y = 0.0F;
        item->width = width;
        item->height = 1.0F;
    };
    setBlocker(ModuleId::GraphicEqualizer, 0.60F, 0.11F);
    setBlocker(ModuleId::RivanLibrary, 0.71F, 0.11F);
    setBlocker(ModuleId::VideoPreview, 0.82F, 0.09F);
    setBlocker(ModuleId::Lyrics, 0.91F, 0.09F);
    const auto beforeNoSpaceSnap = noSpace;
    Check(!noSpace.SnapToWindow(ModuleId::AllMusic, ModuleWindowDropZone::RightMiddle,
                                0.55F, 0.50F) &&
              layoutsEqual(noSpace, beforeNoSpaceSnap),
          "no-space snap after outside-collapse drag fails without changing layout");

    auto noSpaceCollapse = ModuleLayout::Defaults();
    for (auto& item : noSpaceCollapse.items) item.visible = false;
    auto* collapseTarget = noSpaceCollapse.Find(ModuleId::Rivan);
    auto* collapseSource = noSpaceCollapse.Find(ModuleId::AllMusic);
    collapseTarget->visible = true;
    collapseTarget->x = 0.50F;
    collapseTarget->width = 0.30F;
    collapseTarget->height = 1.0F;
    collapseSource->visible = true;
    collapseSource->x = 0.05F;
    collapseSource->y = 0.40F;
    collapseSource->width = 0.15F;
    collapseSource->height = 0.20F;
    const auto setCollapseBlocker = [&noSpaceCollapse](ModuleId id, float x, float width) {
        auto* item = noSpaceCollapse.Find(id);
        item->visible = true;
        item->x = x;
        item->y = 0.0F;
        item->width = width;
        item->height = 1.0F;
    };
    setCollapseBlocker(ModuleId::GraphicEqualizer, 0.80F, 0.10F);
    setCollapseBlocker(ModuleId::RivanLibrary, 0.90F, 0.10F);
    const auto beforeNoSpaceCollapse = noSpaceCollapse;
    Check(!noSpaceCollapse.CollapseToModule(ModuleId::AllMusic, ModuleId::Rivan,
                                             ModuleCollapseSide::Right,
                                             ModuleCollapseMode::Outside) &&
              layoutsEqual(noSpaceCollapse, beforeNoSpaceCollapse),
          "no-space outside collapse fails without changing layout");

    auto nanSource = ModuleLayout::Defaults();
    for (auto& item : nanSource.items) item.visible = false;
    auto* nanSourceTarget = nanSource.Find(ModuleId::Rivan);
    auto* nanSourceItem = nanSource.Find(ModuleId::AllMusic);
    nanSourceTarget->visible = true;
    nanSourceTarget->x = 0.30F;
    nanSourceTarget->y = 0.30F;
    nanSourceTarget->width = 0.30F;
    nanSourceTarget->height = 0.30F;
    nanSourceItem->visible = true;
    nanSourceItem->width = nan;
    const auto beforeNanSource = nanSource;
    Check(!nanSource.CollapseToWindow(ModuleId::AllMusic, ModuleCollapseSide::Right) &&
              layoutsEqual(nanSource, beforeNanSource),
          "window collapse rejects non-finite source dimensions transactionally");

    auto nanReattach = ModuleLayout::Defaults();
    for (auto& item : nanReattach.items) item.visible = false;
    auto* nanReattachTarget = nanReattach.Find(ModuleId::Rivan);
    auto* nanReattachSource = nanReattach.Find(ModuleId::AllMusic);
    nanReattachTarget->visible = true;
    nanReattachTarget->x = 0.30F;
    nanReattachTarget->y = 0.30F;
    nanReattachTarget->width = 0.30F;
    nanReattachTarget->height = 0.30F;
    nanReattachSource->visible = true;
    nanReattachSource->x = 0.05F;
    nanReattachSource->y = 0.05F;
    nanReattachSource->width = 0.15F;
    nanReattachSource->height = 0.15F;
    Check(nanReattach.CollapseToModule(ModuleId::AllMusic, ModuleId::Rivan,
                                       ModuleCollapseSide::Right,
                                       ModuleCollapseMode::Outside),
          "finite outside collapse prepares corrupted-geometry reattachment test");
    const auto beforeNanReattach = nanReattach;
    nanReattachSource = nanReattach.Find(ModuleId::AllMusic);
    nanReattachSource->expandedX = nan;
    Check(!nanReattach.ReattachOutsideCollapseHandles(beforeNanReattach) &&
              layoutsEqual(nanReattach, beforeNanReattach),
          "outside reattachment rejects non-finite stored geometry transactionally");

    auto nanObstacle = ModuleLayout::Defaults();
    for (auto& item : nanObstacle.items) item.visible = false;
    auto* nanObstacleSource = nanObstacle.Find(ModuleId::AllMusic);
    auto* nanObstacleItem = nanObstacle.Find(ModuleId::Rivan);
    nanObstacleSource->visible = true;
    nanObstacleSource->x = 0.0F;
    nanObstacleSource->y = 0.0F;
    nanObstacleSource->width = 0.40F;
    nanObstacleSource->height = 1.0F;
    nanObstacleItem->visible = true;
    nanObstacleItem->x = nan;
    ModuleNormalizedRect available{};
    Check(!nanObstacle.FindplusWindowRectangle(
              ModuleId::AllMusic, {0.0F, 0.0F, 1.0F, 1.0F}, 0.75F, 0.5F,
              0.10F, 0.10F, available),
          "free-space search rejects non-finite obstacle bounds before sorting");
}

void TestModuleLayoutSessionRoundTrip() {
    const auto root = std::filesystem::temp_directory_path() /
                      (L"RivanModuleLayoutTests-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    rivan::config::SettingsManager writer(root / L"settings.ini", root / L"session.ini");
    auto session = writer.Session();
    session.moduleLayout = rivan::ui::ModuleLayout::Defaults();
    session.moduleLayout.Find(rivan::ui::ModuleId::Rivan)->x = 0.2F;
    session.moduleLayout.Find(rivan::ui::ModuleId::AllMusic)->visible = false;
    session.moduleLayout.Find(rivan::ui::ModuleId::Rivan)->dockState =
        rivan::ui::ModuleDockState::Snapped;
    session.moduleLayout.MakeTab(rivan::ui::ModuleId::Rivan, rivan::ui::ModuleId::RivanLibrary);
    std::string error;
    Check(writer.SetSession(session, &error) && writer.SaveSession(&error),
          "module layout session saves");
    rivan::config::SettingsManager reader(root / L"settings.ini", root / L"session.ini");
    Check(reader.LoadSession(&error), "module layout session loads");
    Check(std::abs(reader.Session().moduleLayout.Find(rivan::ui::ModuleId::Rivan)->x - 0.2F) < 0.001F,
          "module position survives session round-trip");
    Check(!reader.Session().moduleLayout.Find(rivan::ui::ModuleId::AllMusic)->visible,
          "module visibility survives session round-trip");
    Check(reader.Session().moduleLayout.tabCount == 2,
           "module tabs survive session round-trip");
    Check(reader.Session().moduleLayout.Find(rivan::ui::ModuleId::Rivan)->dockState ==
               rivan::ui::ModuleDockState::Snapped,
          "module dock state survives session round-trip");

    auto multiGroupSession = writer.Session();
    multiGroupSession.moduleLayout = rivan::ui::ModuleLayout::Defaults();
    multiGroupSession.moduleLayout.MakeTab(rivan::ui::ModuleId::Rivan,
                                           rivan::ui::ModuleId::AllMusic);
    multiGroupSession.moduleLayout.MakeTab(rivan::ui::ModuleId::GraphicEqualizer,
                                           rivan::ui::ModuleId::RivanLibrary);
    multiGroupSession.moduleLayout.SetGroupActiveTab(rivan::ui::ModuleId::Rivan, 1);
    multiGroupSession.moduleLayout.SetGroupActiveTab(rivan::ui::ModuleId::GraphicEqualizer, 1);
    Check(writer.SetSession(multiGroupSession, &error) && writer.SaveSession(&error) &&
              reader.LoadSession(&error),
          "multiple tab groups save and reload");
    const auto& loadedMultiGroup = reader.Session().moduleLayout;
    Check(loadedMultiGroup.tabCount == 4 &&
              loadedMultiGroup.TabRoot(rivan::ui::ModuleId::Rivan) == rivan::ui::ModuleId::Rivan &&
              loadedMultiGroup.TabRoot(rivan::ui::ModuleId::AllMusic) == rivan::ui::ModuleId::Rivan &&
              loadedMultiGroup.TabRoot(rivan::ui::ModuleId::GraphicEqualizer) ==
                  rivan::ui::ModuleId::GraphicEqualizer &&
              loadedMultiGroup.TabRoot(rivan::ui::ModuleId::RivanLibrary) ==
                  rivan::ui::ModuleId::GraphicEqualizer &&
              loadedMultiGroup.GroupActiveMember(rivan::ui::ModuleId::Rivan) ==
                  rivan::ui::ModuleId::AllMusic &&
              loadedMultiGroup.GroupActiveMember(rivan::ui::ModuleId::GraphicEqualizer) ==
                  rivan::ui::ModuleId::RivanLibrary,
          "multiple tab groups retain roots and active tabs after persistence");

    {
        std::ofstream legacySession(root / L"legacy-session.ini", std::ios::binary);
        legacySession << "[meta]\nformat=1\n[modules]\n"
                      << "tab_count=2\nactive_tab=1\ntab_0=0\ntab_1=1\n";
    }
    rivan::config::SettingsManager legacyReader(root / L"settings.ini", root / L"legacy-session.ini");
    Check(legacyReader.LoadSession(&error), "legacy global tab session loads");
    const auto& legacyLayout = legacyReader.Session().moduleLayout;
    Check(legacyLayout.tabCount == 2 &&
              legacyLayout.TabRoot(rivan::ui::ModuleId::Rivan) == rivan::ui::ModuleId::Rivan &&
              legacyLayout.TabRoot(rivan::ui::ModuleId::AllMusic) == rivan::ui::ModuleId::Rivan &&
              legacyLayout.GroupActiveMember(rivan::ui::ModuleId::Rivan) ==
                  rivan::ui::ModuleId::AllMusic,
          "legacy tab_count, tab_i, and active_tab retain their single-group meaning");

    auto resizable = writer.Session();
    resizable.moduleLayout = rivan::ui::ModuleLayout::Defaults();
    auto* collapsible = resizable.moduleLayout.Find(rivan::ui::ModuleId::RivanLibrary);
    Check(collapsible != nullptr &&
              resizable.moduleLayout.CollapseToWindow(rivan::ui::ModuleId::RivanLibrary,
                                                      rivan::ui::ModuleCollapseSide::Right) &&
              resizable.moduleLayout.ToggleCollapsedModule(rivan::ui::ModuleId::RivanLibrary),
          "a collapsible module can be prepared for persistence");
    collapsible = resizable.moduleLayout.Find(rivan::ui::ModuleId::RivanLibrary);
    collapsible->x = 0.50F;
    collapsible->y = 0.08F;
    collapsible->width = 0.34F;
    collapsible->height = 0.40F;
    rivan::ui::ModuleLayout::SyncExpandedGeometry(*collapsible);
    Check(writer.SetSession(resizable, &error) && writer.SaveSession(&error),
          "resized collapsible module session saves");
    Check(reader.LoadSession(&error), "resized collapsible module session reloads");
    const auto* loadedCollapsible =
        reader.Session().moduleLayout.Find(rivan::ui::ModuleId::RivanLibrary);
    Check(loadedCollapsible != nullptr &&
               std::abs(loadedCollapsible->expandedX - 0.50F) < 0.001F &&
               std::abs(loadedCollapsible->expandedY - 0.08F) < 0.001F &&
               std::abs(loadedCollapsible->expandedWidth - 0.34F) < 0.001F &&
               std::abs(loadedCollapsible->expandedHeight - 0.40F) < 0.001F,
           "resized collapsible geometry survives a session round-trip");

    auto retainedExpansion = writer.Session();
    retainedExpansion.moduleLayout = rivan::ui::ModuleLayout::Defaults();
    for (auto& item : retainedExpansion.moduleLayout.items) item.visible = false;
    auto* retainedTarget = retainedExpansion.moduleLayout.Find(rivan::ui::ModuleId::Rivan);
    auto* retainedSource = retainedExpansion.moduleLayout.Find(rivan::ui::ModuleId::AllMusic);
    retainedTarget->visible = true;
    retainedTarget->width = 0.10F;
    retainedTarget->height = 1.0F;
    retainedSource->visible = true;
    retainedSource->x = 0.10F;
    retainedSource->y = 0.20F;
    retainedSource->width = 0.80F;
    retainedSource->height = 0.40F;
    Check(retainedExpansion.moduleLayout.CollapseToModule(
              rivan::ui::ModuleId::AllMusic, rivan::ui::ModuleId::Rivan,
              rivan::ui::ModuleCollapseSide::Right,
              rivan::ui::ModuleCollapseMode::Outside) &&
              retainedExpansion.moduleLayout.PreservePixelGeometry(
                  1000.0F, 800.0F, 700.0F, 800.0F),
          "collapsed expansion outside current canvas survives session round-trip");
    const auto* retainedBeforeSave =
        retainedExpansion.moduleLayout.Find(rivan::ui::ModuleId::AllMusic);
    const float retainedExpandedX = retainedBeforeSave->expandedX;
    const float retainedExpandedY = retainedBeforeSave->expandedY;
    const float retainedExpandedWidth = retainedBeforeSave->expandedWidth;
    const float retainedExpandedHeight = retainedBeforeSave->expandedHeight;
    Check(writer.SetSession(retainedExpansion, &error) && writer.SaveSession(&error) &&
              reader.LoadSession(&error),
          "collapsed expansion outside current canvas saves and reloads");
    const auto* loadedRetainedSource =
        reader.Session().moduleLayout.Find(rivan::ui::ModuleId::AllMusic);
    Check(loadedRetainedSource != nullptr && loadedRetainedSource->collapsed,
          "session reload retains collapsed state outside canvas");
    Check(loadedRetainedSource != nullptr &&
              std::abs(loadedRetainedSource->expandedX - retainedExpandedX) < 0.001F,
          "session reload retains collapsed expanded x outside canvas");
    Check(loadedRetainedSource != nullptr &&
              std::abs(loadedRetainedSource->expandedY - retainedExpandedY) < 0.001F,
          "session reload retains collapsed expanded y outside canvas");
    Check(loadedRetainedSource != nullptr && loadedRetainedSource->expandedWidth > 1.0F &&
              std::abs(loadedRetainedSource->expandedWidth - retainedExpandedWidth) < 0.001F,
          "session reload retains collapsed expanded width outside canvas");
    Check(loadedRetainedSource != nullptr &&
              std::abs(loadedRetainedSource->expandedHeight - retainedExpandedHeight) < 0.001F,
          "session reload retains collapsed expanded height outside canvas");

    auto cyclicSession = writer.Session();
    cyclicSession.moduleLayout = rivan::ui::ModuleLayout::Defaults();
    auto* firstCycle = cyclicSession.moduleLayout.Find(rivan::ui::ModuleId::Rivan);
    auto* secondCycle = cyclicSession.moduleLayout.Find(rivan::ui::ModuleId::AllMusic);
    firstCycle->collapseMode = rivan::ui::ModuleCollapseMode::Inside;
    firstCycle->collapseSide = rivan::ui::ModuleCollapseSide::Right;
    firstCycle->collapseTarget = rivan::ui::ModuleId::AllMusic;
    firstCycle->collapsed = true;
    secondCycle->collapseMode = rivan::ui::ModuleCollapseMode::Inside;
    secondCycle->collapseSide = rivan::ui::ModuleCollapseSide::Left;
    secondCycle->collapseTarget = rivan::ui::ModuleId::Rivan;
    secondCycle->collapsed = true;
    Check(writer.SetSession(cyclicSession, &error) && writer.SaveSession(&error) &&
              reader.LoadSession(&error),
          "cyclic collapse session round-trip loads");
    Check(!(reader.Session().moduleLayout.Find(rivan::ui::ModuleId::Rivan)->collapsed &&
            reader.Session().moduleLayout.Find(rivan::ui::ModuleId::AllMusic)->collapsed),
          "session load breaks cyclic collapse metadata");
    Check(reader.Session().moduleLayout.HasValidGeometry(),
          "cycle cleanup leaves valid expanded module geometry");

    std::filesystem::remove_all(root, ec);
}

void TestLegacySongCoverSettingMigration() {
    const auto root = std::filesystem::temp_directory_path() /
                      (L"RivanSongRowMigrationTests-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    const auto settingsFile = root / L"settings.ini";
    const auto sessionFile = root / L"session.ini";
    {
        std::ofstream settings(settingsFile, std::ios::binary);
        settings << "[meta]\nformat=1\n[library]\nmusic_root="
                 << (root / L"Music").string()
                 << "\n[appearance]\nskin=dark-purple\ntrack_covers_enabled=false\n";
    }
    std::filesystem::create_directories(root / L"Music", ec);

    rivan::config::SettingsManager manager(settingsFile, sessionFile);
    std::string error;
    Check(manager.LoadSettings(&error), "legacy song cover preference loads");
    Check(!manager.Settings().songRowLayout.Field(rivan::ui::SongRowField::Cover).visible,
           "legacy disabled song covers migrate to a hidden cover field");
    Check(manager.SaveSettings(&error), "migrated song row layout saves");
    std::ifstream saved(settingsFile, std::ios::binary);
    const std::string persisted((std::istreambuf_iterator<char>(saved)),
                                std::istreambuf_iterator<char>());
    Check(persisted.find("[song_row_layout]") != std::string::npos &&
              persisted.find("track_covers_enabled") == std::string::npos,
           "song row layout replaces the obsolete song cover setting on save");
    std::filesystem::remove_all(root, ec);
}

void TestWindowSessionRoundTrip() {
    const auto root = std::filesystem::temp_directory_path() /
                      (L"RivanWindowSessionTests-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    rivan::config::SettingsManager writer(root / L"settings.ini", root / L"session.ini");
    auto session = writer.Session();
    session.window = {250, 300, 800, 600};
    std::string error;
    Check(writer.SetSession(session, &error) && writer.SaveSession(&error),
          "main window minimum size saves to the session");

    rivan::config::SettingsManager reader(root / L"settings.ini", root / L"session.ini");
    Check(reader.LoadSession(&error), "main window minimum size session reloads");
    const auto& window = reader.Session().window;
    Check(window.x == 250 && window.y == 300 && window.width == 800 && window.height == 600,
          "main window size and position survive a session round-trip");

    std::filesystem::remove_all(root, ec);
}

void TestModuleResizeCollisionBehavior() {
    using rivan::ui::ModuleId;
    using rivan::ui::ModuleLayout;

    auto squash = ModuleLayout::Defaults();
    for (auto& item : squash.items) item.visible = false;
    auto* resized = squash.Find(ModuleId::Rivan);
    auto* obstacle = squash.Find(ModuleId::AllMusic);
    resized->visible = true;
    resized->x = 0.0F;
    resized->y = 0.0F;
    resized->width = 0.40F;
    resized->height = 0.50F;
    obstacle->visible = true;
    obstacle->x = 0.50F;
    obstacle->y = 0.0F;
    obstacle->width = 0.50F;
    obstacle->height = 0.50F;
    Check(squash.ResizeModule(ModuleId::Rivan, 0.90F, 0.25F, true, false, false, false),
          "module resize with collision applies");
    Check(std::abs(squash.Find(ModuleId::Rivan)->width - 0.90F) < 0.001F &&
              std::abs(squash.Find(ModuleId::AllMusic)->x - 0.90F) < 0.001F &&
              !squash.HasConflictingGeometry(),
          "squash resize gives resized module priority and preserves obstacle minimum size");

    auto overlap = ModuleLayout::Defaults();
    for (auto& item : overlap.items) item.visible = false;
    overlap.Find(ModuleId::Rivan)->visible = true;
    overlap.Find(ModuleId::Rivan)->x = 0.0F;
    overlap.Find(ModuleId::Rivan)->y = 0.0F;
    overlap.Find(ModuleId::Rivan)->width = 0.40F;
    overlap.Find(ModuleId::Rivan)->height = 0.50F;
    overlap.Find(ModuleId::AllMusic)->visible = true;
    overlap.Find(ModuleId::AllMusic)->x = 0.50F;
    overlap.Find(ModuleId::AllMusic)->y = 0.0F;
    overlap.Find(ModuleId::AllMusic)->width = 0.50F;
    overlap.Find(ModuleId::AllMusic)->height = 0.50F;
    Check(overlap.ResizeModule(ModuleId::Rivan, 0.90F, 0.25F,
                               true, false, false, false, false) &&
              overlap.HasConflictingGeometry(),
           "overlap resize toggle preserves legacy overlapping behavior");

    auto unsafeCandidate = ModuleLayout::Defaults();
    for (auto& item : unsafeCandidate.items) item.visible = false;
    const auto configure = [&unsafeCandidate](ModuleId id, float x, float y,
                                               float width, float height) {
        auto* item = unsafeCandidate.Find(id);
        item->visible = true;
        item->x = x;
        item->y = y;
        item->width = width;
        item->height = height;
        ModuleLayout::SyncExpandedGeometry(*item);
    };
    configure(ModuleId::Rivan, 0.0F, 0.80F, 0.10F, 0.10F);
    configure(ModuleId::AllMusic, 0.10F, 0.30F, 0.30F, 0.30F);
    configure(ModuleId::GraphicEqualizer, 0.60F, 0.30F, 0.30F, 0.30F);
    Check(unsafeCandidate.SquashForExpansion(
              ModuleId::Rivan, {0.20F, 0.40F, 0.60F, 0.80F}) &&
              !unsafeCandidate.HasConflictingGeometry() &&
              unsafeCandidate.Find(ModuleId::AllMusic)->x < 0.60F &&
              unsafeCandidate.Find(ModuleId::Rivan)->x == 0.0F &&
              unsafeCandidate.Find(ModuleId::Rivan)->y == 0.80F,
          "squash rejects an unsafe candidate and preserves valid layout state");
}

void TestLyricsParsing() {
    const auto document = rivan::lyrics::LyricsService::ParseLrc(
        L"[ar:Artist]\n[00:01.20][00:02.30]First line\n[01:02.50]Second line\n");
    Check(document.synced && document.lines.size() == 3,
          "LRC parser expands repeated timestamps");
    Check(document.lines[0].timestampSeconds > 1.19 &&
              document.lines[0].timestampSeconds < 1.21 &&
              document.lines[0].text == L"First line",
          "LRC parser reads timestamp and text");
    const auto plain = rivan::lyrics::LyricsService::ParseLrclibResponse(
        R"({"syncedLyrics":null,"plainLyrics":"One\nTwo"})");
    Check(!plain.synced && plain.PlainText() == L"One\nTwo",
          "LRCLIB parser falls back to plain lyrics");
    const auto escaped = rivan::lyrics::LyricsService::ParseLrclibResponse(
        R"({"syncedLyrics":null,"plainLyrics":"Caf\u00e9\nIt\u0027s fine"})");
    Check(!escaped.synced && escaped.PlainText() == L"Café\nIt's fine",
          "LRCLIB parser decodes JSON unicode escapes");
    const auto astral = rivan::lyrics::LyricsService::ParseLrclibResponse(
        R"({"syncedLyrics":null,"plainLyrics":"Smile \ud83d\ude00"})");
    Check(!astral.Empty() && astral.PlainText() == L"Smile 😀",
          "LRCLIB parser decodes JSON surrogate pairs");
    const auto result = rivan::lyrics::LyricsService::ParseLrclibResponse(
        R"({"id":1,"trackName":"misery.","artistName":"pupsies","plainLyrics":"One\nTwo","syncedLyrics":"[00:00.50]One\n[00:01.00]Two"})");
    Check(result.synced && result.lines.size() == 2 && result.lines[1].timestampSeconds > 0.99,
          "LRCLIB search result parses synchronized lyrics");
    const auto structural = rivan::lyrics::LyricsService::ParseLrclibResponse(
        R"({"plainLyrics":"The phrase \"syncedLyrics\" is text","syncedLyrics":null})");
    Check(!structural.synced && structural.PlainText() == L"The phrase \"syncedLyrics\" is text",
          "LRCLIB parser ignores field-like lyric text");
}

void TestLyricsPublicationRevisions() {
    std::mutex mutex;
    std::condition_variable completion;
    bool completed = false;
    rivan::lyrics::LyricsService service;
    const auto initialRevision = service.Revision();
    service.SetNotify([&] {
        if (!service.Snapshot().loading) {
            std::scoped_lock lock(mutex);
            completed = true;
            completion.notify_one();
        }
    });

    service.Request(1, L"", L"", L"", 0.0);
    {
        std::unique_lock lock(mutex);
        Check(completion.wait_for(lock, std::chrono::seconds{2}, [&] { return completed; }),
              "empty lyrics request publishes a completion snapshot");
    }

    const auto snapshot = service.Snapshot();
    Check(!snapshot.loading, "completed lyrics snapshot is not loading");
    Check(service.Revision() == initialRevision + 2 &&
              snapshot.revision == initialRevision + 2,
          "lyrics loading and completion snapshots receive distinct revisions");
}

void TestLyricsLayoutMigration() {
    const auto root = std::filesystem::temp_directory_path() /
                      (L"RivanLyricsMigrationTests-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    {
        std::ofstream session(root / L"session.ini", std::ios::binary);
        session << "[meta]\nformat=1\n[modules]\n";
        session << "module_3_x=0.46\nmodule_3_y=0\nmodule_3_width=0.54\nmodule_3_height=0.66\n";
        session << "module_4_x=0.46\nmodule_4_y=0.68\nmodule_4_width=0.54\nmodule_4_height=0.30\n";
    }
    rivan::config::SettingsManager settings(root / L"settings.ini", root / L"session.ini");
    std::string loadError;
    Check(settings.LoadSession(&loadError), "five-module session loads for lyrics migration");
    const auto* lyrics = settings.Session().moduleLayout.Find(rivan::ui::ModuleId::Lyrics);
    const auto* preview = settings.Session().moduleLayout.Find(rivan::ui::ModuleId::VideoPreview);
    Check(lyrics != nullptr && lyrics->visible && preview != nullptr &&
              std::abs(preview->y - 0.49F) < 0.001F,
          "five-module default session migrates to visible lyrics layout");
    std::filesystem::remove_all(root, error);
}

// ---------------------------------------------------------------------------
// Pre-release regression coverage: nested-root dedup and the AddExternalTrack
// duplicate guard. (Another agent appends layout tests below this banner.)
// ---------------------------------------------------------------------------

void TestNestedRootDedup() {
    TemporaryLibrary files;
    rivan::library::LibraryScanner scanner;
    // [child, parent] configuration: the acceptance loop keeps both roots, so a
    // later pass must drop the broader parent and scan the child only once.
    const std::vector<std::filesystem::path> roots{files.Root() / L"Rock",
                                                   files.Root()};
    const auto scan = scanner.Scan(std::span<const std::filesystem::path>(roots));
    Check(scan.root.filename() == L"Rock",
          "child-first overlapping roots keep the first (deepest) root as catalog root");
    Check(scan.tracks.size() == 3,
          "child-first overlapping roots scan the deeper root only");
    std::unordered_set<rivan::library::TrackId> seenTracks;
    bool duplicatedTrack = false;
    for (const auto& track : scan.tracks) {
        if (!seenTracks.insert(track.id).second) duplicatedTrack = true;
    }
    Check(!duplicatedTrack,
          "child-first overlapping roots emit no duplicate track ids");
    Check(scan.tracks.size() > 0 && scan.tracks.front().filePath.filename() == L"four.opus",
          "scanned tracks stay ordered by folded file path");
    Check(scan.playlists.front().kind == rivan::playlist::PlaylistKind::AllMusic,
          "All Music remains the leading playlist");
    std::unordered_set<rivan::library::TrackId> seenAllMusic;
    bool duplicatedAllMusic = false;
    for (const auto id : scan.playlists.front().trackIds) {
        if (!seenAllMusic.insert(id).second) duplicatedAllMusic = true;
    }
    Check(!duplicatedAllMusic,
          "overlapping roots add each id to All Music exactly once");
    const auto rock = std::find_if(scan.playlists.begin(), scan.playlists.end(),
                                   [](const auto& playlist) {
                                       return playlist.kind ==
                                                  rivan::playlist::PlaylistKind::Directory &&
                                              playlist.directory.filename() == L"Rock";
                                   });
    Check(rock != scan.playlists.end() && rock->trackIds.size() == 3,
          "the child folder playlist keeps its direct tracks once");
    if (rock != scan.playlists.end()) {
        std::unordered_set<rivan::library::TrackId> seenRock;
        bool duplicatedRock = false;
        for (const auto id : rock->trackIds) {
            if (!seenRock.insert(id).second) duplicatedRock = true;
        }
        Check(!duplicatedRock,
              "overlapping roots do not duplicate ids in the child playlist");
    }
}

void TestSiblingRootRetention() {
    TemporaryLibrary files;
    rivan::library::LibraryScanner scanner;
    const std::vector<std::filesystem::path> roots{files.Root() / L"Rock",
                                                   files.Root() / L"Game"};
    const auto scan = scanner.Scan(std::span<const std::filesystem::path>(roots));
    Check(scan.tracks.size() == 4,
          "same-drive sibling roots are both scanned instead of being treated as descendants");
    const auto hasDirectory = [&scan](std::wstring_view name) {
        return std::any_of(scan.playlists.begin(), scan.playlists.end(), [name](const auto& playlist) {
            return playlist.kind == rivan::playlist::PlaylistKind::Directory &&
                   playlist.directory.filename() == name;
        });
    };
    Check(hasDirectory(L"Rock") && hasDirectory(L"Game"),
          "same-drive sibling roots both produce directory playlists");
}

void TestAddExternalTrackDuplicateGuard() {
    rivan::playlist::Playlist folder;
    folder.id = 42;
    folder.name = L"Folder";
    folder.kind = rivan::playlist::PlaylistKind::Directory;
    // A Directory playlist can retain an id from a previous rescan even when the
    // source file is gone, leaving that id outside the scan catalog.
    const auto orphan = rivan::library::Track::FromFile(
        std::filesystem::path(L"C:/music/a.mp3"));
    folder.trackIds.push_back(orphan.id);
    rivan::playlist::PlaylistManager manager;
    manager.ApplyScan(MakeScan({}, {folder}));
    Check(!manager.AddExternalTrack(folder.id, orphan),
          "AddExternalTrack rejects a duplicate Directory id");
    Check(manager.FindTrack(orphan.id) == nullptr,
          "rejected duplicate import leaves no stale external track");
    const auto fresh = rivan::library::Track::FromFile(
        std::filesystem::path(L"C:/music/b.mp3"));
    Check(manager.AddExternalTrack(folder.id, fresh),
          "AddExternalTrack still accepts a fresh Directory id");
    Check(manager.FindTrack(fresh.id) != nullptr,
          "accepted import resolves through the manager");
}

// ---------------------------------------------------------------------------
// Pre-release layout regression coverage: collapse-shift isolation during
// resize expansion and proportional tab-strip widths in narrow panels.
// ---------------------------------------------------------------------------

void TestCollapseShiftIsolation() {
    using rivan::ui::ModuleCollapseMode;
    using rivan::ui::ModuleCollapseSide;
    using rivan::ui::ModuleExpansionBehavior;
    using rivan::ui::ModuleId;
    using rivan::ui::ModuleLayout;

    // The source collapses to the left window edge; one peer is collapsed with its
    // handle inside the source's expanded bounds while a second peer collapses to
    // the same edge well below the source. The blocker covers the source's expanded
    // rectangle only afterwards, forcing the resize path.
    const auto prepare = [] {
        auto layout = ModuleLayout::Defaults();
        for (auto& item : layout.items) item.visible = false;
        auto* source = layout.Find(ModuleId::Rivan);
        auto* blocker = layout.Find(ModuleId::AllMusic);
        source->visible = true;
        source->x = 0.0F;
        source->y = 0.0F;
        source->width = 0.30F;
        source->height = 0.40F;
        blocker->visible = true;
        blocker->x = 0.30F;
        blocker->y = 0.0F;
        blocker->width = 0.30F;
        blocker->height = 1.0F;
        Check(layout.CollapseToWindow(ModuleId::Rivan, ModuleCollapseSide::Left, 0.20F),
              "collapse-shift fixture collapses the expansion source to the left edge");
        auto* overlapping = layout.Find(ModuleId::GraphicEqualizer);
        overlapping->visible = true;
        // Deterministic window-collapsed geometry covering the source's rectangle;
        // SetCollapsedGeometry mirrors CollapseToWindow's handle attachment.
        layout.SetCollapsedGeometry(
            *overlapping, {0.0F, 0.05F, 0.06F, 0.15F}, {0.0F, 0.0F, 0.30F, 0.40F},
            ModuleCollapseMode::Outside, ModuleCollapseSide::Left, ModuleId::GraphicEqualizer,
            true);
        auto* unrelated = layout.Find(ModuleId::RivanLibrary);
        unrelated->visible = true;
        layout.SetCollapsedGeometry(
            *unrelated, {0.0F, 0.70F, 0.06F, 0.80F}, {0.0F, 0.60F, 0.30F, 0.90F},
            ModuleCollapseMode::Outside, ModuleCollapseSide::Left, ModuleId::RivanLibrary,
            true);
        blocker = layout.Find(ModuleId::AllMusic);
        blocker->x = 0.20F;
        blocker->width = 0.50F;
        return layout;
    };

    auto layout = prepare();
    const auto* source = layout.Find(ModuleId::Rivan);
    const auto* overlapping = layout.Find(ModuleId::GraphicEqualizer);
    const auto* unrelated = layout.Find(ModuleId::RivanLibrary);
    const rivan::ui::ModuleNormalizedRect sourceRect{
        source->expandedX, source->expandedY,
        source->expandedX + source->expandedWidth,
        source->expandedY + source->expandedHeight};
    Check(ModuleLayout::Intersects(ModuleLayout::Bounds(*overlapping), sourceRect) &&
              !ModuleLayout::Intersects(ModuleLayout::Bounds(*unrelated), sourceRect),
          "collapse-shift fixture places only one collapsed peer inside the source");

    const auto overlappingBefore = *overlapping;
    const auto unrelatedBefore = *unrelated;
    const float sourceRightPixels =
        (source->expandedX + source->expandedWidth) * 1000.0F;
    const float blockerLeftPixels = layout.Find(ModuleId::AllMusic)->x * 1000.0F;
    // Matches ResizeForExpansion: overlapping obstacle right edge plus the 8px gap.
    const float expectedShift = sourceRightPixels - blockerLeftPixels + 8.0F;
    Check(expectedShift > 0.0F, "collapse-shift fixture forces canvas growth");

    float resizedWidth = 0.0F;
    float resizedHeight = 0.0F;
    Check(layout.ToggleCollapsedModule(ModuleId::Rivan, ModuleExpansionBehavior::Resize,
                                       1000.0F, 800.0F, &resizedWidth, &resizedHeight) &&
              resizedWidth > 1000.0F,
          "resize expansion runs the collapse-shift pass");
    overlapping = layout.Find(ModuleId::GraphicEqualizer);
    unrelated = layout.Find(ModuleId::RivanLibrary);
    source = layout.Find(ModuleId::Rivan);
    Check(std::abs(unrelated->handleX * resizedWidth -
                   unrelatedBefore.handleX * 1000.0F) < 0.01F &&
              std::abs(unrelated->expandedX * resizedWidth -
                       unrelatedBefore.expandedX * 1000.0F) < 0.01F &&
              unrelated->handleX < 0.001F,
          "unrelated collapsed peer keeps its handle attached to the window edge");
    Check(std::abs(overlapping->handleX * resizedWidth -
                   (overlappingBefore.handleX * 1000.0F + expectedShift)) < 0.01F &&
              std::abs(overlapping->expandedX * resizedWidth -
                       (overlappingBefore.expandedX * 1000.0F + expectedShift)) < 0.01F,
          "collapsed peer overlapping the expanding source shifts with the growth");
    Check(std::abs(overlapping->x - overlapping->handleX) < 0.0001F &&
              std::abs(overlapping->y - overlapping->handleY) < 0.0001F &&
              std::abs(overlapping->width - overlapping->handleWidth) < 0.0001F &&
              std::abs(overlapping->height - overlapping->handleHeight) < 0.0001F &&
              std::abs(unrelated->x - unrelated->handleX) < 0.0001F &&
              std::abs(unrelated->y - unrelated->handleY) < 0.0001F &&
              std::abs(unrelated->width - unrelated->handleWidth) < 0.0001F &&
              std::abs(unrelated->height - unrelated->handleHeight) < 0.0001F,
          "horizontal collapse resize keeps item rectangles equal to handle rectangles");
    Check(!layout.HasConflictingGeometry() && source != nullptr && !source->collapsed,
          "collapse-shift resize opens the source without new conflicts");

    auto vertical = ModuleLayout::Defaults();
    for (auto& item : vertical.items) item.visible = false;
    auto* verticalSource = vertical.Find(ModuleId::Rivan);
    auto* verticalBlocker = vertical.Find(ModuleId::AllMusic);
    verticalSource->visible = true;
    verticalSource->x = 0.0F;
    verticalSource->y = 0.0F;
    verticalSource->width = 0.40F;
    verticalSource->height = 0.30F;
    verticalBlocker->visible = true;
    verticalBlocker->x = 0.0F;
    verticalBlocker->y = 0.30F;
    verticalBlocker->width = 1.0F;
    verticalBlocker->height = 0.50F;
    Check(vertical.CollapseToWindow(ModuleId::Rivan, ModuleCollapseSide::Top, 0.20F),
          "vertical collapse-shift fixture collapses the source to the top edge");
    auto* verticalOverlapping = vertical.Find(ModuleId::GraphicEqualizer);
    verticalOverlapping->visible = true;
    vertical.SetCollapsedGeometry(
        *verticalOverlapping, {0.05F, 0.0F, 0.15F, 0.06F}, {0.0F, 0.0F, 0.40F, 0.30F},
        ModuleCollapseMode::Outside, ModuleCollapseSide::Top,
        ModuleId::GraphicEqualizer, true);
    auto* verticalUnrelated = vertical.Find(ModuleId::RivanLibrary);
    verticalUnrelated->visible = true;
    vertical.SetCollapsedGeometry(
        *verticalUnrelated, {0.70F, 0.70F, 0.80F, 0.76F}, {0.60F, 0.60F, 0.90F, 0.90F},
        ModuleCollapseMode::Outside, ModuleCollapseSide::Top,
        ModuleId::RivanLibrary, true);
    verticalBlocker = vertical.Find(ModuleId::AllMusic);
    verticalBlocker->y = 0.20F;
    verticalBlocker->height = 0.50F;
    float verticalWidth = 0.0F;
    float verticalHeight = 0.0F;
    Check(vertical.ToggleCollapsedModule(ModuleId::Rivan, ModuleExpansionBehavior::Resize,
                                         1000.0F, 800.0F, &verticalWidth, &verticalHeight) &&
              verticalHeight > 800.0F,
          "vertical resize expansion runs the collapse-shift pass");
    verticalOverlapping = vertical.Find(ModuleId::GraphicEqualizer);
    verticalUnrelated = vertical.Find(ModuleId::RivanLibrary);
    Check(std::abs(verticalOverlapping->x - verticalOverlapping->handleX) < 0.0001F &&
              std::abs(verticalOverlapping->y - verticalOverlapping->handleY) < 0.0001F &&
              std::abs(verticalOverlapping->width - verticalOverlapping->handleWidth) < 0.0001F &&
              std::abs(verticalOverlapping->height - verticalOverlapping->handleHeight) < 0.0001F &&
              std::abs(verticalUnrelated->x - verticalUnrelated->handleX) < 0.0001F &&
              std::abs(verticalUnrelated->y - verticalUnrelated->handleY) < 0.0001F &&
              std::abs(verticalUnrelated->width - verticalUnrelated->handleWidth) < 0.0001F &&
              std::abs(verticalUnrelated->height - verticalUnrelated->handleHeight) < 0.0001F,
          "vertical collapse resize keeps item rectangles equal to handle rectangles");
}

void TestTabStripProportionalWidths() {
    using rivan::ui::ModuleId;
    using rivan::ui::ModuleLayout;

    auto layout = ModuleLayout::Defaults();
    layout.MakeTab(ModuleId::Rivan, ModuleId::AllMusic);
    layout.TabWith(ModuleId::GraphicEqualizer, ModuleId::Rivan);
    layout.TabWith(ModuleId::RivanLibrary, ModuleId::Rivan);
    layout.TabWith(ModuleId::VideoPreview, ModuleId::Rivan);
    const ModuleId root = ModuleId::Rivan;
    Check(layout.tabCount == 5 && layout.GroupTabCount(root) == 5,
          "narrow-panel tab fixture groups five modules");

    // Narrow panel: five tabs at the old 44px floor would need 220px and overflow.
    // The mirror below reproduces the renderer's proportional tab-strip math.
    constexpr float boundsLeft = 10.0F;
    constexpr float boundsRight = 110.0F;
    const std::size_t count = layout.GroupTabCount(root);
    const auto boundary = [boundsLeft, boundsRight, count](std::size_t index) {
        return boundsLeft + (boundsRight - boundsLeft) *
            static_cast<float>(index) / static_cast<float>(count);
    };
    float lastRight = boundsLeft;
    for (std::size_t index = 0; index < count; ++index) {
        const float left = boundary(index);
        const float right = std::min(boundsRight, boundary(index + 1));
        lastRight = right;
        Check(left < right, "tab strip keeps every tab rectangle ordered");
        Check(left >= boundsLeft - 0.001F && right <= boundsRight + 0.001F,
              "tab strip keeps every tab rectangle inside the panel");
    }
    Check(std::abs(lastRight - boundsRight) < 0.001F,
          "the last tab ends exactly at the panel right edge");

    // The renderer maps a pixel inside a tab to its group member, so the last tab
    // must remain reachable through the layout's tab-index scheme.
    const float tabWidth = (boundsRight - boundsLeft) / static_cast<float>(count);
    const auto hitIndex = [boundsLeft, tabWidth](float x) {
        return static_cast<std::size_t>((x - boundsLeft) / tabWidth);
    };
    const std::size_t lastIndex = count - 1;
    const float lastCenter = boundary(lastIndex) + tabWidth * 0.5F;
    Check(hitIndex(lastCenter) == lastIndex &&
              layout.GroupMember(root, lastIndex) == ModuleId::VideoPreview &&
              layout.TabIndex(ModuleId::VideoPreview) == lastIndex,
          "the last tab stays reachable and maps to its group member");
}

} // namespace

int main() {
    TestLibraryAndQueue();
    TestUserPlaylistEditing();
    TestUserPlaylistScanFlow();
    TestSpectrum();
    TestAnalysisBufferReuse();
    TestSkinCustomizationRoundTrip();
    TestSkinRejectsUnsafeAssets();
    TestSongRowLayoutSettingRoundTrip();
    TestSongRowLayoutDefaults();
    TestTrackCoverCacheDimensions();
    TestLegacySongCoverSettingMigration();
    TestWindowSessionRoundTrip();
    TestModuleResizeCollisionBehavior();
    TestIniMetaFormat();
    TestIniValueCodec();
    TestUiModuleRegistry();
    TestDuplicateModuleGeometryRepair();
    TestWindowSnapping();
    TestCollapsibleSnapping();
    TestSnappingWithExistingLayoutConflict();
    TestOutsideCollapseTransactions();
    TestModuleLayoutSessionRoundTrip();
    TestLyricsParsing();
    TestLyricsPublicationRevisions();
    TestLyricsLayoutMigration();
    TestNestedRootDedup();
    TestSiblingRootRetention();
    TestAddExternalTrackDuplicateGuard();
    TestCollapseShiftIsolation();
    TestTabStripProportionalWidths();
    if (failures == 0) std::cout << "Rivan core tests passed\n";
    return failures == 0 ? 0 : 1;
}
