// Win32UiSkinStudioState.h
// Skin Studio editing state shared by the UI coordinator and its feature files.
#pragma once

#include "../skin/Skin.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d2d1.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rivan::ui {

struct Win32UiSkinStudioState {
    enum class StudioAction : std::uint64_t {
        AddRectangle = 20,
        AddEllipse = 21,
        AddLine = 22,
        ToggleShapeFill = 23,
        ImportImage = 30,
        SelectColors = 80,
        SelectGeneral = 81,
        SelectElements = 84,
        ShowElementEditor = 85,
        ShowLayers = 86,
        ToggleImageOverPanels = 122,
        ToggleImageOverScreens = 123,
        ToggleShapeOverScreens = 148,
        ToggleShapeOverPanels = 149,
        ScreenEyedropper = 91,
        OpenShapeRecolor = 151,
        OpenImageTint = 152,
        ClearImageTint = 153,
        ElementEyedropper = 154,
        ElementHexEdit = 155,
    };

    // Color picker can edit palette slots (Colors section) or decor recolor/tint (Elements).
    enum class StudioColorTarget : std::uint8_t { Palette, Shape, ImageTint };

    static constexpr std::uint64_t kSelectColorBase = 300;
    static constexpr std::uint64_t kOpenColorPickerBase = 400;
    static constexpr std::uint64_t kSelectImageBase = 500;
    static constexpr std::uint64_t kSelectShapeBase = 1000;
    static constexpr std::uint64_t kSelectLayerBase = 1200;

    [[nodiscard]] static constexpr std::uint64_t Action(StudioAction action) noexcept {
        return static_cast<std::uint64_t>(action);
    }

    struct ColorField { const wchar_t* name; skin::Color skin::SkinPalette::* member; };

    enum class StudioSection : std::uint8_t { General, Colors, Elements };

    static constexpr std::array<const wchar_t*, 6> kFontChoices{
        L"Segoe UI", L"Tahoma", L"Verdana", L"Lucida Console", L"Consolas", L"Courier New"};

    enum class DecorDragMode : std::uint8_t { Move, Resize, Rotate };

    // Draft is the working copy live-previewed through the host. studioOpen tracks
    // whether the draft was seeded from the active skin.
    skin::Skin studioDraft;
    bool studioOpen{};
    std::size_t studioColorIndex{};
    StudioSection studioSection{StudioSection::General};
    // HEX entry buffer for the color picker. Active when studioHexEditing is set; the
    // user types a #RRGGBB[AA] value that is applied to the selected color on Enter.
    std::wstring studioHex;
    bool studioHexEditing{};
    bool studioHexSelectAll{};
    D2D1_RECT_F studioColorPickerBounds{};
    D2D1_RECT_F studioHueBounds{};
    bool studioColorPickerVisible{};
    StudioColorTarget studioColorTarget{StudioColorTarget::Palette};
    std::uint64_t seenColorFocusRevision{};
    std::uint64_t seenElementFocusRevision{};
    bool studioNameEditing{};
    std::wstring studioName;
    bool draggingStudioColor{};
    bool draggingStudioHue{};
    // Screen-wide eyedropper: sample any desktop pixel into the selected studio color.
    bool pickingScreenColor{};
    bool eyedropperSkipUp{};
    std::size_t studioImageIndex{};
    std::size_t studioImageScroll{};
    std::size_t studioImageRows{};
    D2D1_RECT_F studioImageListBounds{};
    std::size_t studioShapeIndex{};
    bool studioImageFocused{};
    bool studioShapeFocused{};
    bool studioLayersTab{};
    std::size_t studioLayerScroll{};
    std::size_t studioLayerRows{};
    D2D1_RECT_F studioLayerBounds{};
    int draggingLayer{};
    std::size_t studioLayerDropPosition{};
    bool studioFontDropdown{};
    // Selected decor element for drag-move on the canvas: 0=none, else 1-based.
    // Positive = shape index+1, negative = -(image index+1).
    int draggingDecor{};
    DecorDragMode decorDragMode{DecorDragMode::Move};
    float dragStartAngle{};
    float dragStartRotation{};
    bool previewPending{};
    ULONGLONG lastPreviewTick{};
    D2D1_POINT_2F dragOffset{};
    D2D1_RECT_F studioPanelBounds{};
};

} // namespace rivan::ui
