// Win32Ui.MetadataEditor.h
// Metadata field editing: prompt dialog and file property writing.
#pragma once
#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <string>

namespace rivan::ui {

enum class TrackMetadataField : std::uint8_t {
    Author,
    Album,
    Genre,
    Year
};

// Returns the label string for the field (e.g. L"Change author", L"Change album" etc.).
[[nodiscard]] const wchar_t* MetadataFieldLabel(TrackMetadataField field) noexcept;

// Shows a modal dialog prompting the user to enter a new value for the given field.
// On input, |value| holds the current field value; on success it is replaced with the
// user's input. Returns false if the user cancelled (pressed Esc, Cancel, or closed the
// dialog), true if the user confirmed.
[[nodiscard]] bool PromptTrackMetadataValue(HWND owner, TrackMetadataField field,
                                            std::wstring& value);

// Returns true for file formats that have a native Windows writable property handler.
[[nodiscard]] bool HandledByWindowsPropertyStore(const std::wstring& filePath) noexcept;

// Writes a text metadata property to an audio file using Windows IPropertyStore.
// Returns true if the write succeeded.
[[nodiscard]] bool WriteTrackMetadataValue(const std::wstring& filePath,
                                           TrackMetadataField field,
                                           const std::wstring& value);

// Locates ffmpeg.exe in the tools directory under LocalDataRoot.
[[nodiscard]] std::filesystem::path LocateFfmpeg();

// Writes a text metadata property using ffmpeg (fallback for formats IPropertyStore cannot handle).
[[nodiscard]] bool WriteTrackMetadataValueFfmpeg(const std::wstring& filePath,
                                                 TrackMetadataField field,
                                                 const std::wstring& value);

// Writes cover art using ffmpeg (fallback for formats IPropertyStore cannot handle).
[[nodiscard]] bool WriteCoverArtFfmpeg(const std::wstring& filePath,
                                       const std::wstring& imagePath);

} // namespace rivan::ui