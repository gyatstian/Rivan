// Win32Ui.MetadataEditor.cpp
// Modal prompt dialog for metadata text editing + IPropertyStore writing.
#include "Win32Ui.MetadataEditor.h"
#include "../core/AppPaths.h"

#include <Windows.h>
#include <shobjidl.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#pragma comment(lib, "propsys.lib")

namespace rivan::ui {
namespace {

// ---------------------------------------------------------------------------
// Control identifiers used by the prompt dialog.
// ---------------------------------------------------------------------------
constexpr UINT kIdEdit = 100;
constexpr UINT kIdOk = IDOK;
constexpr UINT kIdCancel = IDCANCEL;

// Per-instance context passed to the dialog window procedure.
struct PromptContext {
    TrackMetadataField field;
    std::wstring* value;   // in/out: current value on entry, result on confirm
    HWND editControl{};    // cached after creation
    bool confirmed = false;
};

// Dialog window class name – registered once on first use.
constexpr wchar_t kDialogClassName[] = L"RivanMetadataInput";

// Map a TrackMetadataField to the label shown above the edit box.
const wchar_t* FieldName(TrackMetadataField field) noexcept {
    switch (field) {
    case TrackMetadataField::Author: return L"Author";
    case TrackMetadataField::Album:  return L"Album";
    case TrackMetadataField::Genre:  return L"Genre";
    case TrackMetadataField::Year:   return L"Year";
    }
    return L"Value";
}

// Map a TrackMetadataField to the corresponding Windows property key.
REFPROPERTYKEY FieldToPKey(TrackMetadataField field) noexcept {
    switch (field) {
    case TrackMetadataField::Author: return PKEY_Music_Artist;
    case TrackMetadataField::Album:  return PKEY_Music_AlbumTitle;
    case TrackMetadataField::Genre:  return PKEY_Music_Genre;
    case TrackMetadataField::Year:   return PKEY_Media_Year;
    }
    return PKEY_Music_Artist; // fallback, should not reach
}

// ---------------------------------------------------------------------------
// One-time registration of the prompt dialog window class.
// ---------------------------------------------------------------------------
bool RegisterDialogClass() noexcept {
    static bool registered = false;
    if (registered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
        switch (msg) {
        case WM_CREATE: {
            auto* ctx = reinterpret_cast<PromptContext*>(
                reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

            const HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            constexpr int margin = 10;
            constexpr int labelHeight = 20;
            constexpr int editTop = margin + labelHeight + 4;
            constexpr int editHeight = 24;
            constexpr int buttonWidth = 80;
            constexpr int buttonHeight = 26;
            constexpr int buttonTop = 140 - margin - buttonHeight;
            constexpr int clientWidth = 350;

            // Static label with the field name.
            CreateWindowExW(0, L"STATIC", FieldName(ctx->field),
                            WS_CHILD | WS_VISIBLE,
                            margin, margin, clientWidth - 2 * margin, labelHeight,
                            hwnd, nullptr, nullptr, nullptr);

            // Edit control – single line, pre-filled with the current value,
            // contents selected so typing replaces it immediately.
            ctx->editControl = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", ctx->value->c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                margin, editTop, clientWidth - 2 * margin, editHeight,
                hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdEdit)),
                nullptr, nullptr);
            if (ctx->editControl) {
                SendMessageW(ctx->editControl, WM_SETFONT,
                             reinterpret_cast<WPARAM>(font), TRUE);
                SendMessageW(ctx->editControl, EM_SETSEL, 0, -1);
            }

            // OK button – the default push button so Enter confirms.
            const HWND okButton = CreateWindowExW(
                0, L"BUTTON", L"OK",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                clientWidth - margin - buttonWidth - margin - buttonWidth,
                buttonTop, buttonWidth, buttonHeight,
                hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdOk)),
                nullptr, nullptr);
            if (okButton) {
                SendMessageW(okButton, WM_SETFONT,
                             reinterpret_cast<WPARAM>(font), TRUE);
            }
            SendMessageW(hwnd, DM_SETDEFID, kIdOk, 0);

            // Cancel button.
            const HWND cancelButton = CreateWindowExW(
                0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                clientWidth - margin - buttonWidth,
                buttonTop, buttonWidth, buttonHeight,
                hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdCancel)),
                nullptr, nullptr);
            if (cancelButton) {
                SendMessageW(cancelButton, WM_SETFONT,
                             reinterpret_cast<WPARAM>(font), TRUE);
            }

            // IsDialogMessageW maps Esc to IDCANCEL when a cancel control exists.
            return 0;
        }

        case WM_COMMAND: {
            const auto id = LOWORD(wp);
            if (id == kIdOk) {
                auto* ctx = reinterpret_cast<PromptContext*>(
                    GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (ctx != nullptr && ctx->editControl != nullptr) {
                    const int length = GetWindowTextLengthW(ctx->editControl) + 1;
                    ctx->value->resize(static_cast<std::size_t>(length) - 1);
                    GetWindowTextW(ctx->editControl, ctx->value->data(), length);
                    ctx->confirmed = true;
                }
                DestroyWindow(hwnd);
            } else if (id == kIdCancel) {
                DestroyWindow(hwnd);
            }
            return 0;
        }

        case WM_CLOSE:
            // Closing from the system menu behaves like Cancel.
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(kIdCancel, 0), 0);
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wp, lp);
    };
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kDialogClassName;

    registered = RegisterClassExW(&wc) != 0;
    return registered;
}

// ---------------------------------------------------------------------------
// ffmpeg fallback helpers for formats Windows IPropertyStore cannot handle
// (e.g. .opus, .ogg, .flac). ffmpeg lives at %LOCALAPPDATA%\Rivan\tools\ffmpeg.exe.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// METADATA_BLOCK_PICTURE helpers for Vorbis-comment cover art embedding.
// ---------------------------------------------------------------------------

struct ImageDimensions { std::uint32_t width = 0; std::uint32_t height = 0; };

bool ReadImageDimensions(const std::wstring& filePath, ImageDimensions& dims) {
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(filePath.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, frame.GetAddressOf());
    if (FAILED(hr)) return false;

    UINT w = 0, h = 0;
    hr = frame->GetSize(&w, &h);
    if (FAILED(hr)) return false;
    dims.width = static_cast<std::uint32_t>(w);
    dims.height = static_cast<std::uint32_t>(h);
    return true;
}

std::string Base64Encode(const std::vector<std::uint8_t>& data) {
    static const wchar_t alphabet[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((data.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const auto b0 = data[i];
        const auto b1 = (i + 1 < data.size()) ? data[i + 1] : 0;
        const auto b2 = (i + 2 < data.size()) ? data[i + 2] : 0;
        const auto triple = (static_cast<unsigned>(b0) << 16) | (static_cast<unsigned>(b1) << 8) | b2;
        result.push_back(static_cast<char>(alphabet[(triple >> 18) & 0x3F]));
        result.push_back(static_cast<char>(alphabet[(triple >> 12) & 0x3F]));
        result.push_back((i + 1 < data.size()) ? static_cast<char>(alphabet[(triple >> 6) & 0x3F]) : '=');
        result.push_back((i + 2 < data.size()) ? static_cast<char>(alphabet[triple & 0x3F]) : '=');
    }
    return result;
}

std::string MimeTypeForExtension(const std::wstring& ext) {
    auto lower = ext;
    for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
    if (lower == L".jpg" || lower == L".jpeg") return "image/jpeg";
    if (lower == L".png") return "image/png";
    if (lower == L".bmp") return "image/bmp";
    if (lower == L".gif") return "image/gif";
    if (lower == L".webp") return "image/webp";
    return "image/jpeg"; // fallback
}

void WriteBigEndianU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

bool BuildMetadataBlockPicture(const std::wstring& imagePath, std::string& base64Out) {
    // Read raw image bytes
    std::ifstream file(imagePath, std::ios::binary);
    if (!file) return false;
    std::vector<std::uint8_t> imageData((std::istreambuf_iterator<char>(file)), {});
    if (imageData.empty()) return false;

    // Get dimensions via WIC
    ImageDimensions dims{};
    ReadImageDimensions(imagePath, dims);

    // Determine MIME type
    auto ext = std::filesystem::path(imagePath).extension().wstring();
    std::string mime = MimeTypeForExtension(ext);

    // Build binary structure
    std::vector<std::uint8_t> binary;
    binary.reserve(4 + 4 + mime.size() + 4 + 0 + 4 + 4 + 4 + 4 + 4 + imageData.size());

    WriteBigEndianU32(binary, 3); // picture type: front cover
    WriteBigEndianU32(binary, static_cast<std::uint32_t>(mime.size()));
    binary.insert(binary.end(), mime.begin(), mime.end());
    WriteBigEndianU32(binary, 0); // description length (empty)
    WriteBigEndianU32(binary, dims.width);
    WriteBigEndianU32(binary, dims.height);
    WriteBigEndianU32(binary, 24); // color depth
    WriteBigEndianU32(binary, 0);  // colors used
    WriteBigEndianU32(binary, static_cast<std::uint32_t>(imageData.size()));
    binary.insert(binary.end(), imageData.begin(), imageData.end());

    // Base64-encode
    base64Out = Base64Encode(binary);
    return true;
}

// Tries to replace |src| with |dst| (rename), falling back to copy+delete if
// the rename fails (e.g. because the destination is open by the audio engine).
bool ReplaceFileWithFallback(const std::filesystem::path& src, const std::filesystem::path& dst) {
    // First try MoveFileExW with MOVEFILE_REPLACE_EXISTING (fast, atomic).
    if (MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return true;
    }

    // MoveFileExW failed — try copy + remove fallback.
    // This handles the case where the destination file is open for reading.
    std::error_code ec;
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return false;

    std::filesystem::remove(src, ec);
    return true;
}

// Wraps a value in double quotes, escaping any embedded double quotes.
std::wstring SimpleQuote(const std::wstring& value) {
    std::wstring result;
    result.reserve(value.size() + 3);
    result.push_back(L'"');
    for (const wchar_t ch : value) {
        if (ch == L'"') result += L"\\\"";
        else result.push_back(ch);
    }
    result.push_back(L'"');
    return result;
}

// Map a TrackMetadataField to the ffmpeg metadata key name.
const wchar_t* FfmpegKeyForField(TrackMetadataField field) noexcept {
    switch (field) {
    case TrackMetadataField::Author: return L"artist";
    case TrackMetadataField::Album:  return L"album";
    case TrackMetadataField::Genre:  return L"genre";
    case TrackMetadataField::Year:   return L"date";
    }
    return L"artist"; // fallback, should not reach
}

// Runs ffmpeg once with |args| appended to the quoted executable path.
// Waits up to 30s for completion and reports success only on exit code 0.
bool RunFfmpegWithArgs(const std::wstring& ffmpegPath, const std::wstring& args) {
    std::wstring commandLine = L"\"" + ffmpegPath + L"\" " + args;

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(ffmpegPath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return false;
    }

    WaitForSingleObject(process.hProcess, 30000); // 30s timeout

    DWORD exitCode = 0;
    const bool succeeded = GetExitCodeProcess(process.hProcess, &exitCode) && exitCode == 0;

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return succeeded;
}

} // anonymous namespace

// ===========================================================================
// Public API
// ============================================================================

const wchar_t* MetadataFieldLabel(TrackMetadataField field) noexcept {
    switch (field) {
    case TrackMetadataField::Author: return L"Change author";
    case TrackMetadataField::Album:  return L"Change album";
    case TrackMetadataField::Genre:  return L"Change genre";
    case TrackMetadataField::Year:   return L"Change year";
    }
    return L"Change value";
}

bool HandledByWindowsPropertyStore(const std::wstring& filePath) noexcept {
    auto ext = std::filesystem::path(filePath).extension().wstring();
    for (auto& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
    return ext == L".mp3" || ext == L".wma" || ext == L".wmv" ||
           ext == L".mp4" || ext == L".m4a" || ext == L".m4v" ||
           ext == L".wav" || ext == L".aac" || ext == L".asf";
}

bool PromptTrackMetadataValue(HWND owner, TrackMetadataField field,
                              std::wstring& value) {
    if (!RegisterDialogClass()) return false;

    PromptContext ctx;
    ctx.field = field;
    ctx.value = &value;

    // Compute the outer window size so the client area is exactly 350x140.
    constexpr int clientWidth = 350;
    constexpr int clientHeight = 140;
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_DLGMODALFRAME;
    RECT adjusted{0, 0, clientWidth, clientHeight};
    AdjustWindowRectEx(&adjusted, style, FALSE, exStyle);
    const int width = adjusted.right - adjusted.left;
    const int height = adjusted.bottom - adjusted.top;

    // Center the dialog over the owner (or the screen if there is no owner).
    RECT ownerRect{};
    if (owner != nullptr && IsWindow(owner)) {
        GetWindowRect(owner, &ownerRect);
    } else {
        ownerRect = {0, 0, GetSystemMetrics(SM_CXSCREEN),
                     GetSystemMetrics(SM_CYSCREEN)};
    }
    const int x = ownerRect.left + (ownerRect.right - ownerRect.left - width) / 2;
    const int y = ownerRect.top + (ownerRect.bottom - ownerRect.top - height) / 2;

    HWND dialog = CreateWindowExW(
        exStyle, kDialogClassName, L"Rivan",
        style,
        std::max(x, 0), std::max(y, 0),
        width, height,
        owner, nullptr, GetModuleHandleW(nullptr), &ctx);
    if (dialog == nullptr) return false;

    // Disable the owner for modal behaviour.
    if (owner != nullptr) {
        EnableWindow(owner, FALSE);
    }

    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    if (ctx.editControl != nullptr) {
        SetFocus(ctx.editControl);
    }

    // Modal message loop. IsDialogMessageW provides Enter->default button,
    // Esc->cancel button, and tab navigation between the controls.
    MSG msg{};
    while (IsWindow(dialog)) {
        const BOOL result = GetMessageW(&msg, nullptr, 0, 0);
        if (result == 0) {
            // WM_QUIT: forward it after tearing down the dialog.
            DestroyWindow(dialog);
            PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        if (result == -1) break; // error
        if (!IsDialogMessageW(dialog, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // Re-enable and activate the owner.
    if (owner != nullptr && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }

    return ctx.confirmed;
}

bool WriteTrackMetadataValue(const std::wstring& filePath,
                             TrackMetadataField field,
                             const std::wstring& value) {
    // Only try IPropertyStore for formats Windows handles natively.
    // For .opus, .ogg, .flac, .webm → go straight to ffmpeg.
    if (HandledByWindowsPropertyStore(filePath)) {
        Microsoft::WRL::ComPtr<IPropertyStore> properties;
        HRESULT hr = SHGetPropertyStoreFromParsingName(
            filePath.c_str(), nullptr, GPS_READWRITE,
            IID_PPV_ARGS(properties.GetAddressOf()));
        if (SUCCEEDED(hr)) {
            // PKEY_Media_Year expects a 32-bit integer; store a parseable year as such
            // so handlers that reject string assignments still accept it.
            if (field == TrackMetadataField::Year) {
                wchar_t* end = nullptr;
                const unsigned long parsed = std::wcstoul(value.c_str(), &end, 10);
                if (end != value.c_str() && value.c_str()[0] != L'\0' && parsed <= 9999) {
                    PROPVARIANT propValue{};
                    propValue.vt = VT_UI4;
                    propValue.ulVal = static_cast<ULONG>(parsed);
                    hr = properties->SetValue(FieldToPKey(field), propValue);
                    // No PropVariantClear needed for simple type.
                } else {
                    // Not a plain year – fall back to a string value.
                    PROPVARIANT propValue{};
                    if (SUCCEEDED(InitPropVariantFromString(value.c_str(), &propValue))) {
                        hr = properties->SetValue(FieldToPKey(field), propValue);
                        PropVariantClear(&propValue);
                    } else {
                        hr = E_FAIL;
                    }
                }
            } else {
                PROPVARIANT propValue{};
                if (SUCCEEDED(InitPropVariantFromString(value.c_str(), &propValue))) {
                    hr = properties->SetValue(FieldToPKey(field), propValue);
                    PropVariantClear(&propValue);
                } else {
                    hr = E_FAIL;
                }
            }

            if (SUCCEEDED(hr)) {
                hr = properties->Commit();
            }
            if (SUCCEEDED(hr)) return true;
        }
    }

    // IPropertyStore unavailable or failed – use ffmpeg.
    return WriteTrackMetadataValueFfmpeg(filePath, field, value);
}

// ===========================================================================
// ffmpeg fallback public API
// ===========================================================================

std::filesystem::path LocateFfmpeg() {
    const auto path = core::AppPaths::LocalDataRoot() / L"tools" / L"ffmpeg.exe";
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec) ? path : std::filesystem::path{};
}

bool WriteTrackMetadataValueFfmpeg(const std::wstring& filePath,
                                   TrackMetadataField field,
                                   const std::wstring& value) {
    const auto ffmpeg = LocateFfmpeg();
    if (ffmpeg.empty()) return false;

    // Build temp path in the same directory.
    // Preserve original extension so ffmpeg infers the muxer: file.opus -> file.RivanMeta.opus
    std::filesystem::path input(filePath);
    auto temp = input.parent_path() /
        (input.stem().wstring() + L".RivanMeta" + input.extension().wstring());

    const wchar_t* key = FfmpegKeyForField(field);
    std::wstring args = L"-i " + SimpleQuote(filePath) +
                        L" -metadata " + key + L"=" + SimpleQuote(value) +
                        L" -codec copy -y " + SimpleQuote(temp.wstring());

    if (!RunFfmpegWithArgs(ffmpeg.wstring(), args)) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        return false;
    }

    // Replace original with temp.
    if (!ReplaceFileWithFallback(temp, input)) {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

bool WriteCoverArtFfmpeg(const std::wstring& filePath, const std::wstring& imagePath) {
    const auto ffmpeg = LocateFfmpeg();
    if (ffmpeg.empty()) return false;

    // Build the METADATA_BLOCK_PICTURE base64 blob
    std::string b64;
    if (!BuildMetadataBlockPicture(imagePath, b64)) return false;

    std::filesystem::path input(filePath);
    // Preserve original extension so ffmpeg infers the muxer: file.opus -> file.RivanCover.opus
    auto temp = input.parent_path() /
        (input.stem().wstring() + L".RivanCover" + input.extension().wstring());
    // Temp metadata file for the base64 blob (avoids 32K command-line limit)
    auto metaFile = input.parent_path() /
        (input.stem().wstring() + L".RivanMeta.txt");

    // Write ffmetadata file: ";FFMETADATA1\nmetadata_block_picture=<b64>\n"
    // The file is UTF-8, with a BOM or without (ffmpeg accepts both).
    {
        std::ofstream meta(metaFile, std::ios::binary);
        if (!meta) return false;
        // ffmpeg's ffmetadata parser reads UTF-8 without BOM fine.
        meta << ";FFMETADATA1\n";
        meta << "metadata_block_picture=" << b64 << "\n";
        meta.close();
        if (!meta) {
            std::error_code ec;
            std::filesystem::remove(metaFile, ec);
            return false;
        }
    }

    // ffmpeg command: -i input -f ffmetadata -i meta.txt -map_metadata 1 -c copy -y output
    // -map_metadata 1 copies metadata from the second input (the metadata file) to the output
    // -c copy copies all streams without re-encoding
    std::wstring args = L"-i " + SimpleQuote(filePath) +
                        L" -f ffmetadata -i " + SimpleQuote(metaFile.wstring()) +
                        L" -map_metadata 1 -c copy -y " + SimpleQuote(temp.wstring());

    bool success = false;
    if (RunFfmpegWithArgs(ffmpeg.wstring(), args)) {
        // Replace original with temp
        if (ReplaceFileWithFallback(temp, input)) {
            success = true;
        } else {
            std::error_code ec;
            std::filesystem::remove(temp, ec);
        }
    } else {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
    }

    // Always clean up the metadata file
    std::error_code ec;
    std::filesystem::remove(metaFile, ec);
    return success;
}

} // namespace rivan::ui