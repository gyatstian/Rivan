// Win32UiModuleState.h
// Main-window module gesture and geometry state.
#pragma once

#include "layout/ModuleLayout.h"

#include <d2d1.h>
#include <cstdint>
#include <optional>
#include <vector>

namespace rivan::ui {

struct Win32UiModuleState {
    struct ModuleRegion {
        ModuleId id{ModuleId::Rivan};
        D2D1_RECT_F bounds{};
    };

    enum class ModuleGesture : std::uint8_t { None, Move, Resize };

    std::vector<ModuleRegion> moduleRegions;
    std::optional<ModuleId> draggingModule;
    ModuleGesture moduleGesture{ModuleGesture::None};
    bool moduleResizeRight{};
    bool moduleResizeBottom{};
    bool moduleResizeLeft{};
    bool moduleResizeTop{};
    bool moduleDragActive{};
    bool moduleDetachTabOnMove{};
    bool moduleMoveTabbedGroup{};
    bool moduleMoveSnapGroup{};
    ModuleId moduleDragSnapRoot{ModuleId::Rivan};
    D2D1_POINT_2F moduleDragStart{};
    D2D1_POINT_2F moduleDragOffset{};
    std::optional<ModuleId> moduleDropTarget;
    ModuleDropZone moduleDropZone{ModuleDropZone::None};
    ModuleWindowDropZone moduleWindowDropZone{ModuleWindowDropZone::None};
    std::optional<ModuleId> moduleCollapseTarget;
    ModuleCollapseSide moduleCollapseSide{ModuleCollapseSide::None};
    ModuleCollapseMode moduleCollapseMode{ModuleCollapseMode::None};
    bool moduleCollapseTargetIsWindow{};
    std::optional<ModuleId> collapsedArrowPress;
    D2D1_POINT_2F collapsedArrowPressStart{};
    D2D1_RECT_F collapsedArrowPressBounds{};
    bool collapsedArrowDragStarted{};
    bool moduleDragFromCollapsedArrow{};
    D2D1_RECT_F moduleCollapsedArrowOrigin{};
    ModuleLayout moduleLayoutPreview{ModuleLayout::Defaults()};
    bool moduleDropPreviewValid{};
    D2D1_POINT_2F moduleDropLastPointer{-1.0F, -1.0F};
    ModuleLayout moduleLayoutDraft{ModuleLayout::Defaults()};
    bool moduleExpansionResizePending{};
    std::optional<ModuleId> moduleExpansionResizeModule;
    ModuleLayout moduleExpansionRestoreLayout{ModuleLayout::Defaults()};
    bool internalModuleResize{};
    int moduleExpansionRestoreWidth{};
    int moduleExpansionRestoreHeight{};
};

} // namespace rivan::ui
