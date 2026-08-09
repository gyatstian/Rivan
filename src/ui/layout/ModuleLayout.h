// ModuleLayout.h
// Public value model and operations for main-window module layout.
#pragma once

#include "ModuleDropZones.h"

#include <array>
#include <cstddef>
#include <optional>

namespace rivan::ui {

struct ModuleLayout final {
    std::array<ModuleLayoutItem, 6> items{};
    std::array<ModuleId, 6> tabOrder{};
    std::size_t tabCount{};
    std::size_t activeTab{};
    std::array<ModuleId, 6> snapGroup{
        ModuleId::Rivan, ModuleId::AllMusic, ModuleId::GraphicEqualizer,
        ModuleId::RivanLibrary, ModuleId::VideoPreview, ModuleId::Lyrics};

    [[nodiscard]] static ModuleLayout Defaults() noexcept;

    [[nodiscard]] ModuleLayoutItem* Find(ModuleId id) noexcept;
    [[nodiscard]] const ModuleLayoutItem* Find(ModuleId id) const noexcept;
    [[nodiscard]] bool IsTabbed(ModuleId id) const noexcept;
    [[nodiscard]] bool IsSnapped(ModuleId id) const noexcept;
    [[nodiscard]] bool IsCollapsed(ModuleId id) const noexcept;
    [[nodiscard]] bool IsEffectivelyCollapsed(ModuleId id) const noexcept;
    [[nodiscard]] bool IsCollapseHandleVisible(ModuleId id) const noexcept;
    void ClearModuleCollapse(ModuleId id) noexcept;
    void ClearCollapseReferences(ModuleId target) noexcept;

    [[nodiscard]] static bool IsCollapseSide(ModuleCollapseSide side) noexcept;
    [[nodiscard]] static bool IsHorizontalCollapseSide(ModuleCollapseSide side) noexcept;
    [[nodiscard]] static bool IsInsideCollapseOverlap(const ModuleLayoutItem& first,
                                                       const ModuleLayoutItem& second) noexcept;
    [[nodiscard]] bool CanCollapseSource(ModuleId source) const noexcept;
    [[nodiscard]] bool HasCollapseExpansionConflict(
        ModuleId source, const ModuleNormalizedRect& destination,
        std::optional<ModuleId> allowedTarget = {}) const noexcept;
    void SetCollapsedGeometry(ModuleLayoutItem& item, ModuleNormalizedRect handle,
                              ModuleNormalizedRect expanded, ModuleCollapseMode mode,
                              ModuleCollapseSide side, ModuleId target,
                              bool targetIsWindow) noexcept;
    static void SyncExpandedGeometry(ModuleLayoutItem& item) noexcept;
    void SetTabGroupGeometry(ModuleId root, ModuleNormalizedRect bounds) noexcept;
    [[nodiscard]] static ModuleNormalizedRect MakeInsideCollapseHandle(
        ModuleNormalizedRect bounds, ModuleCollapseSide side, float thickness,
        float length) noexcept;
    [[nodiscard]] static ModuleNormalizedRect ScaleBounds(
        ModuleNormalizedRect value, const ModuleNormalizedRect& oldBounds,
        const ModuleNormalizedRect& newBounds) noexcept;
    void ScaleCollapsedInsideModules(ModuleId targetRoot,
                                     const ModuleNormalizedRect& oldBounds,
                                     const ModuleNormalizedRect& newBounds) noexcept;
    [[nodiscard]] bool SquashForExpansion(ModuleId source,
                                           ModuleNormalizedRect expanded) noexcept;
    [[nodiscard]] bool ResizeForExpansion(ModuleId source,
                                          ModuleNormalizedRect expanded,
                                          float canvasWidth, float canvasHeight,
                                          float& newWidth, float& newHeight) noexcept;
    [[nodiscard]] bool CollapseToWindow(ModuleId source, ModuleCollapseSide side,
                                        float edgePosition = 0.5F) noexcept;
    [[nodiscard]] bool CollapseToModule(ModuleId source, ModuleId target,
                                        ModuleCollapseSide side, ModuleCollapseMode mode,
                                        float handleTrackThickness = 0.12F,
                                        float edgePosition = 0.5F) noexcept;
    [[nodiscard]] bool ToggleCollapsedModule(
        ModuleId id,
        ModuleExpansionBehavior behavior = ModuleExpansionBehavior::Squash,
        float canvasWidth = 0.0F, float canvasHeight = 0.0F,
        float* resizedWidth = nullptr, float* resizedHeight = nullptr) noexcept;

    [[nodiscard]] ModuleId SnapRoot(ModuleId id) const noexcept;
    [[nodiscard]] bool HasValidGeometry() const noexcept;
    [[nodiscard]] std::size_t TabCount() const noexcept;
    [[nodiscard]] std::size_t ActiveTabIndex() const noexcept;
    [[nodiscard]] static ModuleNormalizedRect Bounds(const ModuleLayoutItem& item) noexcept;
    [[nodiscard]] static bool Intersects(const ModuleNormalizedRect& first,
                                         const ModuleNormalizedRect& second) noexcept;
    [[nodiscard]] static bool Contains(const std::array<ModuleId, 6>& ids,
                                       std::size_t count, ModuleId id) noexcept;
    [[nodiscard]] std::size_t MovingMembers(ModuleId id,
                                              std::array<ModuleId, 6>& members) const noexcept;
    [[nodiscard]] bool HasGeometryConflict(ModuleId firstId,
                                            ModuleId secondId) const noexcept;
    [[nodiscard]] bool HasConflictingGeometry() const noexcept;
    [[nodiscard]] bool DisableDuplicateIndependentModules() noexcept;
    [[nodiscard]] bool HasNewConflictingGeometry(const ModuleLayout& before) const noexcept;
    [[nodiscard]] bool FindplusWindowRectangle(
        ModuleId source, ModuleNormalizedRect region, float pointerX, float pointerY,
        float minimumWidth, float minimumHeight, ModuleNormalizedRect& result,
        std::optional<ModuleCollapseSide> attachedSide = {},
        bool attachWindowEdge = false) const noexcept;
    [[nodiscard]] bool IsSnapGrouped(ModuleId id) const noexcept;
    void DetachSnapModule(ModuleId id) noexcept;
    void ResizeSnapGroup(ModuleId id, float pointerX, float pointerY,
                         bool resizeRight, bool resizeBottom,
                         bool resizeLeft, bool resizeTop) noexcept;
    [[nodiscard]] bool PreservePixelGeometry(float oldWidth, float oldHeight,
                                              float newWidth, float newHeight,
                                              bool resizeRight = false,
                                              bool resizeBottom = false,
                                              bool resizeLeft = false,
                                              bool resizeTop = false) noexcept;

    [[nodiscard]] ModuleId TabRoot(ModuleId id) const noexcept;
    void ClearTabs() noexcept;
    void MakeTab(ModuleId first, ModuleId second) noexcept;
    void RemoveTab(ModuleId id) noexcept;
    void TabWith(ModuleId source, ModuleId target) noexcept;
    [[nodiscard]] bool SnapTo(ModuleId source, ModuleId target,
                              ModuleDropZone zone) noexcept;
    [[nodiscard]] bool SnapToWindow(ModuleId source, ModuleWindowDropZone zone,
                                    float pointerX, float pointerY) noexcept;
};

} // namespace rivan::ui
