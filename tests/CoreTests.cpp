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
    if (failures == 0) std::cout << "Rivan core tests passed\n";
    return failures == 0 ? 0 : 1;
}
