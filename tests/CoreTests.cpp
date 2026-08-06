// CoreTests.cpp
// Dependency-free behavioral checks for Rivan's deterministic library, queue, and FFT core.
// The test executable returns nonzero at the first failed invariant.
#include "../src/audio/AudioAnalysisBuffer.h"
#include "../src/config/SettingsManager.h"
#include "../src/library/LibraryScanner.h"
#include "../src/playlist/PlaybackQueue.h"
#include "../src/playlist/PlaylistManager.h"
#include "../src/skin/Skin.h"
#include "../src/visualization/Visualization.h"
#include "../src/ui/UiModule.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <span>
#include <string>
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
    Check(buffer.StoredFrames() == 128, "analysis buffer stores pushed frames");

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

void TestFilePreviewSettingRoundTrip() {
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
    settings.trackCoverArtEnabled = false;
    settings.filePreviewEnabled = false;
    settings.startAtStartup = true;
    settings.exitToTray = true;
    settings.discordShowGithubButton = true;
    std::string error;
    Check(writer.SetSettings(settings, &error), "file preview setting accepts false");
    Check(writer.SaveSettings(&error), "file preview setting saves");

    rivan::config::SettingsManager reader(settingsFile, sessionFile);
    Check(reader.LoadSettings(&error), "file preview setting reloads");
    Check(reader.Settings().filePreviewEnabled == false,
           "file preview disabled state survives settings round-trip");
    Check(reader.Settings().trackCoverArtEnabled == false,
           "track cover setting survives settings round-trip");
    Check(reader.Settings().startAtStartup,
          "start at startup survives settings round-trip");
    Check(reader.Settings().exitToTray,
          "exit to tray survives settings round-trip");
    Check(reader.Settings().discordShowGithubButton,
          "Discord GitHub button setting survives settings round-trip");

    settings.filePreviewEnabled = true;
    settings.trackCoverArtEnabled = true;
    Check(writer.SetSettings(settings, &error) && writer.SaveSettings(&error),
           "file preview setting accepts true");
    Check(reader.LoadSettings(&error) && reader.Settings().filePreviewEnabled &&
              reader.Settings().trackCoverArtEnabled,
           "file preview and track cover settings enable after round-trip");
    std::filesystem::remove_all(root, ec);
}

void TestUiModuleRegistry() {
    using rivan::ui::ModuleId;
    using rivan::ui::UiModuleRegistry;

    const auto modules = UiModuleRegistry::Modules();
    Check(modules.size() == 5, "the built-in UI module registry contains five sections");
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
    Check(UiModuleRegistry::Get(ModuleId::VideoPreview).Key() == "video_preview",
          "the video preview section has a stable key");
    Check(UiModuleRegistry::Get(ModuleId::VideoPreview).Title() == L"VIDEO PREVIEW",
          "the video preview section has a stable title");

    auto layout = rivan::ui::ModuleLayout::Defaults();
    Check(layout.Find(ModuleId::Rivan)->width > 0.0F,
          "module defaults provide normalized geometry");
    const auto* videoPreview = layout.Find(ModuleId::VideoPreview);
    Check(videoPreview != nullptr && std::abs(videoPreview->x - 0.46F) < 0.001F &&
              std::abs(videoPreview->y - 0.68F) < 0.001F &&
              std::abs(videoPreview->width - 0.54F) < 0.001F &&
              std::abs(videoPreview->height - 0.30F) < 0.001F,
          "module defaults reserve a standalone video preview section");
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
    resized.ResizeSnapGroup(ModuleId::Rivan, 0.60F, 0.15F, true, false, false, false);
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
    collapsibleResize.ResizeSnapGroup(ModuleId::AllMusic, 0.40F, 0.5F,
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
          "client resize preserves module pixels while surrounding space remains");
    canvasModule = resizeCanvas.Find(ModuleId::Rivan);
    Check(std::abs(canvasModule->x - (200.0F / 700.0F)) < 0.001F &&
              std::abs(canvasModule->width - (500.0F / 700.0F)) < 0.001F &&
              std::abs(canvasModule->height - 0.40F) < 0.001F,
          "unused window space is removed before modules are squeezed");
    Check(!resizeCanvas.PreservePixelGeometry(700.0F, 800.0F, 400.0F, 800.0F),
          "client resize leaves normalized layout unchanged once modules no longer fit");

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
}

void TestCollapsibleSnapping() {
    using rivan::ui::ModuleCollapseMode;
    using rivan::ui::ModuleCollapseSide;
    using rivan::ui::ModuleDropZone;
    using rivan::ui::ModuleId;
    using rivan::ui::ModuleLayout;

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

    std::filesystem::remove_all(root, ec);
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
    TestFilePreviewSettingRoundTrip();
    TestUiModuleRegistry();
    TestWindowSnapping();
    TestCollapsibleSnapping();
    TestSnappingWithExistingLayoutConflict();
    TestModuleLayoutSessionRoundTrip();
    if (failures == 0) std::cout << "Rivan core tests passed\n";
    return failures == 0 ? 0 : 1;
}
