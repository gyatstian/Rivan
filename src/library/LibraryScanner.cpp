// LibraryScanner.cpp
// Builds deterministic All Music and per-directory playlist snapshots.
#include "LibraryScanner.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#endif

namespace rivan::library {
namespace {

// Lowercase sorting key, computed once per element instead of inside comparators.
std::wstring LowerKey(const std::filesystem::path& path) {
    auto value = path.generic_wstring();
#ifdef _WIN32
    for (auto& character : value) {
        character = static_cast<wchar_t>(std::towlower(character));
    }
#endif
    return value;
}

playlist::PlaylistId DirectoryPlaylistId(const std::filesystem::path& path) noexcept {
    constexpr std::uint64_t offset = 14695981039346656037ull;
    constexpr std::wstring_view domain = L"rivan:directory:";
    const auto hash = FnvHashFoldCase(path.generic_wstring(), FnvHashFoldCase(domain, offset));
    // Reserve low ids: 1 = All Music, 2 = virtual Youtube browser.
    return hash <= playlist::YoutubePlaylistId ? hash + 3 : hash;
}

std::filesystem::path NormalizeRoot(const std::filesystem::path& root) {
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        ec.clear();
        normalized = std::filesystem::absolute(root, ec);
    }
    return (ec ? root : normalized).lexically_normal();
}

bool IsGenuineDescendant(const std::filesystem::path& candidate,
                         const std::filesystem::path& ancestor) {
    std::error_code ec;
    const auto relative = std::filesystem::relative(candidate, ancestor, ec);
    if (ec || relative.empty() || relative == L".") return false;
    const auto first = relative.begin();
    return first != relative.end() && *first != L"..";
}

std::wstring LowerExtension(const std::filesystem::path& path) {
    auto extension = path.extension().wstring();
    for (auto& character : extension) {
        character = static_cast<wchar_t>(std::towlower(character));
    }
    return extension;
}

bool IsOpusPath(const std::filesystem::path& path) {
    return LowerExtension(path) == L".opus";
}

std::uint16_t ReadLittleEndian16(const unsigned char* data) noexcept {
    return static_cast<std::uint16_t>(data[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8);
}

std::uint64_t ReadLittleEndian64(const unsigned char* data) noexcept {
    std::uint64_t value = 0;
    for (int shift = 7; shift >= 0; --shift) {
        value = (value << 8) | data[shift];
    }
    return value;
}

bool MatchesBytes(const std::span<const unsigned char> data,
                  const std::size_t offset,
                  const std::string_view text) noexcept {
    if (offset + text.size() > data.size()) return false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (data[offset + index] != static_cast<unsigned char>(text[index])) return false;
    }
    return true;
}

std::optional<std::uint16_t> ReadOpusPreSkip(const std::filesystem::path& path) noexcept {
    constexpr std::size_t kHeaderSearchBytes = 64 * 1024;

    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;

    std::vector<unsigned char> buffer(kHeaderSearchBytes);
    stream.read(reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(buffer.size()));
    buffer.resize(static_cast<std::size_t>(stream.gcount()));
    const std::span<const unsigned char> bytes{buffer};

    for (std::size_t offset = 0; offset + 12 <= bytes.size(); ++offset) {
        if (MatchesBytes(bytes, offset, "OpusHead")) {
            return ReadLittleEndian16(bytes.data() + offset + 10);
        }
    }
    return std::nullopt;
}

std::optional<std::uint64_t> ReadLastOggGranulePosition(const std::filesystem::path& path,
                                                        const std::uintmax_t fileSize,
                                                        std::stop_token stop) noexcept {
    constexpr std::uintmax_t kSearchChunkBytes = 64 * 1024;
    constexpr auto kUnknownGranule = (std::numeric_limits<std::uint64_t>::max)();

    if (fileSize < 27) return std::nullopt;

    std::ifstream stream(path, std::ios::binary);
    if (!stream) return std::nullopt;

    std::vector<unsigned char> buffer;
    std::uintmax_t chunkEnd = fileSize;
    while (chunkEnd >= 27) {
        if (stop.stop_requested()) return std::nullopt;

        const auto chunkStart = chunkEnd > kSearchChunkBytes ? chunkEnd - kSearchChunkBytes : 0;
        const auto bytesToRead = static_cast<std::size_t>(chunkEnd - chunkStart);
        buffer.assign(bytesToRead, 0);

        stream.clear();
        stream.seekg(static_cast<std::streamoff>(chunkStart), std::ios::beg);
        if (!stream) return std::nullopt;
        stream.read(reinterpret_cast<char*>(buffer.data()),
                    static_cast<std::streamsize>(buffer.size()));
        const auto bytesRead = static_cast<std::size_t>(stream.gcount());
        if (bytesRead < 27) return std::nullopt;

        const std::span<const unsigned char> bytes{buffer.data(), bytesRead};
        for (std::size_t offset = bytes.size() - 27;; --offset) {
            if (MatchesBytes(bytes, offset, "OggS") && offset + 5 <= bytes.size() &&
                bytes[offset + 4] == 0) {
                const auto pageOffset = chunkStart + offset;
                const auto segmentCount = static_cast<std::uintmax_t>(bytes[offset + 26]);
                if (pageOffset + 27 + segmentCount <= fileSize) {
                    const auto granule = ReadLittleEndian64(bytes.data() + offset + 6);
                    if (granule != kUnknownGranule) return granule;
                }
            }
            if (offset == 0) break;
        }

        if (chunkStart == 0) break;
        chunkEnd = chunkStart + 3; // Preserve enough overlap for an OggS marker on boundary.
    }
    return std::nullopt;
}

double ProbeOggOpusDurationSeconds(const std::filesystem::path& path,
                                   const std::uintmax_t fileSize,
                                   std::stop_token stop) noexcept {
    if (!IsOpusPath(path)) return 0.0;
    const auto preSkip = ReadOpusPreSkip(path);
    if (!preSkip) return 0.0;
    const auto granule = ReadLastOggGranulePosition(path, fileSize, stop);
    if (!granule || *granule <= *preSkip) return 0.0;
    return static_cast<double>(*granule - *preSkip) / 48000.0;
}

std::wstring DirectoryName(const std::filesystem::path& root,
                           const std::filesystem::path& directory) {
    std::error_code ec;
    const auto relative = std::filesystem::relative(directory, root, ec);
    if (!ec && !relative.empty() && relative != L".") {
        return relative.generic_wstring();
    }
    auto name = directory.filename().wstring();
    return name.empty() ? directory.wstring() : std::move(name);
}

// Duration cache persisted across runs so unchanged files skip the slow Media
// Foundation container open on subsequent startup scans. Keyed by path; entries
// are invalidated by changed modification time or size. Access is serialized by
// cacheMutex; only the single scan thread reads/writes at run time.
constexpr std::uint32_t DurationCacheMagic = 0x44524156u;  // "VARD" bytes, arbitrary
constexpr std::uint32_t DurationCacheVersion = 1u;
constexpr std::size_t DurationCacheMaxEntries = 100000;

struct DurationCacheEntry {
    std::filesystem::file_time_type modified{};
    std::uintmax_t size{};
    double seconds{};
};

struct DurationProbeResult {
    double seconds{};
    std::uintmax_t fileSize{};
};

std::mutex gDurationCacheMutex;
std::map<std::filesystem::path, DurationCacheEntry> gDurationCache;
bool gDurationCacheLoaded = false;
std::filesystem::path gDurationCachePath;

// Binary format: magic u32, version u32, count u64, then per entry:
//   pathCharCount u32 (chars INCLUDING null), wchar_t path[pathCharCount],
//   modifiedRep i64, size u64, seconds f64.
// Corrupt/partial/older files are ignored and simply yield an empty cache; the
// next completed scan rewrites the file.
void LoadDurationCacheFromDisk() {
    std::scoped_lock lock(gDurationCacheMutex);
    if (gDurationCacheLoaded || gDurationCachePath.empty()) return;
    gDurationCacheLoaded = true;
    std::ifstream in(gDurationCachePath, std::ios::binary);
    if (!in) return;
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint64_t count = 0;
    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    if (!in || magic != DurationCacheMagic || version != DurationCacheVersion) return;
    if (count > DurationCacheMaxEntries) return;  // guard against garbage files
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint32_t charCount = 0;
        in.read(reinterpret_cast<char*>(&charCount), sizeof(charCount));
        if (!in || charCount == 0 || charCount > 65536) return;  // guard against corruption
        std::wstring pathText(charCount, L'\0');
        in.read(reinterpret_cast<char*>(pathText.data()), static_cast<std::streamsize>(charCount * sizeof(wchar_t)));
        if (!in) return;
        pathText.pop_back();  // remove the null terminator
        std::int64_t modifiedRep = 0;
        std::uint64_t size = 0;
        double seconds = 0.0;
        in.read(reinterpret_cast<char*>(&modifiedRep), sizeof(modifiedRep));
        in.read(reinterpret_cast<char*>(&size), sizeof(size));
        in.read(reinterpret_cast<char*>(&seconds), sizeof(seconds));
        if (!in) return;
        gDurationCache.insert_or_assign(
            std::filesystem::path(std::move(pathText)),
            DurationCacheEntry{std::filesystem::file_time_type(
                                   std::filesystem::file_time_type::duration(modifiedRep)),
                               size, seconds});
    }
}

void SaveDurationCacheToDisk() {
    std::scoped_lock lock(gDurationCacheMutex);
    if (gDurationCachePath.empty()) return;
    // Copy under lock so a concurrent probe cannot invalidate iterators, then write
    // outside the critical section would be ideal, but the scan thread is the only
    // caller here; a small snapshot under the lock is simplest and safe.
    std::vector<std::pair<std::filesystem::path, DurationCacheEntry>> snapshot;
    snapshot.reserve(gDurationCache.size());
    for (auto& [path, entry] : gDurationCache) snapshot.emplace_back(path, entry);
    if (snapshot.empty()) return;
    std::ofstream out(gDurationCachePath, std::ios::binary | std::ios::trunc);
    if (!out) return;
    const std::uint32_t magic = DurationCacheMagic;
    const std::uint32_t version = DurationCacheVersion;
    const std::uint64_t count = static_cast<std::uint64_t>(snapshot.size());
    out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const auto& [path, entry] : snapshot) {
        const auto text = path.wstring();
        const std::uint32_t charCount = static_cast<std::uint32_t>(text.size() + 1);
        out.write(reinterpret_cast<const char*>(&charCount), sizeof(charCount));
        out.write(reinterpret_cast<const char*>(text.c_str()),
                   static_cast<std::streamsize>(charCount * sizeof(wchar_t)));
        const auto modifiedRep =
            static_cast<std::int64_t>(entry.modified.time_since_epoch().count());
        out.write(reinterpret_cast<const char*>(&modifiedRep), sizeof(modifiedRep));
        const std::uint64_t size = entry.size;
        out.write(reinterpret_cast<const char*>(&size), sizeof(size));
        const double seconds = entry.seconds;
        out.write(reinterpret_cast<const char*>(&seconds), sizeof(seconds));
    }
}

#ifdef _WIN32
// Scoped Media Foundation lifetime for the scan thread. Media Foundation must be
// started per thread that creates source readers; the audio backend runs on its
// own thread, so the scanner initializes its own instance.
class ScopedMediaFoundation final {
public:
    ScopedMediaFoundation() noexcept {
        comOk_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        mfOk_ = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }
    ~ScopedMediaFoundation() {
        if (mfOk_) MFShutdown();
        if (comOk_) CoUninitialize();
    }
    [[nodiscard]] bool Ready() const noexcept { return comOk_ && mfOk_; }
    ScopedMediaFoundation(const ScopedMediaFoundation&) = delete;
    ScopedMediaFoundation& operator=(const ScopedMediaFoundation&) = delete;

private:
    bool comOk_{};
    bool mfOk_{};
};

// Reads container metadata only (no decode) to recover a track's duration,
// reusing the persisted disk cache when the file's size and mtime are unchanged.
DurationProbeResult ProbeDurationSeconds(const std::filesystem::path& path,
                                         std::stop_token stop) noexcept {
    if (stop.stop_requested()) return {};
    static std::once_flag cacheLoadFlag;
    std::call_once(cacheLoadFlag, []() { LoadDurationCacheFromDisk(); });
    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(path, ec);
    if (ec) return {};
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) return {};
    {
        std::scoped_lock lock(gDurationCacheMutex);
        const auto found = gDurationCache.find(path);
        if (found != gDurationCache.end() && found->second.modified == modified &&
            found->second.size == size) {
            return DurationProbeResult{found->second.seconds, found->second.size};
        }
    }
    double seconds = 0.0;

    if (stop.stop_requested()) return {};
    IMFSourceReader* reader = nullptr;
    if (SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader)) && reader != nullptr) {
        PROPVARIANT value;
        PropVariantInit(&value);
        if (SUCCEEDED(reader->GetPresentationAttribute(
                static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &value))) {
            std::uint64_t hundredNs = 0;
            if (value.vt == VT_UI8) {
                hundredNs = value.uhVal.QuadPart;
            } else if (value.vt == VT_I8 && value.hVal.QuadPart > 0) {
                hundredNs = static_cast<std::uint64_t>(value.hVal.QuadPart);
            }
            seconds = static_cast<double>(hundredNs) / 1e7;
        }
        PropVariantClear(&value);
        reader->Release();
    }

    if (seconds <= 0.0) seconds = ProbeOggOpusDurationSeconds(path, size, stop);
    if (stop.stop_requested()) return {};
    {
        std::scoped_lock lock(gDurationCacheMutex);
        // Bound retained file metadata across long-running sessions and removable-media
        // scans. Evict a small slice rather than the whole map so a library near the
        // cap does not re-probe every file on the next scan.
        if (gDurationCache.size() >= DurationCacheMaxEntries) {
            std::size_t evict = gDurationCache.size() / 16;
            if (evict == 0) evict = 1;
            auto it = gDurationCache.begin();
            while (evict-- > 0 && it != gDurationCache.end()) {
                it = gDurationCache.erase(it);
            }
        }
        gDurationCache.insert_or_assign(path, DurationCacheEntry{modified, size, seconds});
    }
    return DurationProbeResult{seconds, size};
}
#endif

} // namespace

void LibraryScanner::SetDurationCachePath(std::filesystem::path path) {
    std::scoped_lock lock(gDurationCacheMutex);
    gDurationCachePath = std::move(path);
    gDurationCacheLoaded = false;
}

void LibraryScanner::SaveDurationCache() {
    SaveDurationCacheToDisk();
}

bool LibraryScanner::IsSupported(const std::filesystem::path& path) noexcept {
    return Track::IsSupportedFile(path);
}

LibraryScanResult LibraryScanner::Scan(const std::filesystem::path& requestedRoot,
                                      std::stop_token stop) const {
    const std::array roots{requestedRoot};
    return Scan(std::span<const std::filesystem::path>(roots), stop);
}

LibraryScanResult LibraryScanner::Scan(std::span<const std::filesystem::path> requestedRoots,
                                      std::stop_token stop) const {
    LibraryScanResult result;
#ifdef _WIN32
    const ScopedMediaFoundation mediaFoundation;
    const bool canProbeDurations = mediaFoundation.Ready();
#endif

    // Normalize roots, dropping empties and exact duplicates. Descendant filtering below
    // removes overlapping roots without treating same-drive siblings as descendants.
    std::vector<std::filesystem::path> roots;
    for (const auto& requested : requestedRoots) {
        if (requested.empty()) continue;
        auto normalized = NormalizeRoot(requested);
        auto contained = false;
        for (const auto& accepted : roots) {
            if (IsGenuineDescendant(normalized, accepted)) {
                contained = true;
                break;
            }
        }
        if (contained) continue;
        if (std::find(roots.begin(), roots.end(), normalized) == roots.end()) {
            roots.push_back(std::move(normalized));
        }
    }
    // With [child, parent] ordering the acceptance loop keeps both, so the child's
    // files would be scanned twice. Drop any later, broader root that contains an
    // earlier accepted one; the earlier (deeper) root already covers its files.
    std::vector<std::filesystem::path> survivingRoots;
    for (auto& root : roots) {
        auto containsEarlier = false;
        for (const auto& earlier : survivingRoots) {
            if (IsGenuineDescendant(earlier, root)) {
                containsEarlier = true;
                break;
            }
        }
        if (!containsEarlier) survivingRoots.push_back(std::move(root));
    }
    roots = std::move(survivingRoots);
    if (!roots.empty()) result.root = roots.front();

    // Direct (non-recursive) tracks per directory, and the set of directories that are
    // scan roots so hierarchy links can stop climbing there.
    std::map<std::filesystem::path, std::vector<TrackId>> directoryTracks;
    std::set<std::filesystem::path> rootSet(roots.begin(), roots.end());

    for (const auto& root : roots) {
        if (stop.stop_requested()) return {};
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) continue;
        // Ensure every root appears as a playlist even when it holds no direct files.
        directoryTracks.try_emplace(root);

        std::vector<std::filesystem::path> files;
        const auto options = std::filesystem::directory_options::skip_permission_denied;
        std::filesystem::recursive_directory_iterator iterator(root, options, ec);
        const std::filesystem::recursive_directory_iterator end;
        if (ec) ec.clear();
        while (iterator != end) {
            if (stop.stop_requested()) return {};
            const auto entryPath = iterator->path();
            if (iterator->is_directory(ec) && !ec) {
                // Normalize folder keys to match Track::FromFile parent paths.
                directoryTracks.try_emplace(NormalizeRoot(entryPath));
            } else if (!ec && iterator->is_regular_file(ec) && !ec && IsSupported(entryPath)) {
                files.push_back(entryPath);
            } else if (ec) ec.clear();
            iterator.increment(ec);
            if (ec) ec.clear();
        }

        // Fold each path once; sorting then compares precomputed lowercase keys.
        std::vector<std::pair<std::filesystem::path, std::wstring>> keyedFiles;
        keyedFiles.reserve(files.size());
        for (const auto& file : files) {
            keyedFiles.emplace_back(file, LowerKey(file));
        }
        std::sort(keyedFiles.begin(), keyedFiles.end(), [](const auto& left, const auto& right) {
            return left.second < right.second;
        });
        files.clear();
        files.reserve(keyedFiles.size());
        for (auto& keyed : keyedFiles) {
            files.push_back(std::move(keyed.first));
        }
        for (const auto& file : files) {
            if (stop.stop_requested()) return {};
            auto track = Track::FromFile(file);
#ifdef _WIN32
            std::uintmax_t probedBytes = 0;
            if (canProbeDurations) {
                const auto probe = ProbeDurationSeconds(track.filePath, stop);
                track.durationSeconds = probe.seconds;
                probedBytes = probe.fileSize;
            }
#endif
            if (track.durationSeconds > 0.0 && probedBytes > 0) {
                const double kbps = static_cast<double>(probedBytes) * 8.0 /
                                    (track.durationSeconds * 1000.0);
                if (std::isfinite(kbps) && kbps > 0.0 && kbps <= 100000.0) {
                    track.bitrateKbps = static_cast<int>(std::lround(kbps));
                }
            }
            if (stop.stop_requested()) return {};
            // Parent already normalized via Track::FromFile; create node if walk missed it.
            const auto parent = track.filePath.parent_path();
            directoryTracks.try_emplace(parent);
            directoryTracks[parent].push_back(track.id);
            result.tracks.push_back(std::move(track));
        }
    }

    // Sort tracks by precomputed lowercase file-path keys (moved back in order).
    std::vector<std::pair<std::size_t, std::wstring>> keyedTrackOrder;
    keyedTrackOrder.reserve(result.tracks.size());
    for (std::size_t index = 0; index < result.tracks.size(); ++index) {
        keyedTrackOrder.emplace_back(index, LowerKey(result.tracks[index].filePath));
    }
    std::sort(keyedTrackOrder.begin(), keyedTrackOrder.end(), [](const auto& left, const auto& right) {
        return left.second < right.second;
    });
    std::vector<library::Track> sortedTracks;
    sortedTracks.reserve(result.tracks.size());
    for (const auto& [index, key] : keyedTrackOrder) {
        (void)key;
        sortedTracks.push_back(std::move(result.tracks[index]));
    }
    result.tracks = std::move(sortedTracks);

    // All Music: the recursive union across every root.
    playlist::Playlist allMusic;
    allMusic.id = playlist::AllMusicPlaylistId;
    allMusic.name = L"All Music";
    allMusic.kind = playlist::PlaylistKind::AllMusic;
    allMusic.directory = result.root;
    allMusic.trackIds.reserve(result.tracks.size());
    for (const auto& track : result.tracks) {
        allMusic.trackIds.push_back(track.id);
    }
    result.playlists.push_back(std::move(allMusic));

    // Locate the owning root for a directory so root-relative names and depth resolve.
    const auto owningRoot = [&rootSet](std::filesystem::path directory) -> std::filesystem::path {
        for (;;) {
            if (rootSet.contains(directory)) return directory;
            auto parent = directory.parent_path();
            if (parent == directory || parent.empty()) return {};
            directory = std::move(parent);
        }
    };

    std::vector<playlist::Playlist> directories;
    directories.reserve(directoryTracks.size());
    for (auto& [directory, trackIds] : directoryTracks) {
        const auto root = owningRoot(directory);
        playlist::Playlist folder;
        folder.id = DirectoryPlaylistId(directory);
        folder.name = root.empty() ? DirectoryName(directory, directory)
                                   : (directory == root ? root.filename().empty()
                                                              ? root.wstring()
                                                              : root.filename().wstring()
                                                        : directory.filename().wstring());
        folder.kind = playlist::PlaylistKind::Directory;
        folder.directory = directory;
        folder.trackIds = std::move(trackIds);
        // Depth relative to the owning root; parent is the enclosing folder (0 for roots).
        if (!root.empty() && directory != root) {
            folder.parentId = DirectoryPlaylistId(directory.parent_path());
            std::error_code ec;
            const auto relative = std::filesystem::relative(directory, root, ec);
            folder.depth = ec ? 1U : static_cast<std::uint32_t>(
                std::distance(relative.begin(), relative.end()));
        }
        directories.push_back(std::move(folder));
    }
    // Sort folder playlists by precomputed lowercase directory keys (moved back in order).
    std::vector<std::pair<std::size_t, std::wstring>> keyedDirectoryOrder;
    keyedDirectoryOrder.reserve(directories.size());
    for (std::size_t index = 0; index < directories.size(); ++index) {
        keyedDirectoryOrder.emplace_back(index, LowerKey(directories[index].directory));
    }
    std::sort(keyedDirectoryOrder.begin(), keyedDirectoryOrder.end(),
              [](const auto& left, const auto& right) {
                  return left.second < right.second;
              });
    std::vector<playlist::Playlist> sortedDirectories;
    sortedDirectories.reserve(directories.size());
    for (const auto& [index, key] : keyedDirectoryOrder) {
        (void)key;
        sortedDirectories.push_back(std::move(directories[index]));
    }
    directories = std::move(sortedDirectories);
    result.playlists.insert(result.playlists.end(),
                            std::make_move_iterator(directories.begin()),
                            std::make_move_iterator(directories.end()));
    return result;
}

} // namespace rivan::library
