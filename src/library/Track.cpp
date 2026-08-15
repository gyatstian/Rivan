// Track.cpp
// Creates stable, dependency-free track records from filesystem paths.
#include "Track.h"

#include <shobjidl.h>
#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <cwctype>
#include <system_error>

namespace rivan::library {
namespace {

std::wstring ReadProperty(IPropertyStore* properties, REFPROPERTYKEY key) {
    if (properties == nullptr) return {};
    PROPVARIANT value{};
    PropVariantInit(&value);
    std::wstring result;
    if (SUCCEEDED(properties->GetValue(key, &value))) {
        PWSTR text = nullptr;
        if (SUCCEEDED(PropVariantToStringAlloc(value, &text)) && text != nullptr) {
            result = text;
            CoTaskMemFree(text);
        }
    }
    PropVariantClear(&value);
    return result;
}

void ReadEmbeddedMetadata(const std::filesystem::path& path, Track& track) {
    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    if (FAILED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT,
                                                  IID_PPV_ARGS(properties.GetAddressOf())))) {
        return;
    }
    track.title = ReadProperty(properties.Get(), PKEY_Title);
    track.artist = ReadProperty(properties.Get(), PKEY_Music_Artist);
    track.album = ReadProperty(properties.Get(), PKEY_Music_AlbumTitle);
}

std::filesystem::path NormalizePath(const std::filesystem::path& path) {
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    if (ec) {
        ec.clear();
        normalized = std::filesystem::absolute(path, ec);
    }
    return (ec ? path : normalized).lexically_normal();
}

TrackId PathId(const std::filesystem::path& path) noexcept {
    constexpr std::uint64_t offset = 14695981039346656037ull;
    const auto hash = FnvHashFoldCase(path.generic_wstring(), offset);
    return hash == 0 ? 1 : hash;
}

std::wstring Lowercase(std::wstring value) noexcept {
    for (auto& character : value) {
        character = static_cast<wchar_t>(std::towlower(character));
    }
    return value;
}

} // namespace

std::uint64_t FnvHashFoldCase(std::wstring_view text, std::uint64_t seed) noexcept {
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t hash = seed;
    for (wchar_t character : text) {
#ifdef _WIN32
        character = static_cast<wchar_t>(std::towlower(character));
#endif
        const auto value = static_cast<std::uint32_t>(character);
        for (unsigned shift = 0; shift < 32; shift += 8) {
            hash ^= static_cast<unsigned char>((value >> shift) & 0xffu);
            hash *= prime;
        }
    }
    return hash;
}

bool Track::IsAvailable() const noexcept {
    std::error_code ec;
    return std::filesystem::is_regular_file(filePath, ec) && !ec;
}

bool Track::IsSupportedFile(const std::filesystem::path& path) noexcept {
    const auto extension = Lowercase(path.extension().wstring());
    return extension == L".mp3" || extension == L".wav" || extension == L".flac" ||
           extension == L".mp4" || extension == L".m4a" || extension == L".opus" ||
           extension == L".webm" || extension == L".ogg" || extension == L".aac" ||
           extension == L".m4v";
}

bool Track::IsAudioFile(const std::filesystem::path& path) noexcept {
    const auto extension = Lowercase(path.extension().wstring());
    return extension == L".mp3" || extension == L".wav" || extension == L".flac" ||
           extension == L".m4a" || extension == L".opus" || extension == L".ogg" ||
           extension == L".aac";
}

Track Track::FromFile(const std::filesystem::path& path) {
    Track track;
    track.filePath = NormalizePath(path);
    track.id = PathId(track.filePath);
    ReadEmbeddedMetadata(track.filePath, track);
    if (track.title.empty()) track.title = track.filePath.stem().wstring();
    return track;
}

Track Track::FromPathOnly(const std::filesystem::path& path) {
    Track track;
    track.filePath = NormalizePath(path);
    track.id = PathId(track.filePath);
    if (track.title.empty()) track.title = track.filePath.stem().wstring();
    return track;
}

} // namespace rivan::library
