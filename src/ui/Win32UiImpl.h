// Win32UiImpl.h
#pragma once
// Native Win32/Direct2D presentation styled after late-90s desktop audio players.
#include "Win32Ui.h"
#include "layout/ModuleLayout.h"
#include "layout/ModulePixelGeometry.h"
#include "Win32UiModuleState.h"
#include "Win32UiSkinStudioState.h"
#include "Win32UiSongRowLayoutState.h"
#include "Win32UiRenderState.h"
#include "Win32UiPreviewState.h"
#include "Win32UiPlaylistState.h"

#include "../resource.h"
#include "../visualization/VisualizationRenderer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dwrite_3.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmsystem.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "winmm.lib")

namespace rivan::ui {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClassName[] = L"Rivan.Direct2D.UiHost.v2";
constexpr UINT_PTR kRefreshTimer = 1;
// Debounced live search while typing in the Youtube browser (ms).
constexpr UINT_PTR kYoutubeSearchDebounceTimer = 2;
constexpr UINT kYoutubeSearchDebounceMs = 200;
constexpr UINT_PTR kYoutubeGrabberCopyTimer = 3;
constexpr UINT_PTR kYoutubeGrabberAddressCopyTimer = 4;
constexpr UINT_PTR kYoutubeGrabberClipboardTimer = 5;
constexpr UINT kYoutubeGrabberCopyDelayMs = 75;
constexpr UINT kYoutubeGrabberAddressCopyDelayMs = 125;
constexpr UINT kYoutubeGrabberClipboardDelayMs = 100;
// Notification-area (system tray) icon for the exit-to-tray feature. The callback
// message is delivered to the Main window; the menu command IDs drive restore/exit.
constexpr UINT kTrayCallbackMessage = WM_APP + 41;
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayMenuOpen = 1;
constexpr UINT kTrayMenuExit = 2;
constexpr int kYoutubeGrabberHotkeyId = 3;
// Preview only presents already-decoded frames. 60 Hz avoids paint pressure without
// making the audio-clock-driven preview visibly sluggish.
// A full scene repaint is enough at the analyzer's ~30 Hz cadence. Faster timer
// ticks only add redundant UI work while playback is active.
constexpr UINT kRefreshPlayingMilliseconds = 33;
constexpr UINT kRefreshIdleMilliseconds = 200;
constexpr UINT kRefreshMilliseconds = kRefreshPlayingMilliseconds;
constexpr double kPreviewSeekThresholdSeconds = 0.50;
constexpr double kPreviewFrameLeadSeconds = 0.033;
constexpr std::size_t kMaximumTrackCoverCacheEntries = 96;
// Borderless window: keep OVERLAPPEDWINDOW so Aero snap, resize, and min/max still work,
// but the caption is removed visually via WM_NCCALCSIZE. This is the pixel thickness of
// the invisible resize border reported by WM_NCHITTEST.
constexpr int kResizeBorder = 6;
constexpr float kTitlebarHeight = 28.0F;
constexpr float kTitlebarButtonSize = 22.0F;
[[nodiscard]] HICON LoadRivanIcon(HINSTANCE instance, int width, int height) noexcept {
    HICON icon = reinterpret_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_RIVAN), IMAGE_ICON, width, height, LR_DEFAULTCOLOR | LR_SHARED));
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

void PositionToolWindow(HWND tool, HWND owner, int verticalOffset) {
    RECT ownerRect{};
    RECT toolRect{};
    if (!GetWindowRect(owner, &ownerRect) || !GetWindowRect(tool, &toolRect)) return;
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const int width = toolRect.right - toolRect.left;
    const int height = toolRect.bottom - toolRect.top;
    int x = ownerRect.right + 12;
    if (x + width > monitor.rcWork.right) x = ownerRect.left - width - 12;
    if (x < monitor.rcWork.left) x = monitor.rcWork.right - width;
    const int y = std::clamp(ownerRect.top + verticalOffset, monitor.rcWork.top,
                             std::max(monitor.rcWork.top, monitor.rcWork.bottom - height));
    SetWindowPos(tool, nullptr, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

[[nodiscard]] D2D1_COLOR_F ToD2D(skin::Color color) noexcept {
    return D2D1::ColorF(static_cast<float>(color.red) / 255.0F,
                        static_cast<float>(color.green) / 255.0F,
                        static_cast<float>(color.blue) / 255.0F,
                        static_cast<float>(color.alpha) / 255.0F);
}

// Slightly darkens a color for bevel-dark / recessed screen fills.
[[nodiscard]] skin::Color Darken(skin::Color color, float factor) noexcept {
    const auto scale = [factor](std::uint8_t channel) {
        return static_cast<std::uint8_t>(std::clamp(static_cast<float>(channel) * factor, 0.0F, 255.0F));
    };
    return {scale(color.red), scale(color.green), scale(color.blue), color.alpha};
}

[[nodiscard]] skin::Color HsvColor(float hue, float saturation, float value,
                                   std::uint8_t alpha = 255) noexcept {
    hue = hue - std::floor(hue);
    saturation = std::clamp(saturation, 0.0F, 1.0F);
    value = std::clamp(value, 0.0F, 1.0F);
    const float scaled = hue * 6.0F;
    const int sector = static_cast<int>(scaled) % 6;
    const float fraction = scaled - std::floor(scaled);
    const float p = value * (1.0F - saturation);
    const float q = value * (1.0F - fraction * saturation);
    const float t = value * (1.0F - (1.0F - fraction) * saturation);
    float red{}, green{}, blue{};
    switch (sector) {
    case 0: red = value; green = t; blue = p; break;
    case 1: red = q; green = value; blue = p; break;
    case 2: red = p; green = value; blue = t; break;
    case 3: red = p; green = q; blue = value; break;
    case 4: red = t; green = p; blue = value; break;
    default: red = value; green = p; blue = q; break;
    }
    const auto channel = [](float component) {
        return static_cast<std::uint8_t>(std::lround(component * 255.0F));
    };
    return {channel(red), channel(green), channel(blue), alpha};
}

void ColorToHsv(skin::Color color, float& hue, float& saturation, float& value) noexcept {
    const float red = static_cast<float>(color.red) / 255.0F;
    const float green = static_cast<float>(color.green) / 255.0F;
    const float blue = static_cast<float>(color.blue) / 255.0F;
    const float maximum = std::max({red, green, blue});
    const float minimum = std::min({red, green, blue});
    const float delta = maximum - minimum;
    value = maximum;
    saturation = maximum > 0.0F ? delta / maximum : 0.0F;
    if (delta == 0.0F) hue = 0.0F;
    else if (maximum == red) hue = std::fmod((green - blue) / delta, 6.0F) / 6.0F;
    else if (maximum == green) hue = ((blue - red) / delta + 2.0F) / 6.0F;
    else hue = ((red - green) / delta + 4.0F) / 6.0F;
    if (hue < 0.0F) hue += 1.0F;
}

[[nodiscard]] D2D1_RECT_F Rect(float left, float top, float right, float bottom) noexcept {
    return D2D1::RectF(left, top, right, bottom);
}

[[nodiscard]] bool Contains(const D2D1_RECT_F& rectangle, float x, float y) noexcept {
    return x >= rectangle.left && x <= rectangle.right &&
           y >= rectangle.top && y <= rectangle.bottom;
}

[[nodiscard]] float Width(const D2D1_RECT_F& rectangle) noexcept {
    return std::max(0.0F, rectangle.right - rectangle.left);
}

[[nodiscard]] float Height(const D2D1_RECT_F& rectangle) noexcept {
    return std::max(0.0F, rectangle.bottom - rectangle.top);
}

[[nodiscard]] std::wstring FormatTime(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    const auto total = static_cast<unsigned long long>(seconds);
    const auto hours = total / 3600ULL;
    const auto minutes = (total / 60ULL) % 60ULL;
    const auto remaining = total % 60ULL;
    wchar_t buffer[32]{};
    if (hours != 0) {
        std::swprintf(buffer, std::size(buffer), L"%llu:%02llu:%02llu", hours, minutes, remaining);
    } else {
        std::swprintf(buffer, std::size(buffer), L"%llu:%02llu", minutes, remaining);
    }
    return buffer;
}

[[nodiscard]] const wchar_t* CategoryName(SettingCategory category) noexcept {
    switch (category) {
    case SettingCategory::General: return L"GENERAL";
    case SettingCategory::Appearance: return L"APPEARANCE";
    case SettingCategory::Discord: return L"DISCORD";
    case SettingCategory::Online: return L"ONLINE";
    case SettingCategory::SkinManager: return L"SKIN MANAGER";
    }
    return L"SETTINGS";
}

[[nodiscard]] const wchar_t* RepeatLabel(RepeatMode mode) noexcept {
    switch (mode) {
    case RepeatMode::Off: return L"REPEAT";
    case RepeatMode::All: return L"REPEAT ALL";
    case RepeatMode::One: return L"REPEAT ONE";
    }
    return L"REPEAT";
}

[[nodiscard]] std::wstring Lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

[[nodiscard]] bool Matches(const TrackView& track, const std::wstring& query) {
    if (query.empty()) return true;
    const std::wstring needle = Lowercase(query);
    return Lowercase(track.title).find(needle) != std::wstring::npos ||
           Lowercase(track.artist).find(needle) != std::wstring::npos ||
           Lowercase(track.album).find(needle) != std::wstring::npos;
}

[[nodiscard]] std::string Utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                              static_cast<int>(text.size()), nullptr, 0,
                                              nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                        result.data(), required, nullptr, nullptr);
    return result;
}

} // namespace

struct Win32Ui::Impl : Win32UiModuleState, Win32UiSkinStudioState,
                        Win32UiRenderState, Win32UiPreviewState, Win32UiPlaylistState,
                        Win32UiSongRowLayoutState {
    enum class HitKind : std::uint8_t {
        Command, Playlist, PlaylistToggle, Track, Seek, Volume, Setting,
        PlaylistSearch, WindowControl, Studio, Refresh, SettingsAction, TimeToggle,
        YoutubeResult, YoutubeChooserAction, FilePreviewFullscreen, FilePreviewExitFullscreen,
        ModuleTitle,
        ModuleTab,
        ModuleCollapseToggle,
        LyricsAction,
        LyricsVerse,
        SongRowField,
        // Playlist Editor bottom-row buttons and the tree "new playlist" (+) button.
        EditorAdd, EditorRemove, NewPlaylist
    };
    struct HitRegion {
        D2D1_RECT_F bounds{};
        HitKind kind{HitKind::Command};
        Command command{Command::PlayPause};
        std::uint64_t id{};
        SettingCategory category{SettingCategory::General};
        // Visible position of a track/playlist row (disambiguates duplicate ids and
        // drives multi-select ranges and drag reordering). Meaningful for Track/Playlist.
        std::size_t index{};
    };

    explicit Impl(IUiHost& callbackHost, WindowKind windowKind)
        : host(callbackHost), windowKind(windowKind) {}

    IUiHost& host;
    WindowKind windowKind{WindowKind::Main};
    WindowOptions options{};
    HWND window{};
    HINSTANCE instance{};
    // Most recently observed window rectangle. Persisted on shutdown after the window
    // handle is gone, so the App keeps the last size/position instead of the stale value.
    RECT lastWindowRect{};
    bool hasWindowRect{};
    UiModel model;
    std::vector<HitRegion> hits;
    struct ColorFocusRegion { D2D1_RECT_F bounds{}; std::size_t index{}; };
    std::vector<ColorFocusRegion> colorFocusRegions;
    // Draggable application titlebar region reported as HTCAPTION so the borderless
    // window can be moved independently of the module layout.
    bool draggingSeek{};
    bool draggingVolume{};
    // Player LCD time readout toggles between elapsed and remaining on click.
    bool showRemaining{};
    POINT mouse{-1, -1};

    // Skin Manager rename state remains with the settings coordinator.
    bool managerNameEditing{};
    std::size_t managerSkinIndex{};
    std::wstring managerSkinName;
    // Preferences detail pane: pixel scroll for General content that exceeds the window.
    float settingsScrollY{};
    float settingsContentHeight{};
    D2D1_RECT_F settingsDetailsBounds{};
    D2D1_RECT_F lyricsContentBounds{};
    float lyricsScrollY{};
    bool lyricsSyncedMode_{true};
    std::uint64_t lyricsRevision_{};
    DWRITE_TEXT_ALIGNMENT lyricsAlignment_{DWRITE_TEXT_ALIGNMENT_LEADING};
    // Skin Manager saved-skins list row scroll.
    std::size_t settingsSkinScroll{};
    std::size_t settingsSkinRows{};
    D2D1_RECT_F settingsSkinListBounds{};
    D2D1_SIZE_F lastCanvas{};
    std::unique_ptr<Win32Ui> settingsWindow;
    std::unique_ptr<Win32Ui> studioWindow;
    std::unique_ptr<Win32Ui> youtubeWindow;
    // True while the notification-area icon is live (exit-to-tray hid the window).
    bool trayIconAdded{};
    bool youtubeGrabberHotkeyRegistered{};
    bool youtubeGrabberHotkeyCapture{};
    bool youtubeGrabberHotkeyCaptureFailed{};
    std::uint32_t youtubeGrabberHotkeyModifiers{};
    std::uint32_t youtubeGrabberHotkeyVirtualKey{};
    std::uint32_t youtubeGrabberClipboardSequence{};
    unsigned youtubeGrabberClipboardAttempts{};
    HWND youtubeGrabberTargetWindow{};
    ComPtr<IDataObject> youtubeGrabberClipboardBackup;

    void ClearFilePreview() noexcept;

    void StopPreviewWorker() noexcept;

    [[nodiscard]] static bool IsVideoPath(const std::wstring& path);

    [[nodiscard]] bool CreatePreviewBitmapFromBgra(UINT width, UINT height, const BYTE* data,
                                                    UINT stride);

    [[nodiscard]] bool CreatePreviewBitmapFromHBitmap(HBITMAP bitmap);

    [[nodiscard]] bool CreatePreviewBitmapFromEncoded(const BYTE* data, std::size_t size);

    // Pulls embedded album art from ID3v2 APIC frames when Shell thumbnails fail.
    [[nodiscard]] bool LoadEmbeddedId3Cover(const std::wstring& path);

    [[nodiscard]] bool LoadCoverArt(const std::wstring& path);

    [[nodiscard]] bool OpenVideoPreview(const std::wstring& path);

    [[nodiscard]] ComPtr<ID2D1Bitmap> CreateTrackCoverBitmapFromHBitmap(HBITMAP bitmap,
                                                                           UINT maximumDimension);

    [[nodiscard]] ComPtr<ID2D1Bitmap> CreateTrackCoverBitmapFromEncoded(const BYTE* data,
                                                                           std::size_t size,
                                                                           UINT maximumDimension);

    [[nodiscard]] ComPtr<ID2D1Bitmap> LoadEmbeddedId3TrackCover(const std::wstring& path,
                                                                   UINT maximumDimension);

    void TrimTrackCoverCache();

    [[nodiscard]] ID2D1Bitmap* TrackCoverBitmap(const std::wstring& path,
                                                 UINT maximumDimension);

    void DrawTrackCover(const TrackView& track, const D2D1_RECT_F& bounds);

    void UpdateVideoPreviewFrame();

    void DrawPreviewBitmap(const D2D1_RECT_F& bounds);

    void DrawPreviewFullscreenOverlay(const D2D1_SIZE_F size,
                                      std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);

    void DrawVideoPreview(const D2D1_RECT_F& bounds,
                          std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);

    [[nodiscard]] bool IsVideoPreviewModuleVisible() const noexcept;

    void LoadFilePreview(const std::wstring& path);

    [[nodiscard]] std::wstring ActivePreviewPath() const;

    void ExitPreviewFullscreen() noexcept;

    void EnterPreviewFullscreen() noexcept;

    void SyncFilePreview(bool advanceVideo = true);
    void SelectStudioSection(StudioSection section);

    [[nodiscard]] skin::Color* ActiveStudioColor();

    void OpenElementColorPicker(StudioColorTarget colorTarget);

    void DrawStudioColorPicker(float left, float right, float& y, const std::function<D2D1_RECT_F()>& row,
                               std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b,
                               std::uint64_t eyedropperAction, std::uint64_t hexAction);

    [[nodiscard]] bool CreateDeviceIndependentResources();

    [[nodiscard]] ComPtr<IDWriteFontCollection1> CustomFontCollection(
        const std::filesystem::path& file);

    [[nodiscard]] ComPtr<IDWriteTextFormat> BuildTextFormat(
        const std::wstring& family, float size, DWRITE_FONT_WEIGHT weight,
        const std::filesystem::path& customFile = {},
        DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL);

    [[nodiscard]] std::optional<std::wstring> FontFamilyFromFile(
        const std::filesystem::path& file);

    // ApplySkinFonts guard keys: the raw typography inputs that feed its signature check,
    // compared before any string building so unchanged skin fonts skip the per-paint
    // wstring work. fontSignature (in Win32UiRenderState) remains the reset authority.
    std::string lastAppliedFontFamily;
    std::filesystem::path lastAppliedCustomFontFile;
    std::filesystem::path lastAppliedSkinDirectory;
    int lastAppliedFontBaseSizeKey{std::numeric_limits<int>::min()};

    // Rebuilds UI text formats from active skin typography. Custom files use a private
    // DirectWrite collection because FR_PRIVATE fonts are absent from its system collection.
    void ApplySkinFonts();

    [[nodiscard]] bool CreateTarget();

    void DiscardTarget() noexcept;

    void SyncRefreshTimer() noexcept;

    void AddHit(const D2D1_RECT_F& bounds, Command command);

    void AddIdHit(const D2D1_RECT_F& bounds, HitKind kind, std::uint64_t id);

    void AddSimpleHit(const D2D1_RECT_F& bounds, HitKind kind);

    void AddSettingHit(const D2D1_RECT_F& bounds, SettingCategory category);

    [[nodiscard]] const HitRegion* HitTest(float x, float y) const noexcept;

    [[nodiscard]] const HitRegion* HitTestContent(float x, float y) const noexcept;

    void DrawText(std::wstring_view textValue, const D2D1_RECT_F& bounds,
                   ID2D1Brush* brush, IDWriteTextFormat* format,
                   DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING,
                   DWRITE_PARAGRAPH_ALIGNMENT vertical = DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
                   D2D1_DRAW_TEXT_OPTIONS drawOptions = D2D1_DRAW_TEXT_OPTIONS_CLIP,
                   const D2D1_RECT_F* clipBounds = nullptr,
                   bool trim = false);

    // Horizontally scrolling single-line text, like an HTML <marquee> moving to the
    // right. The glyphs travel from the left edge toward the right and wrap around.
    void DrawMarquee(const D2D1_RECT_F& bounds, const std::wstring& textValue, ID2D1Brush* brush);

    void FlushDeferredTexts();

    void DrawBevel(const D2D1_RECT_F& bounds, ID2D1Brush* fill, ID2D1Brush* light,
                   ID2D1Brush* dark, bool inset = false, float thickness = 1.0F);

    // Panels honor appearance toggles:
    //  * panelOpacity < 1 lets skin decor (images/shapes) show through the panel fill.
    //  * showTitleBars=false drops the raised metallic bar behind titles; the title text
    //    then sits directly on the panel background.
    //  * showPanelBorders=false removes the magnetic raised frame around each panel.
    [[nodiscard]] D2D1_RECT_F DrawPanel(const D2D1_RECT_F& bounds, std::wstring_view titleValue,
                                         ID2D1Brush* metal, ID2D1Brush* raised, ID2D1Brush* light,
                                         ID2D1Brush* dark, ID2D1Brush* green, ID2D1Brush* /*stripe*/,
                                         std::optional<ModuleId> module = std::nullopt);

    // Transparent buttons drop the beveled metal background and use the larger regular
    // font so they read as plain text; the label brightens on hover / active. Classic
    // beveled buttons remain available when the skin disables transparent buttons.
    void DrawButton(const D2D1_RECT_F& bounds, const wchar_t* label, Command command,
                     ID2D1Brush* fill, ID2D1Brush* hotFill, ID2D1Brush* light,
                     ID2D1Brush* dark, ID2D1Brush* textBrush, bool active = false);

    void DrawStaticButton(const D2D1_RECT_F& bounds, const wchar_t* label,
                          ID2D1Brush* fill, ID2D1Brush* light, ID2D1Brush* dark,
                          ID2D1Brush* textBrush);

    void DrawWindowButton(const D2D1_RECT_F& bounds, const wchar_t* label, std::uint64_t action,
                          ID2D1Brush* fill, ID2D1Brush* light, ID2D1Brush* dark,
                          ID2D1Brush* textBrush);

    void DrawSlider(const D2D1_RECT_F& bounds, float value, HitKind kind,
                    ID2D1Brush* screen, ID2D1Brush* green, ID2D1Brush* silver,
                    ID2D1Brush* light, ID2D1Brush* dark);

    [[nodiscard]] bool IsYoutubeBrowsingNow();

    void ArmYoutubeSearchDebounce();

    void FlushYoutubeSearchDebounce();

    void DrawSearch(const D2D1_RECT_F& bounds, const std::wstring& query, SearchTarget search,
                    ID2D1Brush* screen, ID2D1Brush* light, ID2D1Brush* dark,
                    ID2D1Brush* green, ID2D1Brush* dim);

    [[nodiscard]] const std::vector<const TrackView*>& Filtered(const std::vector<TrackView>& source,
                                                                  const std::wstring& query);

    // Position of a TrackView (borrowed from model.tracks) within that vector. Selection
    // and drag reorder key off this stable model index, not the filtered row index, so
    // duplicate entries and search filtering stay unambiguous.
    [[nodiscard]] std::size_t ModelTrackIndex(const TrackView* track) const noexcept;
    // Position of a visible row within its owning playlist. Parent-folder views may
    // contain entries from several source playlists.
    [[nodiscard]] std::size_t SourceTrackIndex(const TrackView* track) const noexcept;

    // Thin horizontal insertion bar drawn between rows while a track drag is active.
    void DrawTrackDropIndicator(const D2D1_RECT_F& row, bool below, ID2D1Brush* brush);

    void DrawTrackRows(const D2D1_RECT_F& bounds, const std::vector<const TrackView*>& tracks,
                       std::size_t& scroll, std::size_t& visibleRows,
                       ID2D1Brush* screen, ID2D1Brush* green, ID2D1Brush* greenDim,
                        ID2D1Brush* selection, ID2D1Brush* white, ID2D1Brush* dim,
                        bool showArtist = true);
    [[nodiscard]] float TrackRowHeight() const noexcept;
    [[nodiscard]] IDWriteTextFormat* SongRowFormat(int sizeDelta,
                                                    SongRowFontWeight weight,
                                                    SongRowFontStyle style = SongRowFontStyle::Normal);
    void DrawSongRow(const D2D1_RECT_F& row, const TrackView& track,
                     std::size_t number, std::size_t modelIndex, bool selected,
                     ID2D1Brush* primary, ID2D1Brush* secondary,
                     ID2D1Brush* active, const SongRowLayout* layout = nullptr,
                     bool sample = false,
                     std::array<D2D1_RECT_F, kSongRowFieldCount>* fieldBounds = nullptr);
    void DrawTrackRenameField(const D2D1_RECT_F& bounds, ID2D1Brush* textBrush);

    // Renders the current folder view: the selected folder's loose tracks first (no
    // header) followed by one separator header per subfolder section. Falls back to a
    // plain flat list when the model provides no sections. Search filters tracks by
    // title/artist/album and hides sections left empty.
    void DrawSectionedTracks(const D2D1_RECT_F& bounds, std::size_t& scroll,
                             std::size_t& visibleRows,
                             ID2D1Brush* screen, ID2D1Brush* green, ID2D1Brush* greenDim,
                             ID2D1Brush* selection, ID2D1Brush* white, ID2D1Brush* dim);

    [[nodiscard]] std::wstring SelectedPlaylistName() const;

    void DrawPlayer(const D2D1_RECT_F& bounds,
                    std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);

    void DrawPlaylistEditor(const D2D1_RECT_F& bounds,
                            std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);

    void DrawEqualizer(const D2D1_RECT_F& bounds,
                       std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);

    void DrawLibrary(const D2D1_RECT_F& bounds,
                     std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);

    void DrawLyrics(const D2D1_RECT_F& bounds,
                    std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);

    void HandleLyricsAction(std::uint64_t action);

    void HandleLyricsVerse(std::size_t index);

    void DrawMini(const D2D1_SIZE_F size,
                   std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);

    void DrawTitlebar(const D2D1_SIZE_F size,
                     std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);

    [[nodiscard]] bool HasTitlebar() const noexcept;

    // Decodes a skin image file into a device bitmap, caching by absolute path.
    [[nodiscard]] ID2D1Bitmap* LoadSkinBitmap(const std::filesystem::path& relative);

    [[nodiscard]] static std::vector<DecorRef> DecorOrder(const skin::Skin& value);

    [[nodiscard]] const std::vector<DecorRef>& CachedDecorOrder();

    // Layer 0 draws on window background. Layers 1 and 2 replay enabled decor over
    // panels and screens while control holes keep sliders usable and visible.
    void DrawSkinDecor(const D2D1_SIZE_F size, int layer = 0);

    void DrawImageSelection(const D2D1_SIZE_F size);

    void DrawFull(const D2D1_SIZE_F size,
                  std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);

    void DrawSettings(const D2D1_SIZE_F size,
                      std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);

    void DrawYoutubeChooser(const D2D1_SIZE_F size,
                            std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);

    // General pane: music folder list. Show every configured root, then one empty
    // slot so the next folder can be chosen. After each choice another empty slot
    // appears (no limit). Subfolders of all roots become playlists.
    void DrawGeneralPane(const D2D1_RECT_F& details,
                         std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);

    // Skin Manager pane: two side-by-side actions on top (open the skin studio / open the
    // skins folder), and a scrollable-ish list of saved and built-in skins below. Clicking
    // a skin applies it immediately.
    void DrawSkinManagerPane(const D2D1_RECT_F& details,
                             std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);

    // ---- Skin Studio (implementations in Win32Ui.SkinStudio.cpp) ------------

    [[nodiscard]] static const std::array<ColorField, 13>& StudioColorFields();
    void EnsureStudioDraft();
    void PushPreview();
    void QueuePreview();
    void StudioButton(const D2D1_RECT_F& bounds, const std::wstring& label, std::uint64_t action,
                      std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b, bool active = false,
                      IDWriteTextFormat* format = nullptr);
    void SettingsButton(const D2D1_RECT_F& bounds, const std::wstring& label, std::uint64_t action,
                        std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);
    void HandleSettingsAction(std::uint64_t action);
    void HandleYoutubeChooserAction(std::uint64_t action);
    [[nodiscard]] std::optional<std::filesystem::path> PickFolder();
    void StudioRailButton(const D2D1_RECT_F& bounds, const wchar_t* icon, const wchar_t* label,
                          StudioSection section, std::uint64_t action,
                          std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);
    void DrawSongRowLayoutEditor(float left, float right, float& y,
                                 std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);
    void BeginSongRowFieldDrag(SongRowField field, float x, float y);
    void UpdateSongRowFieldDrag(float x, float y);
    [[nodiscard]] bool ApplySongRowSnap() noexcept;
    void AdjustSongRowSnapGap(int delta);
    void CommitSongRowLayoutEdit();
    void DrawSkinStudio(const D2D1_SIZE_F size,
                        std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);
    template <typename LabelFn, typename RowFn>
    void DrawStudioColors(float left, float right, float& y, LabelFn label, RowFn row,
                          std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);
    template <typename LabelFn, typename RowFn>
    void DrawStudioFont(float left, float right, float& y, LabelFn label, RowFn row,
                        std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);
    template <typename LabelFn, typename RowFn>
    void DrawStudioToggles(float left, float right, float& y, LabelFn label, RowFn row,
                           std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);
    template <typename LabelFn, typename RowFn>
    void DrawStudioElements(float left, float right, float& y, LabelFn label, RowFn row,
                            std::array<ComPtr<ID2D1SolidColorBrush>, 14>& b);
    [[nodiscard]] std::filesystem::path PickFile(const COMDLG_FILTERSPEC* filters, UINT filterCount);
    void StepColorChannel(int channelPair);
    [[nodiscard]] static std::wstring ToHexW(skin::Color color);
    void SampleScreenColorAtCursor();
    void BeginScreenEyedropper();
    void CancelScreenEyedropper();
    void ApplyStudioHex();
    void CopyStudioHex();
    void PasteStudioHex();
    void PastePlaylistQuery();
    void CopyTrackName();
    void PasteTrackName();
    void ImportStudioImage();
    void ImportStudioFont();
    void HandleStudioAction(std::uint64_t action);


    // Brushes are derived from the active skin palette so appearance is never hard-coded.
    // Index legend (kept for the existing draw call sites):
    //  0 windowBg 1 panelBg 2 controlBg 3 bevelLight 4 bevelDark 5 screen 6 accent
    //  7 hoverBg 8 accent 9 textPrimary 10 textSecondary 11 selection 12 textPrimary
    //  13 controls (seek/vol fill, titlebars, window chrome text, transport labels, EQ ON/AUTO)
    // Reuses solidBrushes; returns references for Draw* call sites that expect ComPtr array.
    [[nodiscard]] std::array<ComPtr<ID2D1SolidColorBrush>, 14>& UpdateBrushes();

    // Keeps a click-to-play mono-selection in sync with the transport. A plain track click
    // both selects and plays the row, so the played track lands in trackSelection. When the
    // transport auto-advances, the new track's `playing` flag already highlights it; without
    // this the previous row would keep its selection fill and look like it is still active.
    // Only the lone auto-selection of the previously playing row is moved; genuine multi- or
    // ctrl-selections are left untouched.
    void SyncSelectionToPlayback();

    void Paint();

    void Resize(UINT width, UINT height, bool minimized = false);

    // Snapshots the window rectangle for later shutdown persistence.
    void CaptureWindowRect() noexcept;

    // ---- Module interaction (implementation in Win32Ui.ModuleInteraction.cpp) ----
    void BeginModuleDrag(ModuleId id, float x, float y,
                         const ModuleLayout* layoutOverride = nullptr,
                         bool detachTabOnMove = false);
    void DetachModuleFromTabs(ModuleLayout& layout, ModuleId id) const noexcept;
    void UpdateModuleDrag(float x, float y);
    void FinishModuleDrag() noexcept;
    void ResetModuleDropPreview() noexcept;
    void ResolveModuleDropPreview(float x, float y);
    [[nodiscard]] bool BeginModuleResize(float x, float y);
    [[nodiscard]] HCURSOR ModuleCursor(float x, float y) const noexcept;

    void InvokeSafely(Command command);

    // ---- Notification-area (system tray) support ----------------------------

    [[nodiscard]] NOTIFYICONDATAW TrayIconData() const noexcept;

    void AddTrayIcon();

    void RemoveTrayIcon();

    // Restores the hidden main window and drops the tray icon.
    void RestoreFromTray();

    // Right-click tray menu: Open restores the window, Exit closes for real.
    void ShowTrayMenu();

    [[nodiscard]] bool RegisterYoutubeGrabberHotkey(std::uint32_t modifiers,
                                                    std::uint32_t virtualKey) noexcept;

    void UnregisterYoutubeGrabberHotkey() noexcept;

    // ---- Input (implementations in Win32Ui.Input.cpp) -----------------------

    void SetSlider(const HitRegion& hit, float x);
    // Keeps the focused element draggable through overlapping decor, then falls back to
    // the topmost element under the pointer. Returns false if nothing was hit.
    [[nodiscard]] bool BeginDecorDrag(float x, float y);
    void MoveDecor(float x, float y);
    void UpdateStudioColor(float x, float y, bool hueOnly);
    void UpdateLayerDrag(float y);
    void PointerDown(float x, float y);
    void PointerMove(float x, float y);
    void PointerUp(std::optional<D2D1_POINT_2F> release = std::nullopt) noexcept;
    // Provides resize borders and a draggable caption for the borderless window.
    [[nodiscard]] LRESULT HitTestNonClient(int screenX, int screenY) const;
    [[nodiscard]] static bool IsImageDrop(const std::filesystem::path& path);
    void DropFiles(HDROP drop);
    void AddDroppedImages(const std::vector<std::wstring>& paths, POINT dropPoint);
    void ActivateFirstSearchResult();
    void Character(wchar_t character);
    bool KeyDown(WPARAM key);
    void Scroll(float x, float y, int direction);
    // Right-click context menu (tracks / playlists) + selection helpers.
    void PointerRightDown(float x, float y);
    void ApplyTrackClickSelection(std::size_t modelIndex, bool ctrl, bool shift);
    void ApplyPlaylistClickSelection(std::uint64_t id, std::size_t treeIndex, bool ctrl, bool shift);
    void ResetTrackSelectionForPlaylist(std::uint64_t playlistId);
    [[nodiscard]] std::vector<std::size_t> SelectedTrackIndicesSorted() const;
    void ShowTrackContextMenu(std::size_t modelIndex);
    void ShowPlaylistContextMenu(std::uint64_t playlistId);
    void SyncMouseFromCursor();
    void CommitPlaylistName();
    void CancelPlaylistName();
    void CommitTrackName();
    void CancelTrackName();
    void BeginTrackRename(std::size_t modelIndex);
    [[nodiscard]] std::size_t ResolveTrackDrop(float y) const;
    void ResolvePlaylistDrop(float y);
    void FinishTrackDrag();
    void FinishPlaylistDrag();
    // Press / drag lifecycle for track rows and tree playlist rows.
    void BeginTrackPress(const HitRegion& hit, float x, float y);
    void BeginPlaylistPress(const HitRegion& hit, float x, float y);
    void UpdateRowDrag(float x, float y);
    void FinishRowDrag() noexcept;
    void RemoveSelectedTracks();
    void BeginCreatePlaylist();
    // True when the tree row at model.playlists index is a user (editable) playlist.
    [[nodiscard]] bool IsUserPlaylistId(std::uint64_t id) const noexcept;
    // True when the tree row can be drag-reordered (any Directory folder or User playlist).
    [[nodiscard]] bool IsReorderablePlaylistId(std::uint64_t id) const noexcept;
    // Sibling-group key used to scope a folder/playlist reorder.
    [[nodiscard]] std::uint64_t PlaylistReorderParent(std::uint64_t id) const noexcept;
};

} // namespace rivan::ui
