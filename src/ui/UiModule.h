// UiModule.h
// Stable identities for the top-level sections of the main Rivan window.
#pragma once

#include <array>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace rivan::ui {

// A module id is the identity of a section, not its current position on screen. Keeping
// that distinction here gives future layout code a stable key for persistence, drag/drop,
// tabs, and replacement without making any of those behaviours part of the UI yet.
enum class ModuleId : std::uint8_t {
    Rivan,
    AllMusic,
    GraphicEqualizer,
    RivanLibrary,
    VideoPreview,
};

// Floating modules can be placed freely.  A side drop changes the placement to
// Snapped; the state is persisted separately from the rectangle so callers can
// distinguish an intentional dock from a coincidentally adjacent floating panel.
enum class ModuleDockState : std::uint8_t {
    Floating,
    Snapped,
};

enum class ModuleDropZone : std::uint8_t {
    None,
    Center,
    Left,
    Right,
    Top,
    Bottom,
};

// Drop targets which belong to the client window rather than to another module.
// The names describe the visual target, while the actual destination is calculated
// from the currently unoccupied part of that target.  This is important for nested
// snaps: a right-middle drop must not overwrite a module already occupying the
// right-top part of the window.
enum class ModuleWindowDropZone : std::uint8_t {
    None,
    RightTop,
    RightMiddle,
    RightBottom,
    Center,
    LeftTop,
    LeftMiddle,
    LeftBottom,
};

// A collapsible module keeps its expanded rectangle in the normal layout fields and
// exposes a small handle while collapsed. Inside drops split target space between source
// and target; outside drops expand in adjacent free space.
enum class ModuleCollapseMode : std::uint8_t {
    None,
    Inside,
    Outside,
};

enum class ModuleCollapseSide : std::uint8_t {
    None,
    Left,
    Right,
    Top,
    Bottom,
};

enum class ModuleExpansionBehavior : std::uint8_t {
    Squash,
    Resize,
};

struct ModuleNormalizedRect final {
    float left{};
    float top{};
    float right{};
    float bottom{};
};

[[nodiscard]] inline bool IsWindowDrop(ModuleWindowDropZone zone) noexcept {
    return zone != ModuleWindowDropZone::None;
}

// Resolve the seven window targets from normalized client coordinates.  The middle
// target is deliberately a smaller central area so the two side-middle targets remain
// easy to select at the far left/right edges.
[[nodiscard]] inline ModuleWindowDropZone ResolveModuleWindowDropZone(float x, float y) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0.0F || x > 1.0F ||
        y < 0.0F || y > 1.0F) {
        return ModuleWindowDropZone::None;
    }
    if (x >= 0.25F && x <= 0.75F && y >= 0.25F && y <= 0.75F) {
        return ModuleWindowDropZone::Center;
    }
    const bool right = x >= 0.5F;
    if (y < 0.25F) {
        return right ? ModuleWindowDropZone::RightTop : ModuleWindowDropZone::LeftTop;
    }
    if (y > 0.75F) {
        return right ? ModuleWindowDropZone::RightBottom : ModuleWindowDropZone::LeftBottom;
    }
    return right ? ModuleWindowDropZone::RightMiddle : ModuleWindowDropZone::LeftMiddle;
}

[[nodiscard]] inline ModuleNormalizedRect ModuleWindowDropBounds(
    ModuleWindowDropZone zone) noexcept {
    switch (zone) {
    case ModuleWindowDropZone::RightTop: return {0.5F, 0.0F, 1.0F, 0.5F};
    case ModuleWindowDropZone::RightMiddle: return {0.5F, 0.0F, 1.0F, 1.0F};
    case ModuleWindowDropZone::RightBottom: return {0.5F, 0.5F, 1.0F, 1.0F};
    case ModuleWindowDropZone::Center: return {0.0F, 0.0F, 1.0F, 1.0F};
    case ModuleWindowDropZone::LeftTop: return {0.0F, 0.0F, 0.5F, 0.5F};
    case ModuleWindowDropZone::LeftMiddle: return {0.0F, 0.0F, 0.5F, 1.0F};
    case ModuleWindowDropZone::LeftBottom: return {0.0F, 0.5F, 0.5F, 1.0F};
    case ModuleWindowDropZone::None: break;
    }
    return {};
}

[[nodiscard]] inline bool IsSideDrop(ModuleDropZone zone) noexcept {
    return zone == ModuleDropZone::Left || zone == ModuleDropZone::Right ||
           zone == ModuleDropZone::Top || zone == ModuleDropZone::Bottom;
}

// The middle half of a target is reserved for tab merging.  The remaining
// quarters select a side.  At a corner the vertical edge wins ties, making the
// result deterministic while still making the whole edge easy to hit.
[[nodiscard]] inline ModuleDropZone ResolveModuleDropZone(float x, float y,
                                                           float left, float top,
                                                           float right, float bottom) noexcept {
    const float width = right - left;
    const float height = bottom - top;
    if (!(width > 0.0F) || !(height > 0.0F) || x < left || x > right || y < top || y > bottom) {
        return ModuleDropZone::None;
    }
    const float normalizedX = (x - left) / width;
    const float normalizedY = (y - top) / height;
    if (normalizedX >= 0.25F && normalizedX <= 0.75F &&
        normalizedY >= 0.25F && normalizedY <= 0.75F) {
        return ModuleDropZone::Center;
    }

    const float horizontalDistance = std::min(normalizedX, 1.0F - normalizedX);
    const float verticalDistance = std::min(normalizedY, 1.0F - normalizedY);
    if (horizontalDistance < verticalDistance) {
        return normalizedX < 0.5F ? ModuleDropZone::Left : ModuleDropZone::Right;
    }
    return normalizedY < 0.5F ? ModuleDropZone::Top : ModuleDropZone::Bottom;
}

struct ModuleLayoutItem final {
    ModuleId id{ModuleId::Rivan};
    float x{};
    float y{};
    float width{1.0F};
    float height{1.0F};
    bool visible{true};
    ModuleDockState dockState{ModuleDockState::Floating};
    ModuleCollapseMode collapseMode{ModuleCollapseMode::None};
    ModuleCollapseSide collapseSide{ModuleCollapseSide::None};
    ModuleId collapseTarget{ModuleId::Rivan};
    bool collapseTargetIsWindow{};
    bool collapsed{};
    float expandedX{};
    float expandedY{};
    float expandedWidth{};
    float expandedHeight{};
    float handleX{};
    float handleY{};
    float handleWidth{};
    float handleHeight{};
};

struct ModuleLayout final {
    std::array<ModuleLayoutItem, 5> items{};
    // A non-empty group contains the visible modules sharing one tab strip. The
    // first id is the active tab for that group.
    std::array<ModuleId, 5> tabOrder{};
    std::size_t tabCount{};
    std::size_t activeTab{};
    // A snapped module points at the module which owns its snap group.  The owner
    // is the target of the side drop, so dragging that module moves the whole group;
    // dragging another member detaches only that member.  A module points to itself
    // while it is not part of a snap group.
    std::array<ModuleId, 5> snapGroup{
        ModuleId::Rivan, ModuleId::AllMusic, ModuleId::GraphicEqualizer,
        ModuleId::RivanLibrary, ModuleId::VideoPreview};

    [[nodiscard]] static ModuleLayout Defaults() noexcept {
        return ModuleLayout{
            {{{ModuleId::Rivan, 0.0F, 0.0F, 0.44F, 0.30F, true, ModuleDockState::Floating},
              {ModuleId::AllMusic, 0.0F, 0.33F, 0.44F, 0.49F, true, ModuleDockState::Floating},
              {ModuleId::GraphicEqualizer, 0.0F, 0.84F, 0.44F, 0.16F, true, ModuleDockState::Floating},
              {ModuleId::RivanLibrary, 0.46F, 0.0F, 0.54F, 0.66F, true, ModuleDockState::Floating},
              {ModuleId::VideoPreview, 0.46F, 0.68F, 0.54F, 0.30F, true, ModuleDockState::Floating}}},
             {}, 0, 0,
              {ModuleId::Rivan, ModuleId::AllMusic, ModuleId::GraphicEqualizer,
               ModuleId::RivanLibrary, ModuleId::VideoPreview}};
    }

    [[nodiscard]] ModuleLayoutItem* Find(ModuleId id) noexcept {
        for (auto& item : items) if (item.id == id) return &item;
        return nullptr;
    }

    [[nodiscard]] const ModuleLayoutItem* Find(ModuleId id) const noexcept {
        for (const auto& item : items) if (item.id == id) return &item;
        return nullptr;
    }

    [[nodiscard]] bool IsTabbed(ModuleId id) const noexcept {
        const auto count = TabCount();
        if (count < 2) return false;
        for (std::size_t i = 0; i < count; ++i) {
            if (Find(tabOrder[i]) != nullptr && tabOrder[i] == id) return true;
        }
        return false;
    }

    [[nodiscard]] bool IsSnapped(ModuleId id) const noexcept {
        const auto* item = Find(id);
        return item != nullptr && item->dockState == ModuleDockState::Snapped;
    }

    [[nodiscard]] bool IsCollapsed(ModuleId id) const noexcept {
        const auto* item = Find(id);
        return item != nullptr && item->collapsed &&
               item->collapseMode != ModuleCollapseMode::None;
    }

    void ClearModuleCollapse(ModuleId id) noexcept {
        if (auto* item = Find(id)) {
            item->collapseMode = ModuleCollapseMode::None;
            item->collapseSide = ModuleCollapseSide::None;
            item->collapseTarget = id;
            item->collapseTargetIsWindow = false;
            item->collapsed = false;
        }
    }

    void ClearCollapseReferences(ModuleId target) noexcept {
        for (const auto& candidate : items) {
            if (candidate.id != target && candidate.collapseTarget == target &&
                candidate.collapseMode != ModuleCollapseMode::None) {
                ClearModuleCollapse(candidate.id);
            }
        }
    }

    [[nodiscard]] static bool IsCollapseSide(ModuleCollapseSide side) noexcept {
        return side != ModuleCollapseSide::None;
    }

    [[nodiscard]] static bool IsHorizontalCollapseSide(ModuleCollapseSide side) noexcept {
        return side == ModuleCollapseSide::Left || side == ModuleCollapseSide::Right;
    }

    [[nodiscard]] static bool IsInsideCollapseOverlap(const ModuleLayoutItem& first,
                                                       const ModuleLayoutItem& second) noexcept {
        return first.collapsed && first.collapseMode == ModuleCollapseMode::Inside &&
               !first.collapseTargetIsWindow && first.collapseTarget == second.id;
    }

    [[nodiscard]] bool CanCollapseSource(ModuleId source) const noexcept {
        const auto* item = Find(source);
        return item != nullptr && item->visible && !IsTabbed(source) &&
               (!IsSnapped(source) || !IsSnapGrouped(source));
    }

    [[nodiscard]] bool HasCollapseExpansionConflict(ModuleId source,
                                                     const ModuleNormalizedRect& destination,
                                                     std::optional<ModuleId> allowedTarget = {}) const noexcept {
        for (const auto& item : items) {
            if (!item.visible || item.id == source || item.collapsed) continue;
            if (allowedTarget && item.id == *allowedTarget) continue;
            if (allowedTarget && IsTabbed(item.id) && TabRoot(item.id) == *allowedTarget) continue;
            if (Intersects(destination, Bounds(item))) return true;
        }
        return false;
    }

    void SetCollapsedGeometry(ModuleLayoutItem& item, ModuleNormalizedRect handle,
                              ModuleNormalizedRect expanded,
                              ModuleCollapseMode mode, ModuleCollapseSide side,
                              ModuleId target, bool targetIsWindow) noexcept {
        item.expandedX = expanded.left;
        item.expandedY = expanded.top;
        item.expandedWidth = expanded.right - expanded.left;
        item.expandedHeight = expanded.bottom - expanded.top;
        item.handleX = handle.left;
        item.handleY = handle.top;
        item.handleWidth = handle.right - handle.left;
        item.handleHeight = handle.bottom - handle.top;
        item.x = handle.left;
        item.y = handle.top;
        item.width = handle.right - handle.left;
        item.height = handle.bottom - handle.top;
        item.collapseMode = mode;
        item.collapseSide = side;
        item.collapseTarget = target;
        item.collapseTargetIsWindow = targetIsWindow;
        item.collapsed = true;
        item.dockState = ModuleDockState::Snapped;
    }

    // Keep the rectangle restored by a later collapse in sync with an expanded
    // collapsible module's current geometry.
    static void SyncExpandedGeometry(ModuleLayoutItem& item) noexcept {
        if (item.collapseMode == ModuleCollapseMode::None || item.collapsed) return;
        item.expandedX = item.x;
        item.expandedY = item.y;
        item.expandedWidth = item.width;
        item.expandedHeight = item.height;
    }

    void SetTabGroupGeometry(ModuleId root, ModuleNormalizedRect bounds) noexcept {
        for (auto& candidate : items) {
            const bool belongsToTarget = candidate.id == root ||
                (IsTabbed(candidate.id) && TabRoot(candidate.id) == root);
            if (!belongsToTarget) continue;
            candidate.x = bounds.left;
            candidate.y = bounds.top;
            candidate.width = bounds.right - bounds.left;
            candidate.height = bounds.bottom - bounds.top;
            SyncExpandedGeometry(candidate);
        }
    }

    [[nodiscard]] static ModuleNormalizedRect MakeInsideCollapseHandle(
        ModuleNormalizedRect bounds, ModuleCollapseSide side,
        float thickness, float length) noexcept {
        const float width = std::max(0.0F, bounds.right - bounds.left);
        const float height = std::max(0.0F, bounds.bottom - bounds.top);
        if (IsHorizontalCollapseSide(side)) {
            thickness = std::min(thickness, width);
            length = std::min(length, height);
            const float centerY = (bounds.top + bounds.bottom) * 0.5F;
            if (side == ModuleCollapseSide::Left) {
                return {bounds.left, centerY - length * 0.5F,
                        bounds.left + thickness, centerY + length * 0.5F};
            }
            return {bounds.right - thickness, centerY - length * 0.5F,
                    bounds.right, centerY + length * 0.5F};
        }

        thickness = std::min(thickness, height);
        length = std::min(length, width);
        const float centerX = (bounds.left + bounds.right) * 0.5F;
        if (side == ModuleCollapseSide::Top) {
            return {centerX - length * 0.5F, bounds.top,
                    centerX + length * 0.5F, bounds.top + thickness};
        }
        return {centerX - length * 0.5F, bounds.bottom - thickness,
                centerX + length * 0.5F, bounds.bottom};
    }

    [[nodiscard]] static ModuleNormalizedRect ScaleBounds(
        ModuleNormalizedRect value, const ModuleNormalizedRect& oldBounds,
        const ModuleNormalizedRect& newBounds) noexcept {
        const float oldWidth = oldBounds.right - oldBounds.left;
        const float oldHeight = oldBounds.bottom - oldBounds.top;
        if (!(oldWidth > 0.0F) || !(oldHeight > 0.0F)) return value;
        const float scaleX = (newBounds.right - newBounds.left) / oldWidth;
        const float scaleY = (newBounds.bottom - newBounds.top) / oldHeight;
        return {newBounds.left + (value.left - oldBounds.left) * scaleX,
                newBounds.top + (value.top - oldBounds.top) * scaleY,
                newBounds.left + (value.right - oldBounds.left) * scaleX,
                newBounds.top + (value.bottom - oldBounds.top) * scaleY};
    }

    // Collapsed inside modules are visual children of their target, rather than snap
    // members. Scale their stored panel and handle bounds with that target.
    void ScaleCollapsedInsideModules(ModuleId targetRoot,
                                     const ModuleNormalizedRect& oldBounds,
                                     const ModuleNormalizedRect& newBounds) noexcept {
        for (auto& candidate : items) {
            if (!candidate.visible || !candidate.collapsed ||
                candidate.collapseMode != ModuleCollapseMode::Inside ||
                candidate.collapseTargetIsWindow ||
                SnapRoot(TabRoot(candidate.collapseTarget)) != targetRoot) {
                continue;
            }
            const auto expanded = ScaleBounds(
                {candidate.expandedX, candidate.expandedY,
                 candidate.expandedX + candidate.expandedWidth,
                 candidate.expandedY + candidate.expandedHeight}, oldBounds, newBounds);
            const auto handle = ScaleBounds(
                {candidate.handleX, candidate.handleY,
                 candidate.handleX + candidate.handleWidth,
                 candidate.handleY + candidate.handleHeight}, oldBounds, newBounds);
            candidate.expandedX = expanded.left;
            candidate.expandedY = expanded.top;
            candidate.expandedWidth = expanded.right - expanded.left;
            candidate.expandedHeight = expanded.bottom - expanded.top;
            candidate.handleX = handle.left;
            candidate.handleY = handle.top;
            candidate.handleWidth = handle.right - handle.left;
            candidate.handleHeight = handle.bottom - handle.top;
            candidate.x = handle.left;
            candidate.y = handle.top;
            candidate.width = candidate.handleWidth;
            candidate.height = candidate.handleHeight;
        }
    }

    // Make room for an expanded module by shrinking intersecting modules. Preserve
    // largest available rectangle on each obstacle; reject result if new overlaps appear.
    [[nodiscard]] bool SquashForExpansion(ModuleId source,
                                           ModuleNormalizedRect expanded) noexcept {
        const ModuleLayout before = *this;
        for (auto& obstacle : items) {
            if (!obstacle.visible || obstacle.id == source || obstacle.collapsed ||
                !Intersects(expanded, Bounds(obstacle))) {
                continue;
            }

            struct Candidate {
                ModuleNormalizedRect bounds{};
                float area{};
            } best{};
            const auto consider = [&best](ModuleNormalizedRect bounds) {
                const float width = bounds.right - bounds.left;
                const float height = bounds.bottom - bounds.top;
                if (width < 0.10F || height < 0.10F) return;
                const float area = width * height;
                if (area > best.area) best = {bounds, area};
            };
            const auto current = Bounds(obstacle);
            const float currentWidth = current.right - current.left;
            const float currentHeight = current.bottom - current.top;
            const float minimum = 0.10F;
            const auto sideCandidate = [&](ModuleNormalizedRect bounds) {
                if (bounds.right - bounds.left >= minimum &&
                    bounds.bottom - bounds.top >= minimum && !Intersects(bounds, expanded)) {
                    consider(bounds);
                }
            };
            const float rightWidth = std::min(currentWidth, 1.0F - expanded.right);
            sideCandidate({expanded.right, current.top,
                           expanded.right + rightWidth, current.bottom});
            const float leftWidth = std::min(currentWidth, expanded.left);
            sideCandidate({expanded.left - leftWidth, current.top,
                           expanded.left, current.bottom});
            const float bottomHeight = std::min(currentHeight, 1.0F - expanded.bottom);
            sideCandidate({current.left, expanded.bottom,
                           current.right, expanded.bottom + bottomHeight});
            const float topHeight = std::min(currentHeight, expanded.top);
            sideCandidate({current.left, expanded.top - topHeight,
                           current.right, expanded.top});
            if (best.area <= 0.0F) {
                *this = before;
                return false;
            }
            obstacle.x = best.bounds.left;
            obstacle.y = best.bounds.top;
            obstacle.width = best.bounds.right - best.bounds.left;
            obstacle.height = best.bounds.bottom - best.bounds.top;
            SyncExpandedGeometry(obstacle);
        }
        if (HasNewConflictingGeometry(before)) {
            *this = before;
            return false;
        }
        return true;
    }

    // Add canvas space instead of shrinking obstacles. Existing module pixel sizes stay
    // unchanged; modules intersecting expansion are shifted to the right/bottom.
    [[nodiscard]] bool ResizeForExpansion(ModuleId source, ModuleNormalizedRect expanded,
                                          float canvasWidth, float canvasHeight,
                                          float& newWidth, float& newHeight) noexcept {
        if (!(canvasWidth > 0.0F) || !(canvasHeight > 0.0F)) return false;
        const auto* sourceItem = Find(source);
        if (!sourceItem) return false;
        const bool horizontal = IsHorizontalCollapseSide(sourceItem->collapseSide);
        const bool moveSource = sourceItem->collapseSide == ModuleCollapseSide::Right ||
                                sourceItem->collapseSide == ModuleCollapseSide::Bottom;
        const float oldExtent = horizontal ? canvasWidth : canvasHeight;
        const float sourceLeft = expanded.left * canvasWidth;
        const float sourceTop = expanded.top * canvasHeight;
        const float sourceRight = expanded.right * canvasWidth;
        const float sourceBottom = expanded.bottom * canvasHeight;
        constexpr float gap = 8.0F;
        float extra = 0.0F;
        bool hasObstacleOverlap = false;
        for (const auto& obstacle : items) {
            if (!obstacle.visible || obstacle.id == source || obstacle.collapsed ||
                !Intersects(expanded, Bounds(obstacle))) {
                continue;
            }
            hasObstacleOverlap = true;
            if (horizontal) {
                extra = std::max(extra, moveSource
                    ? obstacle.x * canvasWidth + obstacle.width * canvasWidth - sourceLeft + gap
                    : sourceRight - obstacle.x * canvasWidth + gap);
            } else {
                extra = std::max(extra, moveSource
                    ? obstacle.y * canvasHeight + obstacle.height * canvasHeight - sourceTop + gap
                    : sourceBottom - obstacle.y * canvasHeight + gap);
            }
        }
        if (horizontal) {
            extra = std::max(extra, std::max(0.0F, -sourceLeft + gap));
            extra = std::max(extra, std::max(0.0F, sourceRight - canvasWidth + gap));
        } else {
            extra = std::max(extra, std::max(0.0F, -sourceTop + gap));
            extra = std::max(extra, std::max(0.0F, sourceBottom - canvasHeight + gap));
        }
        if (!(extra > 0.0F) || !std::isfinite(extra)) return false;
        const float extent = oldExtent + extra;
        if (extent > 16384.0F) return false;

        const ModuleLayout before = *this;
        struct PixelRect {
            float x{};
            float y{};
            float width{};
            float height{};
        };
        const auto pixels = [canvasWidth, canvasHeight](const ModuleLayoutItem& item) {
            return PixelRect{item.x * canvasWidth, item.y * canvasHeight,
                             item.width * canvasWidth, item.height * canvasHeight};
        };
        const auto normalize = [canvasWidth, canvasHeight, extent, horizontal](
                                   ModuleLayoutItem& item, PixelRect rect) {
            const float width = horizontal ? extent : canvasWidth;
            const float height = horizontal ? canvasHeight : extent;
            item.x = rect.x / width;
            item.y = rect.y / height;
            item.width = rect.width / width;
            item.height = rect.height / height;
        };
        const bool shiftSource = hasObstacleOverlap && moveSource;
        const auto shiftCollapseGeometry = [canvasWidth, canvasHeight, extent, horizontal,
                                            shiftSource, extra](ModuleLayoutItem& item) {
            if (item.collapseMode == ModuleCollapseMode::None) return;
            const float width = horizontal ? extent : canvasWidth;
            const float height = horizontal ? canvasHeight : extent;
            const float shift = !shiftSource ? extra : 0.0F;
            if (horizontal) {
                item.handleX = (item.handleX * canvasWidth + shift) / width;
                item.handleWidth = item.handleWidth * canvasWidth / width;
                item.expandedX = (item.expandedX * canvasWidth + shift) / width;
                item.expandedWidth = item.expandedWidth * canvasWidth / width;
            } else {
                item.handleY = (item.handleY * canvasHeight + shift) / height;
                item.handleHeight = item.handleHeight * canvasHeight / height;
                item.expandedY = (item.expandedY * canvasHeight + shift) / height;
                item.expandedHeight = item.expandedHeight * canvasHeight / height;
            }
        };
        for (auto& item : items) {
            if (!item.visible || item.id == source) continue;
            auto obstacle = pixels(item);
            const bool overlaps = !item.collapsed &&
                obstacle.x < sourceRight && obstacle.x + obstacle.width > sourceLeft &&
                obstacle.y < sourceBottom && obstacle.y + obstacle.height > sourceTop;
            if (overlaps && !moveSource) {
                if (horizontal) obstacle.x += extra;
                else obstacle.y += extra;
            }
            normalize(item, obstacle);
            shiftCollapseGeometry(item);
        }
        if (auto* expandedSource = Find(source)) {
            const float x = sourceLeft + (horizontal && shiftSource ? extra : 0.0F);
            const float y = sourceTop + (!horizontal && shiftSource ? extra : 0.0F);
            normalize(*expandedSource, {x, y, sourceRight - sourceLeft, sourceBottom - sourceTop});
        }
        for (auto& item : items) {
            if (!item.visible) continue;
            SyncExpandedGeometry(item);
        }
        if (HasNewConflictingGeometry(before)) {
            *this = before;
            return false;
        }
        newWidth = horizontal ? extent : canvasWidth;
        newHeight = horizontal ? canvasHeight : extent;
        return true;
    }

    // Collapse a module to a narrow handle on a client-window edge. Its stored geometry
    // is the rectangle revealed when the handle is expanded again.
    [[nodiscard]] bool CollapseToWindow(ModuleId source, ModuleCollapseSide side,
                                        float edgePosition = 0.5F) noexcept {
        if (!CanCollapseSource(source) || !IsCollapseSide(side)) return false;
        const auto* sourceItem = Find(source);
        if (!sourceItem) return false;
        const ModuleLayout before = *this;
        const float width = sourceItem->width;
        const float height = sourceItem->height;
        if (width < 0.10F || height < 0.10F) return false;

        edgePosition = std::clamp(std::isfinite(edgePosition) ? edgePosition : 0.5F,
                                  0.0F, 1.0F);
        ModuleNormalizedRect expanded{};
        ModuleNormalizedRect handle = expanded;
        if (IsHorizontalCollapseSide(side)) {
            constexpr float handleWidth = 0.06F;
            const float handleHeight = std::clamp(height * 0.22F, 0.08F, 0.18F);
            ModuleNormalizedRect available{};
            const float pointerY = edgePosition;
            if (!FindplusWindowRectangle(source, {0.0F, 0.0F, 1.0F, 1.0F},
                                         side == ModuleCollapseSide::Left ? 0.0F : 1.0F,
                                         pointerY, 0.10F, 0.10F, available, side, true)) {
                return false;
            }
            const float fittedWidth = std::min(width, available.right - available.left);
            const float fittedHeight = std::min(height, available.bottom - available.top);
            const float top = std::clamp(edgePosition - fittedHeight * 0.5F,
                                         available.top, available.bottom - fittedHeight);
            expanded.left = side == ModuleCollapseSide::Left ? 0.0F : 1.0F - fittedWidth;
            expanded.right = expanded.left + fittedWidth;
            expanded.top = top;
            expanded.bottom = top + fittedHeight;
            handle.left = side == ModuleCollapseSide::Left ? 0.0F : 1.0F - handleWidth;
            handle.right = handle.left + handleWidth;
            const float center = expanded.top + fittedHeight * 0.5F;
            handle.top = std::clamp(center - handleHeight * 0.5F, 0.0F, 1.0F - handleHeight);
            handle.bottom = handle.top + handleHeight;
        } else {
            constexpr float handleHeight = 0.06F;
            const float handleWidth = std::clamp(width * 0.22F, 0.08F, 0.18F);
            ModuleNormalizedRect available{};
            const float pointerX = edgePosition;
            if (!FindplusWindowRectangle(source, {0.0F, 0.0F, 1.0F, 1.0F},
                                         pointerX, side == ModuleCollapseSide::Top ? 0.0F : 1.0F,
                                         0.10F, 0.10F, available, side, true)) {
                return false;
            }
            const float fittedWidth = std::min(width, available.right - available.left);
            const float fittedHeight = std::min(height, available.bottom - available.top);
            const float left = std::clamp(edgePosition - fittedWidth * 0.5F,
                                          available.left, available.right - fittedWidth);
            expanded.left = left;
            expanded.right = left + fittedWidth;
            expanded.top = side == ModuleCollapseSide::Top ? 0.0F : 1.0F - fittedHeight;
            expanded.bottom = expanded.top + fittedHeight;
            handle.top = side == ModuleCollapseSide::Top ? 0.0F : 1.0F - handleHeight;
            handle.bottom = handle.top + handleHeight;
            const float center = expanded.left + fittedWidth * 0.5F;
            handle.left = std::clamp(center - handleWidth * 0.5F, 0.0F, 1.0F - handleWidth);
            handle.right = handle.left + handleWidth;
        }
        if (HasCollapseExpansionConflict(source, expanded)) return false;
        // The handle intentionally occupies the tiny edge affordance even when an
        // existing panel already reaches that edge. It is not a normal layout object;
        // HasConflictingGeometry ignores collapsed items after this operation.
        auto* item = Find(source);
        if (!item) return false;
        SetCollapsedGeometry(*item, handle, expanded, ModuleCollapseMode::Outside, side,
                             source, true);
        snapGroup[static_cast<std::size_t>(item - items.data())] = source;
        if (HasNewConflictingGeometry(before)) {
            *this = before;
            return false;
        }
        return true;
    }

    // Collapse a module against a module side. Inside drops split target space when
    // expanded; the compact handle overlays the target edge while collapsed. Outside
    // drops reserve source's current size beside target without unrelated overlap.
    [[nodiscard]] bool CollapseToModule(ModuleId source, ModuleId target,
                                        ModuleCollapseSide side,
                                        ModuleCollapseMode mode,
                                        float handleTrackThickness = 0.12F,
                                        float edgePosition = 0.5F) noexcept {
        if (!CanCollapseSource(source) || source == target || !IsCollapseSide(side) ||
            (mode != ModuleCollapseMode::Inside && mode != ModuleCollapseMode::Outside)) {
            return false;
        }
        const auto* sourceItem = Find(source);
        const auto targetRoot = TabRoot(target);
        const auto* targetItem = Find(targetRoot);
        if (!sourceItem || !targetItem || !targetItem->visible || targetItem->collapsed ||
            IsTabbed(source) && IsTabbed(target) && TabRoot(source) == targetRoot) {
            return false;
        }
        const ModuleLayout before = *this;
        const auto targetBounds = Bounds(*targetItem);
        ModuleNormalizedRect expanded{};
        ModuleNormalizedRect handle{};
        ModuleNormalizedRect collapsedTargetBounds{};
        if (mode == ModuleCollapseMode::Inside) {
            if (IsHorizontalCollapseSide(side)) {
                const float middle = (targetBounds.left + targetBounds.right) * 0.5F;
                expanded = side == ModuleCollapseSide::Left
                    ? ModuleNormalizedRect{targetBounds.left, targetBounds.top, middle,
                                           targetBounds.bottom}
                    : ModuleNormalizedRect{middle, targetBounds.top, targetBounds.right,
                                           targetBounds.bottom};
            } else {
                const float middle = (targetBounds.top + targetBounds.bottom) * 0.5F;
                expanded = side == ModuleCollapseSide::Top
                    ? ModuleNormalizedRect{targetBounds.left, targetBounds.top,
                                           targetBounds.right, middle}
                    : ModuleNormalizedRect{targetBounds.left, middle,
                                           targetBounds.right, targetBounds.bottom};
            }
            // Store compact button thickness. Renderer uses same thickness for every
            // orientation and keeps target gap within one button dimension.
            const float handleLength = IsHorizontalCollapseSide(side)
                ? (targetBounds.bottom - targetBounds.top) * 0.20F
                : (targetBounds.right - targetBounds.left) * 0.20F;
            handle = MakeInsideCollapseHandle(targetBounds, side, handleTrackThickness,
                                               handleLength);
            collapsedTargetBounds = targetBounds;
            switch (side) {
            case ModuleCollapseSide::Left: collapsedTargetBounds.left = handle.right; break;
            case ModuleCollapseSide::Right: collapsedTargetBounds.right = handle.left; break;
            case ModuleCollapseSide::Top: collapsedTargetBounds.top = handle.bottom; break;
            case ModuleCollapseSide::Bottom: collapsedTargetBounds.bottom = handle.top; break;
            case ModuleCollapseSide::None: break;
            }
            if (collapsedTargetBounds.right - collapsedTargetBounds.left < 0.10F ||
                collapsedTargetBounds.bottom - collapsedTargetBounds.top < 0.10F ||
                HasCollapseExpansionConflict(source, expanded, targetRoot)) {
                return false;
            }
        } else {
            const float width = sourceItem->width;
            const float height = sourceItem->height;
            if (width < 0.10F || height < 0.10F) return false;
            edgePosition = std::clamp(std::isfinite(edgePosition) ? edgePosition : 0.5F,
                                      0.0F, 1.0F);
            const ModuleNormalizedRect outsideRegion = [&] {
                switch (side) {
                case ModuleCollapseSide::Left:
                    return ModuleNormalizedRect{0.0F, 0.0F, targetBounds.left, 1.0F};
                case ModuleCollapseSide::Right:
                    return ModuleNormalizedRect{targetBounds.right, 0.0F, 1.0F, 1.0F};
                case ModuleCollapseSide::Top:
                    return ModuleNormalizedRect{0.0F, 0.0F, 1.0F, targetBounds.top};
                case ModuleCollapseSide::Bottom:
                    return ModuleNormalizedRect{0.0F, targetBounds.bottom, 1.0F, 1.0F};
                case ModuleCollapseSide::None:
                    return ModuleNormalizedRect{};
                }
                return ModuleNormalizedRect{};
            }();
            ModuleNormalizedRect available{};
            const float pointerX = IsHorizontalCollapseSide(side)
                ? (side == ModuleCollapseSide::Left ? outsideRegion.right : outsideRegion.left)
                : edgePosition;
            const float pointerY = IsHorizontalCollapseSide(side)
                ? edgePosition
                : (side == ModuleCollapseSide::Top ? outsideRegion.bottom : outsideRegion.top);
            if (!FindplusWindowRectangle(source, outsideRegion, pointerX, pointerY,
                                         0.10F, 0.10F, available, side)) {
                return false;
            }

            const float expandedWidth = std::min(width, available.right - available.left);
            const float expandedHeight = std::min(height, available.bottom - available.top);
            if (side == ModuleCollapseSide::Left || side == ModuleCollapseSide::Right) {
                const float top = std::clamp(edgePosition - expandedHeight * 0.5F,
                                             available.top, available.bottom - expandedHeight);
                if (side == ModuleCollapseSide::Left) {
                    expanded = {targetBounds.left - expandedWidth, top, targetBounds.left,
                                top + expandedHeight};
                } else {
                    expanded = {targetBounds.right, top, targetBounds.right + expandedWidth,
                                top + expandedHeight};
                }
            } else {
                const float left = std::clamp(edgePosition - expandedWidth * 0.5F,
                                              available.left, available.right - expandedWidth);
                if (side == ModuleCollapseSide::Top) {
                    expanded = {left, targetBounds.top - expandedHeight, left + expandedWidth,
                                targetBounds.top};
                } else {
                    expanded = {left, targetBounds.bottom, left + expandedWidth,
                                targetBounds.bottom + expandedHeight};
                }
            }
            handle = expanded;
            constexpr float handleThickness = 0.06F;
            if (side == ModuleCollapseSide::Left) {
                handle.left = expanded.right - handleThickness;
            } else if (side == ModuleCollapseSide::Right) {
                handle.right = expanded.left + handleThickness;
            } else if (side == ModuleCollapseSide::Top) {
                handle.top = expanded.bottom - handleThickness;
            } else {
                handle.bottom = expanded.top + handleThickness;
            }
            if (expanded.left < 0.0F || expanded.top < 0.0F ||
                expanded.right > 1.0F || expanded.bottom > 1.0F ||
                HasCollapseExpansionConflict(source, expanded, targetRoot)) {
                return false;
            }
        }
        if (expanded.right - expanded.left < 0.10F || expanded.bottom - expanded.top < 0.10F ||
            handle.right - handle.left < 0.001F || handle.bottom - handle.top < 0.001F) return false;

        auto* item = Find(source);
        if (!item) return false;
        // The collapsed handle is a visual child of the target, not a normal snapped
        // member. Keeping it in the target snap group causes a later drag to move a
        // hidden handle with the target and makes the arrow impossible to pick up.
        snapGroup[static_cast<std::size_t>(item - items.data())] = source;
        SetCollapsedGeometry(*item, handle, expanded, mode, side, targetRoot, false);
        if (mode == ModuleCollapseMode::Inside) {
            SetTabGroupGeometry(targetRoot, collapsedTargetBounds);
        }
        if (HasNewConflictingGeometry(before)) {
            *this = before;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ToggleCollapsedModule(
        ModuleId id, ModuleExpansionBehavior behavior = ModuleExpansionBehavior::Squash,
        float canvasWidth = 0.0F, float canvasHeight = 0.0F,
        float* resizedWidth = nullptr, float* resizedHeight = nullptr) noexcept {
        auto* item = Find(id);
        if (!item || item->collapseMode == ModuleCollapseMode::None) return false;
        const ModuleLayout before = *this;
        if (!item->collapsed) {
            if (item->handleWidth < 0.001F || item->handleHeight < 0.001F) return false;
            if (item->collapseMode == ModuleCollapseMode::Inside &&
                !item->collapseTargetIsWindow) {
                const ModuleId targetRoot = TabRoot(item->collapseTarget);
                const auto* target = Find(targetRoot);
                if (!target || !target->visible || target->collapsed) return false;
                const auto expanded = Bounds(*item);
                const auto targetBounds = Bounds(*target);
                const ModuleNormalizedRect shared{
                    std::min(expanded.left, targetBounds.left),
                    std::min(expanded.top, targetBounds.top),
                    std::max(expanded.right, targetBounds.right),
                    std::max(expanded.bottom, targetBounds.bottom)};
                const bool horizontal = IsHorizontalCollapseSide(item->collapseSide);
                const float thickness = horizontal ? item->handleWidth : item->handleHeight;
                const float length = horizontal ? item->handleHeight : item->handleWidth;
                const auto handle = MakeInsideCollapseHandle(shared, item->collapseSide,
                                                               thickness, length);
                auto collapsedTargetBounds = shared;
                switch (item->collapseSide) {
                case ModuleCollapseSide::Left: collapsedTargetBounds.left = handle.right; break;
                case ModuleCollapseSide::Right: collapsedTargetBounds.right = handle.left; break;
                case ModuleCollapseSide::Top: collapsedTargetBounds.top = handle.bottom; break;
                case ModuleCollapseSide::Bottom: collapsedTargetBounds.bottom = handle.top; break;
                case ModuleCollapseSide::None: return false;
                }
                if (collapsedTargetBounds.right - collapsedTargetBounds.left < 0.10F ||
                    collapsedTargetBounds.bottom - collapsedTargetBounds.top < 0.10F) {
                    return false;
                }
                SetCollapsedGeometry(*item, handle, expanded, item->collapseMode,
                                     item->collapseSide, targetRoot, false);
                SetTabGroupGeometry(targetRoot, collapsedTargetBounds);
            } else {
                item->x = item->handleX;
                item->y = item->handleY;
                item->width = item->handleWidth;
                item->height = item->handleHeight;
                item->collapsed = true;
            }
            return true;
        }
        if (item->expandedWidth < 0.10F || item->expandedHeight < 0.10F) {
            return false;
        }
        const ModuleNormalizedRect expanded{item->expandedX, item->expandedY,
                                             item->expandedX + item->expandedWidth,
                                             item->expandedY + item->expandedHeight};
        if (item->collapseMode == ModuleCollapseMode::Inside &&
            !item->collapseTargetIsWindow) {
            const ModuleId targetRoot = TabRoot(item->collapseTarget);
            const auto* target = Find(targetRoot);
            if (!target || !target->visible || target->collapsed) return false;
            const auto targetBounds = Bounds(*target);
            const ModuleNormalizedRect shared{
                std::min(expanded.left, targetBounds.left),
                std::min(expanded.top, targetBounds.top),
                std::max(expanded.right, targetBounds.right),
                std::max(expanded.bottom, targetBounds.bottom)};
            ModuleNormalizedRect expandedTargetBounds = shared;
            const bool horizontal = IsHorizontalCollapseSide(item->collapseSide);
            const float handleThickness = horizontal ? item->handleWidth : item->handleHeight;
            switch (item->collapseSide) {
            case ModuleCollapseSide::Left:
                expandedTargetBounds.left = expanded.right + handleThickness;
                break;
            case ModuleCollapseSide::Right:
                expandedTargetBounds.right = expanded.left - handleThickness;
                break;
            case ModuleCollapseSide::Top:
                expandedTargetBounds.top = expanded.bottom + handleThickness;
                break;
            case ModuleCollapseSide::Bottom:
                expandedTargetBounds.bottom = expanded.top - handleThickness;
                break;
            case ModuleCollapseSide::None: return false;
            }
            if (expandedTargetBounds.right - expandedTargetBounds.left < 0.10F ||
                expandedTargetBounds.bottom - expandedTargetBounds.top < 0.10F) {
                return false;
            }
            item->x = expanded.left;
            item->y = expanded.top;
            item->width = expanded.right - expanded.left;
            item->height = expanded.bottom - expanded.top;
            item->collapsed = false;
            SetTabGroupGeometry(targetRoot, expandedTargetBounds);
        } else {
            item->x = expanded.left;
            item->y = expanded.top;
            item->width = expanded.right - expanded.left;
            item->height = expanded.bottom - expanded.top;
            item->collapsed = false;
        }
        const bool introducesConflict = HasNewConflictingGeometry(before);
        const bool outsideCanvas = expanded.left < 0.0F || expanded.top < 0.0F ||
            expanded.right > 1.0F || expanded.bottom > 1.0F;
        if (introducesConflict && !outsideCanvas &&
            behavior == ModuleExpansionBehavior::Squash &&
            SquashForExpansion(id, expanded)) {
            return true;
        }
        if ((introducesConflict && behavior == ModuleExpansionBehavior::Resize) || outsideCanvas) {
            if (resizedWidth != nullptr && resizedHeight != nullptr &&
                ResizeForExpansion(id, expanded, canvasWidth, canvasHeight,
                                   *resizedWidth, *resizedHeight)) {
                return true;
            }
        }
        if (!introducesConflict && !outsideCanvas) return true;
        *this = before;
        return false;
    }

    [[nodiscard]] ModuleId SnapRoot(ModuleId id) const noexcept {
        for (std::size_t index = 0; index < items.size(); ++index) {
            if (items[index].id == id) {
                const auto candidate = snapGroup[index];
                return Find(candidate) != nullptr ? candidate : id;
            }
        }
        return id;
    }

    [[nodiscard]] bool HasValidGeometry() const noexcept {
        for (const auto& item : items) {
            if (!std::isfinite(item.x) || !std::isfinite(item.y) ||
                !std::isfinite(item.width) || !std::isfinite(item.height) ||
                item.width <= 0.0F || item.height <= 0.0F ||
                item.x < 0.0F || item.y < 0.0F ||
                item.x + item.width > 1.0F || item.y + item.height > 1.0F) {
                return false;
            }
            if (!item.collapsed && (item.width < 0.10F || item.height < 0.10F)) {
                return false;
            }
            if (item.collapsed) {
                if (!std::isfinite(item.expandedX) || !std::isfinite(item.expandedY) ||
                    !std::isfinite(item.expandedWidth) || !std::isfinite(item.expandedHeight) ||
                    item.expandedWidth < 0.10F || item.expandedHeight < 0.10F ||
                    item.expandedX < 0.0F || item.expandedY < 0.0F ||
                    item.expandedX + item.expandedWidth > 1.0F ||
                    item.expandedY + item.expandedHeight > 1.0F) {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] std::size_t TabCount() const noexcept {
        return std::min(tabCount, tabOrder.size());
    }

    [[nodiscard]] std::size_t ActiveTabIndex() const noexcept {
        const auto count = TabCount();
        return count == 0 ? 0 : std::min(activeTab, count - 1);
    }

    [[nodiscard]] static ModuleNormalizedRect Bounds(const ModuleLayoutItem& item) noexcept {
        return {item.x, item.y, item.x + item.width, item.y + item.height};
    }

    [[nodiscard]] static bool Intersects(const ModuleNormalizedRect& first,
                                         const ModuleNormalizedRect& second) noexcept {
        // Touching edges are valid adjacent snap boundaries; only a positive-area
        // intersection is an overlap.
        return first.left < second.right && first.right > second.left &&
               first.top < second.bottom && first.bottom > second.top;
    }

    [[nodiscard]] static bool Contains(const std::array<ModuleId, 5>& ids,
                                       std::size_t count, ModuleId id) noexcept {
        for (std::size_t index = 0; index < count; ++index) {
            if (ids[index] == id) return true;
        }
        return false;
    }

    // Collect the visual object moved by a module drag.  A snapped root carries its
    // complete snap group, while a tabbed module carries all tabs because they share
    // one visible rectangle.  The result is also used as the set excluded from
    // occupancy checks, so a module can be moved out of the space it currently owns.
    [[nodiscard]] std::size_t MovingMembers(ModuleId id,
                                             std::array<ModuleId, 5>& members) const noexcept {
        std::size_t count = 0;
        const auto append = [&](ModuleId candidate) {
            if (Contains(members, count, candidate) || count >= members.size()) return;
            members[count++] = candidate;
        };
        const ModuleId tabRoot = TabRoot(id);
        const ModuleId snapRoot = SnapRoot(tabRoot);
        for (const auto& item : items) {
            if (!item.visible) continue;
            if (SnapRoot(item.id) == snapRoot) append(item.id);
            if (IsTabbed(id) && IsTabbed(item.id) && TabRoot(item.id) == tabRoot) {
                append(item.id);
            }
        }
        append(id);
        return count;
    }

    [[nodiscard]] bool HasGeometryConflict(ModuleId firstId, ModuleId secondId) const noexcept {
        const auto* first = Find(firstId);
        const auto* second = Find(secondId);
        if (!first || !second || !first->visible || !second->visible) return false;
        // Tabs intentionally occupy the same rectangle.
        if (IsTabbed(first->id) && IsTabbed(second->id) &&
            TabRoot(first->id) == TabRoot(second->id)) {
            return false;
        }
        if (IsInsideCollapseOverlap(*first, *second) ||
            IsInsideCollapseOverlap(*second, *first)) {
            return false;
        }
        if (first->collapsed || second->collapsed) return false;
        return Intersects(Bounds(*first), Bounds(*second));
    }

    [[nodiscard]] bool HasConflictingGeometry() const noexcept {
        for (std::size_t first = 0; first < items.size(); ++first) {
            for (std::size_t second = first + 1; second < items.size(); ++second) {
                if (HasGeometryConflict(items[first].id, items[second].id)) return true;
            }
        }
        return false;
    }

    // A persisted layout can contain a historical overlap (for example after an older
    // version changed the available module set).  Docking must still be able to repair
    // or rearrange that layout.  Reject a gesture only when it introduces a conflict
    // that did not exist before the gesture; retaining an unrelated pre-existing
    // conflict is preferable to disabling every side/window/collapse drop.  Tab merges
    // already use the same pairwise geometry exception above.
    [[nodiscard]] bool HasNewConflictingGeometry(const ModuleLayout& before) const noexcept {
        for (std::size_t first = 0; first < items.size(); ++first) {
            for (std::size_t second = first + 1; second < items.size(); ++second) {
                const ModuleId firstId = items[first].id;
                const ModuleId secondId = items[second].id;
                if (HasGeometryConflict(firstId, secondId) &&
                    !before.HasGeometryConflict(firstId, secondId)) {
                    return true;
                }
            }
        }
        return false;
    }

    [[nodiscard]] bool FindplusWindowRectangle(ModuleId source, ModuleNormalizedRect region,
                                               float pointerX, float pointerY,
                                               float minimumWidth, float minimumHeight,
                                               ModuleNormalizedRect& result,
                                               std::optional<ModuleCollapseSide> attachedSide = {},
                                               bool attachWindowEdge = false) const noexcept {
        if (!(region.right > region.left) || !(region.bottom > region.top) ||
            !(minimumWidth > 0.0F) || !(minimumHeight > 0.0F)) {
            return false;
        }

        std::array<ModuleId, 5> moving{};
        const auto movingCount = MovingMembers(source, moving);
        std::array<ModuleNormalizedRect, 5> obstacles{};
        std::size_t obstacleCount = 0;
        for (const auto& item : items) {
            if (!item.visible || Contains(moving, movingCount, item.id)) continue;
            // A tab group is one visual obstacle, not one obstacle per tab.  The root
            // is the rectangle used by rendering and by all tab operations.
            if (IsTabbed(item.id) && TabRoot(item.id) != item.id) continue;
            const auto bounds = Bounds(item);
            if (bounds.right <= region.left || bounds.left >= region.right ||
                bounds.bottom <= region.top || bounds.top >= region.bottom) {
                continue;
            }
            if (obstacleCount < obstacles.size()) obstacles[obstacleCount++] = bounds;
        }

        // Every maximal free rectangle has edges on either the requested region or
        // an obstacle edge.  With four modules, enumerating those coordinates is
        // small, deterministic, and handles resized/non-grid-aligned modules without
        // introducing a second layout tree.
        std::array<float, 12> xCoordinates{};
        std::array<float, 12> yCoordinates{};
        std::size_t xCount = 0;
        std::size_t yCount = 0;
        const auto appendCoordinate = [](auto& coordinates, std::size_t& count, float value) {
            if (count >= coordinates.size()) return;
            for (std::size_t index = 0; index < count; ++index) {
                if (std::abs(coordinates[index] - value) < 0.00001F) return;
            }
            coordinates[count++] = value;
        };
        appendCoordinate(xCoordinates, xCount, region.left);
        appendCoordinate(xCoordinates, xCount, region.right);
        appendCoordinate(yCoordinates, yCount, region.top);
        appendCoordinate(yCoordinates, yCount, region.bottom);
        for (std::size_t index = 0; index < obstacleCount; ++index) {
            appendCoordinate(xCoordinates, xCount,
                             std::clamp(obstacles[index].left, region.left, region.right));
            appendCoordinate(xCoordinates, xCount,
                             std::clamp(obstacles[index].right, region.left, region.right));
            appendCoordinate(yCoordinates, yCount,
                             std::clamp(obstacles[index].top, region.top, region.bottom));
            appendCoordinate(yCoordinates, yCount,
                             std::clamp(obstacles[index].bottom, region.top, region.bottom));
        }
        std::sort(xCoordinates.begin(), xCoordinates.begin() + static_cast<std::ptrdiff_t>(xCount));
        std::sort(yCoordinates.begin(), yCoordinates.begin() + static_cast<std::ptrdiff_t>(yCount));

        bool found = false;
        float bestScore = -std::numeric_limits<float>::infinity();
        for (std::size_t leftIndex = 0; leftIndex + 1 < xCount; ++leftIndex) {
            for (std::size_t rightIndex = leftIndex + 1; rightIndex < xCount; ++rightIndex) {
                for (std::size_t topIndex = 0; topIndex + 1 < yCount; ++topIndex) {
                    for (std::size_t bottomIndex = topIndex + 1; bottomIndex < yCount; ++bottomIndex) {
                        const ModuleNormalizedRect candidate{
                            xCoordinates[leftIndex], yCoordinates[topIndex],
                            xCoordinates[rightIndex], yCoordinates[bottomIndex]};
                        if (candidate.right - candidate.left < minimumWidth ||
                            candidate.bottom - candidate.top < minimumHeight) {
                            continue;
                        }
                        if (attachedSide) {
                            constexpr float edgeTolerance = 0.00001F;
                            const bool attached = attachWindowEdge
                                ? (*attachedSide == ModuleCollapseSide::Left
                                    ? std::abs(candidate.left - region.left) < edgeTolerance
                                    : *attachedSide == ModuleCollapseSide::Right
                                        ? std::abs(candidate.right - region.right) < edgeTolerance
                                        : *attachedSide == ModuleCollapseSide::Top
                                            ? std::abs(candidate.top - region.top) < edgeTolerance
                                            : *attachedSide == ModuleCollapseSide::Bottom &&
                                              std::abs(candidate.bottom - region.bottom) < edgeTolerance)
                                : (*attachedSide == ModuleCollapseSide::Left
                                    ? std::abs(candidate.right - region.right) < edgeTolerance
                                    : *attachedSide == ModuleCollapseSide::Right
                                        ? std::abs(candidate.left - region.left) < edgeTolerance
                                        : *attachedSide == ModuleCollapseSide::Top
                                            ? std::abs(candidate.bottom - region.bottom) < edgeTolerance
                                            : *attachedSide == ModuleCollapseSide::Bottom &&
                                              std::abs(candidate.top - region.top) < edgeTolerance);
                            if (!attached) continue;
                        }
                        bool blocked = false;
                        for (std::size_t obstacle = 0; obstacle < obstacleCount; ++obstacle) {
                            if (Intersects(candidate, obstacles[obstacle])) {
                                blocked = true;
                                break;
                            }
                        }
                        if (blocked) continue;

                        const float candidateWidth = candidate.right - candidate.left;
                        const float candidateHeight = candidate.bottom - candidate.top;
                        const float area = candidateWidth * candidateHeight;
                        const bool containsPointer = pointerX >= candidate.left &&
                            pointerX <= candidate.right && pointerY >= candidate.top &&
                            pointerY <= candidate.bottom;
                        const float distanceX = pointerX < candidate.left
                            ? candidate.left - pointerX
                            : pointerX > candidate.right ? pointerX - candidate.right : 0.0F;
                        const float distanceY = pointerY < candidate.top
                            ? candidate.top - pointerY
                            : pointerY > candidate.bottom ? pointerY - candidate.bottom : 0.0F;
                        // Prefer the free rectangle under the cursor.  If the cursor is
                        // over an occupied part, prefer the closest largest remainder.
                        const float score = (containsPointer ? 1000.0F : 0.0F) +
                                            area - (distanceX * distanceX + distanceY * distanceY);
                        if (!found || score > bestScore) {
                            found = true;
                            bestScore = score;
                            result = candidate;
                        }
                    }
                }
            }
        }
        return found;
    }

    [[nodiscard]] bool IsSnapGrouped(ModuleId id) const noexcept {
        const auto root = SnapRoot(id);
        std::size_t count = 0;
        for (const auto& item : items) {
            if (item.visible && SnapRoot(item.id) == root) ++count;
        }
        return count > 1;
    }

    // Remove one member from its snap group.  If the group owner is removed, the
    // first remaining member becomes the new owner.  A one-member remainder is
    // floating because there is no longer a snap relationship to preserve.
    void DetachSnapModule(ModuleId id) noexcept {
        if (!IsSnapGrouped(id)) return;
        const auto oldRoot = SnapRoot(id);
        std::array<ModuleId, 5> remaining{};
        std::size_t remainingCount = 0;
        for (const auto& item : items) {
            if (item.visible && item.id != id && SnapRoot(item.id) == oldRoot) {
                remaining[remainingCount++] = item.id;
            }
        }

        const auto newRoot = oldRoot == id && remainingCount > 0 ? remaining[0] : oldRoot;
        for (auto& item : items) {
            const auto index = static_cast<std::size_t>(&item - items.data());
            if (item.id == id) {
                snapGroup[index] = id;
                item.dockState = ModuleDockState::Floating;
            } else if (SnapRoot(item.id) == oldRoot) {
                snapGroup[index] = newRoot;
                item.dockState = remainingCount > 1 ? ModuleDockState::Snapped
                                                     : ModuleDockState::Floating;
            }
        }
    }

    // Resize a snapped group as one layout object.  The pointer delta is measured
    // from the selected member's edge, while the complete group's bounding box is
    // scaled so every member keeps its relative position and participates in the
    // resize instead of being left at its old dimensions.
    void ResizeSnapGroup(ModuleId id, float pointerX, float pointerY,
                         bool resizeRight, bool resizeBottom,
                         bool resizeLeft, bool resizeTop) noexcept {
        if (!IsSnapGrouped(id)) return;
        const auto root = SnapRoot(id);
        const auto* selected = Find(id);
        if (!selected) return;

        float groupLeft = 1.0F;
        float groupTop = 1.0F;
        float groupRight = 0.0F;
        float groupBottom = 0.0F;
        float minimumScaleX = 0.0F;
        float minimumScaleY = 0.0F;
        for (const auto& item : items) {
            if (!item.visible || SnapRoot(item.id) != root) continue;
            groupLeft = std::min(groupLeft, item.x);
            groupTop = std::min(groupTop, item.y);
            groupRight = std::max(groupRight, item.x + item.width);
            groupBottom = std::max(groupBottom, item.y + item.height);
            minimumScaleX = std::max(minimumScaleX, 0.10F / item.width);
            minimumScaleY = std::max(minimumScaleY, 0.10F / item.height);
        }
        for (const auto& item : items) {
            if (!item.visible || !item.collapsed ||
                item.collapseMode != ModuleCollapseMode::Inside ||
                item.collapseTargetIsWindow ||
                SnapRoot(TabRoot(item.collapseTarget)) != root) {
                continue;
            }
            if (!(item.expandedWidth > 0.0F) || !(item.expandedHeight > 0.0F)) continue;
            minimumScaleX = std::max(minimumScaleX, 0.10F / item.expandedWidth);
            minimumScaleY = std::max(minimumScaleY, 0.10F / item.expandedHeight);
        }
        const float groupWidth = groupRight - groupLeft;
        const float groupHeight = groupBottom - groupTop;
        if (!(groupWidth > 0.0F) || !(groupHeight > 0.0F)) return;

        float newLeft = groupLeft;
        float newTop = groupTop;
        float newRight = groupRight;
        float newBottom = groupBottom;
        if (resizeRight) {
            newRight += pointerX - (selected->x + selected->width);
        }
        if (resizeLeft) {
            newLeft += pointerX - selected->x;
        }
        if (resizeBottom) {
            newBottom += pointerY - (selected->y + selected->height);
        }
        if (resizeTop) {
            newTop += pointerY - selected->y;
        }

        const float minimumWidth = groupWidth * minimumScaleX;
        const float minimumHeight = groupHeight * minimumScaleY;
        if (newRight - newLeft < minimumWidth) {
            if (resizeLeft && !resizeRight) newLeft = newRight - minimumWidth;
            else newRight = newLeft + minimumWidth;
        }
        if (newBottom - newTop < minimumHeight) {
            if (resizeTop && !resizeBottom) newTop = newBottom - minimumHeight;
            else newBottom = newTop + minimumHeight;
        }

        if (resizeLeft) {
            const float shift = std::clamp(newLeft, 0.0F, 1.0F - minimumWidth) - newLeft;
            newLeft += shift;
            newRight += shift;
        } else if (resizeRight) {
            newRight = std::clamp(newRight, minimumWidth, 1.0F);
            newLeft = std::min(newLeft, newRight - minimumWidth);
        }
        if (resizeTop) {
            const float shift = std::clamp(newTop, 0.0F, 1.0F - minimumHeight) - newTop;
            newTop += shift;
            newBottom += shift;
        } else if (resizeBottom) {
            newBottom = std::clamp(newBottom, minimumHeight, 1.0F);
            newTop = std::min(newTop, newBottom - minimumHeight);
        }

        // Existing independent modules are occupied space. A group resize stops at
        // the nearest occupied boundary rather than growing through a resized panel.
        for (const auto& item : items) {
            if (!item.visible || SnapRoot(item.id) == root) continue;
            const auto obstacle = Bounds(item);
            if (resizeRight && obstacle.left >= groupRight &&
                newTop < obstacle.bottom && newBottom > obstacle.top) {
                newRight = std::min(newRight, obstacle.left);
            }
            if (resizeLeft && obstacle.right <= groupLeft &&
                newTop < obstacle.bottom && newBottom > obstacle.top) {
                newLeft = std::max(newLeft, obstacle.right);
            }
            if (resizeBottom && obstacle.top >= groupBottom &&
                newLeft < obstacle.right && newRight > obstacle.left) {
                newBottom = std::min(newBottom, obstacle.top);
            }
            if (resizeTop && obstacle.bottom <= groupTop &&
                newLeft < obstacle.right && newRight > obstacle.left) {
                newTop = std::max(newTop, obstacle.bottom);
            }
        }
        if (newRight - newLeft < minimumWidth || newBottom - newTop < minimumHeight) return;

        const float scaleX = (newRight - newLeft) / groupWidth;
        const float scaleY = (newBottom - newTop) / groupHeight;
        for (auto& item : items) {
            if (!item.visible || SnapRoot(item.id) != root) continue;
            item.x = newLeft + (item.x - groupLeft) * scaleX;
            item.y = newTop + (item.y - groupTop) * scaleY;
            item.width *= scaleX;
            item.height *= scaleY;
            SyncExpandedGeometry(item);
        }
        ScaleCollapsedInsideModules(root, {groupLeft, groupTop, groupRight, groupBottom},
                                    {newLeft, newTop, newRight, newBottom});
    }

    // Preserve module pixel sizes while a client resize consumes only unused canvas.
    // Once an axis can no longer contain all visible modules, leave normalized geometry
    // unchanged so that axis scales with the window rather than clipping a panel.
    [[nodiscard]] bool PreservePixelGeometry(float oldWidth, float oldHeight,
                                              float newWidth, float newHeight) noexcept {
        if (!(oldWidth > 0.0F) || !(oldHeight > 0.0F) ||
            !(newWidth > 0.0F) || !(newHeight > 0.0F)) {
            return false;
        }

        const auto preserveAxis = [this](float oldExtent, float newExtent, bool horizontal) {
            if (std::abs(oldExtent - newExtent) < 0.01F) return false;
            float minimum = std::numeric_limits<float>::infinity();
            float maximum = -std::numeric_limits<float>::infinity();
            for (const auto& item : items) {
                if (!item.visible) continue;
                const float position = (horizontal ? item.x : item.y) * oldExtent;
                const float length = (horizontal ? item.width : item.height) * oldExtent;
                minimum = std::min(minimum, position);
                maximum = std::max(maximum, position + length);
            }
            if (!std::isfinite(minimum) || !std::isfinite(maximum) ||
                maximum - minimum > newExtent) {
                return false;
            }

            const float shift = std::clamp(0.0F, -minimum, newExtent - maximum);
            const auto transform = [oldExtent, newExtent, shift](float& position,
                                                                   float& length) {
                position = (position * oldExtent + shift) / newExtent;
                length = length * oldExtent / newExtent;
            };
            for (auto& item : items) {
                if (!item.visible) continue;
                if (horizontal) {
                    transform(item.x, item.width);
                    if (item.collapseMode != ModuleCollapseMode::None) {
                        transform(item.handleX, item.handleWidth);
                    }
                    if (item.collapsed) {
                        transform(item.expandedX, item.expandedWidth);
                    }
                } else {
                    transform(item.y, item.height);
                    if (item.collapseMode != ModuleCollapseMode::None) {
                        transform(item.handleY, item.handleHeight);
                    }
                    if (item.collapsed) {
                        transform(item.expandedY, item.expandedHeight);
                    }
                }
                SyncExpandedGeometry(item);
            }
            return true;
        };

        const bool changedX = preserveAxis(oldWidth, newWidth, true);
        const bool changedY = preserveAxis(oldHeight, newHeight, false);
        return changedX || changedY;
    }

    // A tabbed module is rendered in the rectangle of the first tab.  Keeping this
    // lookup in the layout model avoids accidentally using a tab's stale, hidden
    // rectangle while it is being dragged or resized.
    [[nodiscard]] ModuleId TabRoot(ModuleId id) const noexcept {
        if (!IsTabbed(id)) return id;
        return Find(tabOrder[0]) != nullptr ? tabOrder[0] : id;
    }

    void ClearTabs() noexcept {
        tabCount = 0;
        activeTab = 0;
    }

    void MakeTab(ModuleId first, ModuleId second) noexcept {
        ClearTabs();
        tabOrder[tabCount++] = first;
        if (first != second) tabOrder[tabCount++] = second;
        activeTab = 0;
    }

    void RemoveTab(ModuleId id) noexcept {
        if (!IsTabbed(id)) return;
        ModuleLayoutItem groupGeometry{};
        if (const auto* root = Find(tabOrder[0])) groupGeometry = *root;
        std::array<ModuleId, 5> remaining{};
        std::size_t remainingCount = 0;
        const auto tabCountValue = TabCount();
        for (std::size_t i = 0; i < tabCountValue; ++i) {
            if (tabOrder[i] != id) remaining[remainingCount++] = tabOrder[i];
        }
        for (std::size_t i = 0; i < remainingCount; ++i) {
            if (auto* item = Find(remaining[i])) {
                item->x = groupGeometry.x;
                item->y = groupGeometry.y;
                item->width = groupGeometry.width;
                item->height = groupGeometry.height;
            }
        }
        if (auto* removed = Find(id)) removed->dockState = ModuleDockState::Floating;
        if (remainingCount < 2) {
            ClearTabs();
            return;
        }
        tabOrder = remaining;
        tabCount = remainingCount;
        activeTab = std::min(activeTab, tabCount == 0 ? 0U : tabCount - 1U);
    }

    void TabWith(ModuleId source, ModuleId target) noexcept {
        if (source == target) return;
        if (IsTabbed(source) && IsTabbed(target) && TabRoot(source) == TabRoot(target)) return;
        const ModuleId geometryId = TabRoot(target);
        ModuleLayoutItem groupGeometry{};
        if (const auto* root = Find(geometryId)) groupGeometry = *root;
        std::array<ModuleId, 5> group{};
        std::size_t count = 0;
        const auto append = [&](ModuleId id) {
            for (std::size_t i = 0; i < count; ++i) {
                if (group[i] == id) return true;
            }
            if (count >= group.size()) return false;
            group[count++] = id;
            return true;
        };
        if (IsTabbed(target)) {
            // Keep the target group's order and root.  Its visible rectangle is
            // the geometry used by the merged group.
            const auto targetTabCount = TabCount();
            for (std::size_t i = 0; i < targetTabCount; ++i) {
                if (!append(tabOrder[i])) return;
            }
        } else if (!append(target)) {
            return;
        }
        if (IsTabbed(source)) {
            // A focused merged module carries its other tabs with it.  Append the
            // source group without duplicating the focused source tab.
            const auto sourceTabCount = TabCount();
            for (std::size_t i = 0; i < sourceTabCount; ++i) {
                if (!append(tabOrder[i])) return;
            }
        } else if (!append(source)) {
            return;
        }
        if (count < 2) {
            ClearTabs();
            return;
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (auto* item = Find(group[i])) {
                item->x = groupGeometry.x;
                item->y = groupGeometry.y;
                item->width = groupGeometry.width;
                item->height = groupGeometry.height;
                item->dockState = Find(geometryId)
                    ? Find(geometryId)->dockState : ModuleDockState::Floating;
            }
        }
        tabOrder = group;
        tabCount = count;
        for (std::size_t i = 0; i < count; ++i) {
            if (group[i] == source) {
                activeTab = i;
                break;
            }
        }
    }

    // Split only the visible target rectangle in half and place the source in the
    // requested half.  Existing members of the target's snap group remain where
    // they are.  This is deliberately geometry-based: it gives side snapping a
    // stable persisted result without introducing a second layout tree.
    [[nodiscard]] bool SnapTo(ModuleId source, ModuleId target, ModuleDropZone zone) noexcept {
        if (source == target || !IsSideDrop(zone)) return false;
        const ModuleLayout before = *this;
        if (IsTabbed(source) && IsTabbed(target) && TabRoot(source) == TabRoot(target)) {
            return false;
        }
        const ModuleId sourceTabRoot = TabRoot(source);
        const ModuleId targetRoot = TabRoot(target);
        const bool targetWasTabbed = IsTabbed(target);
        const ModuleId sourceSnapRoot = SnapRoot(sourceTabRoot);
        const ModuleId targetSnapRoot = SnapRoot(targetRoot);
        if (sourceSnapRoot == targetSnapRoot) return false;
        const auto* targetItem = Find(targetRoot);
        if (!targetItem || targetItem->collapsed || !Find(sourceTabRoot)) return false;
        std::array<ModuleId, 5> sourceMembers{};
        const std::size_t sourceMemberCount = MovingMembers(source, sourceMembers);
        const ModuleLayoutItem targetGeometry = *targetItem;
        if ((zone == ModuleDropZone::Left || zone == ModuleDropZone::Right) &&
            targetGeometry.width < 0.20F) return false;
        if ((zone == ModuleDropZone::Top || zone == ModuleDropZone::Bottom) &&
            targetGeometry.height < 0.20F) return false;

        const float middleX = targetGeometry.x + targetGeometry.width * 0.5F;
        const float middleY = targetGeometry.y + targetGeometry.height * 0.5F;
        const ModuleNormalizedRect sourceBounds = [this, &sourceMembers, sourceMemberCount] {
            ModuleNormalizedRect result{1.0F, 1.0F, 0.0F, 0.0F};
            for (std::size_t index = 0; index < sourceMemberCount; ++index) {
                if (const auto* item = Find(sourceMembers[index])) {
                    result.left = std::min(result.left, item->x);
                    result.top = std::min(result.top, item->y);
                    result.right = std::max(result.right, item->x + item->width);
                    result.bottom = std::max(result.bottom, item->y + item->height);
                }
            }
            return result;
        }();
        const auto setGeometry = [](ModuleLayoutItem& item, ModuleNormalizedRect bounds,
                                    ModuleDockState state) {
            item.x = bounds.left;
            item.y = bounds.top;
            item.width = bounds.right - bounds.left;
            item.height = bounds.bottom - bounds.top;
            item.dockState = state;
        };
        const ModuleNormalizedRect sourceDestination =
            zone == ModuleDropZone::Left
                ? ModuleNormalizedRect{targetGeometry.x, targetGeometry.y, middleX,
                                       targetGeometry.y + targetGeometry.height}
            : zone == ModuleDropZone::Right
                ? ModuleNormalizedRect{middleX, targetGeometry.y,
                                       targetGeometry.x + targetGeometry.width,
                                       targetGeometry.y + targetGeometry.height}
            : zone == ModuleDropZone::Top
                ? ModuleNormalizedRect{targetGeometry.x, targetGeometry.y,
                                       targetGeometry.x + targetGeometry.width, middleY}
                : ModuleNormalizedRect{targetGeometry.x, middleY,
                                       targetGeometry.x + targetGeometry.width,
                                       targetGeometry.y + targetGeometry.height};
        const ModuleNormalizedRect targetDestination =
            zone == ModuleDropZone::Left
                ? ModuleNormalizedRect{middleX, targetGeometry.y,
                                       targetGeometry.x + targetGeometry.width,
                                       targetGeometry.y + targetGeometry.height}
            : zone == ModuleDropZone::Right
                ? ModuleNormalizedRect{targetGeometry.x, targetGeometry.y, middleX,
                                       targetGeometry.y + targetGeometry.height}
            : zone == ModuleDropZone::Top
                ? ModuleNormalizedRect{targetGeometry.x, middleY,
                                       targetGeometry.x + targetGeometry.width,
                                       targetGeometry.y + targetGeometry.height}
                : ModuleNormalizedRect{targetGeometry.x, targetGeometry.y,
                                       targetGeometry.x + targetGeometry.width, middleY};

        if (sourceDestination.right - sourceDestination.left < 0.10F ||
            sourceDestination.bottom - sourceDestination.top < 0.10F) return false;

        // A tabbed source is one visual object.  A snapped source group retains its
        // internal partition, scaled into the new half so the group does not collapse
        // into four overlapping copies of one rectangle.
        const float sourceWidth = sourceBounds.right - sourceBounds.left;
        const float sourceHeight = sourceBounds.bottom - sourceBounds.top;
        if (!(sourceWidth > 0.0F) || !(sourceHeight > 0.0F)) return false;
        const float scaleX = (sourceDestination.right - sourceDestination.left) / sourceWidth;
        const float scaleY = (sourceDestination.bottom - sourceDestination.top) / sourceHeight;
        for (std::size_t index = 0; index < sourceMemberCount; ++index) {
            if (auto* item = Find(sourceMembers[index])) {
                const ModuleNormalizedRect transformed = sourceMemberCount == 1
                    ? sourceDestination
                    : ModuleNormalizedRect{
                        sourceDestination.left + (item->x - sourceBounds.left) * scaleX,
                        sourceDestination.top + (item->y - sourceBounds.top) * scaleY,
                        sourceDestination.left + (item->x + item->width - sourceBounds.left) * scaleX,
                        sourceDestination.top + (item->y + item->height - sourceBounds.top) * scaleY};
                setGeometry(*item, transformed, ModuleDockState::Snapped);
            }
        }

        // Only the target visual object changes.  Other members of an existing snap
        // group may occupy adjacent space (for example the previous right half) and
        // must not be overwritten by a later drop.
        if (auto* item = Find(targetRoot)) setGeometry(*item, targetDestination,
                                                       ModuleDockState::Snapped);
        if (targetWasTabbed) {
            for (std::size_t index = 0; index < TabCount(); ++index) {
                if (auto* item = Find(tabOrder[index])) {
                    setGeometry(*item, targetDestination, ModuleDockState::Snapped);
                }
            }
        }

        // Merge the source snap group into the target snap group.  This is the
        // persistent relationship that makes a side-snapped pair move as one.
        for (std::size_t i = 0; i < sourceMemberCount; ++i) {
            if (auto* item = Find(sourceMembers[i])) {
                const auto itemIndex = static_cast<std::size_t>(item - items.data());
                snapGroup[itemIndex] = targetSnapRoot;
                item->dockState = ModuleDockState::Snapped;
            }
        }
        // A failed plan must not leave a partially applied layout behind.  This is
        // especially important while the pointer is moving because preview planning
        // is intentionally speculative.
        if (HasNewConflictingGeometry(before)) {
            *this = before;
            return false;
        }
        return true;
    }

    // Snap a module to one of the seven client-window targets.  The requested region
    // is treated as an available area, not as permission to overwrite existing
    // modules.  If a resized module occupies part of the region, the largest free
    // rectangle containing (or nearest to) the pointer is selected instead.
    [[nodiscard]] bool SnapToWindow(ModuleId source, ModuleWindowDropZone zone,
                                    float pointerX, float pointerY) noexcept {
        if (!IsWindowDrop(zone)) return false;
        const auto region = ModuleWindowDropBounds(zone);
        ModuleNormalizedRect destination{};
        const ModuleLayout before = *this;
        const bool freeDestination = FindplusWindowRectangle(
            source, region, pointerX, pointerY, 0.10F, 0.10F, destination);

        std::array<ModuleId, 5> moving{};
        const auto movingCount = MovingMembers(source, moving);
        ModuleId targetRoot{};
        bool splitTargetFound = false;
        if (!freeDestination) {
            // If the requested region is occupied, split the module under the
            // pointer instead of replacing it. This is what makes a subsequent
            // right-middle snap consume only the remaining right-side space.
            const ModuleLayoutItem* target = nullptr;
            for (const auto& item : items) {
                if (!item.visible || item.collapsed || Contains(moving, movingCount, item.id)) continue;
                if (IsTabbed(item.id) && TabRoot(item.id) != item.id) continue;
                const auto bounds = Bounds(item);
                if (pointerX >= bounds.left && pointerX <= bounds.right &&
                    pointerY >= bounds.top && pointerY <= bounds.bottom) {
                    if (!target || item.width * item.height > target->width * target->height) {
                        target = &item;
                    }
                }
            }
            if (!target) return false;
            targetRoot = TabRoot(target->id);
            splitTargetFound = true;
            const auto targetGeometry = Bounds(*Find(targetRoot));
            const bool horizontal = zone == ModuleWindowDropZone::RightTop ||
                                    zone == ModuleWindowDropZone::RightBottom ||
                                    zone == ModuleWindowDropZone::LeftTop ||
                                    zone == ModuleWindowDropZone::LeftBottom;
            if (horizontal) {
                const float middle = targetGeometry.top +
                                     (targetGeometry.bottom - targetGeometry.top) * 0.5F;
                if (zone == ModuleWindowDropZone::RightTop ||
                    zone == ModuleWindowDropZone::LeftTop) {
                    destination = {targetGeometry.left, targetGeometry.top,
                                   targetGeometry.right, middle};
                } else {
                    destination = {targetGeometry.left, middle,
                                   targetGeometry.right, targetGeometry.bottom};
                }
            } else {
                const float middle = targetGeometry.left +
                                     (targetGeometry.right - targetGeometry.left) * 0.5F;
                if (zone == ModuleWindowDropZone::RightMiddle) {
                    destination = {middle, targetGeometry.top,
                                   targetGeometry.right, targetGeometry.bottom};
                } else if (zone == ModuleWindowDropZone::LeftMiddle) {
                    destination = {targetGeometry.left, targetGeometry.top,
                                   middle, targetGeometry.bottom};
                } else {
                    return false;
                }
            }
            if (destination.right - destination.left < 0.10F ||
                destination.bottom - destination.top < 0.10F) return false;
        }

        ModuleNormalizedRect sourceBounds{1.0F, 1.0F, 0.0F, 0.0F};
        for (std::size_t index = 0; index < movingCount; ++index) {
            if (const auto* item = Find(moving[index])) {
                sourceBounds.left = std::min(sourceBounds.left, item->x);
                sourceBounds.top = std::min(sourceBounds.top, item->y);
                sourceBounds.right = std::max(sourceBounds.right, item->x + item->width);
                sourceBounds.bottom = std::max(sourceBounds.bottom, item->y + item->height);
            }
        }
        const float sourceWidth = sourceBounds.right - sourceBounds.left;
        const float sourceHeight = sourceBounds.bottom - sourceBounds.top;
        if (!(sourceWidth > 0.0F) || !(sourceHeight > 0.0F)) return false;
        const float scaleX = (destination.right - destination.left) / sourceWidth;
        const float scaleY = (destination.bottom - destination.top) / sourceHeight;
        for (std::size_t index = 0; index < movingCount; ++index) {
            if (auto* item = Find(moving[index])) {
                // A window snap places the dragged module into the visible layout. This
                // also makes it an obstacle for subsequent nested snaps in the same area.
                item->visible = true;
                if (movingCount == 1) {
                    item->x = destination.left;
                    item->y = destination.top;
                    item->width = destination.right - destination.left;
                    item->height = destination.bottom - destination.top;
                } else {
                    item->x = destination.left + (item->x - sourceBounds.left) * scaleX;
                    item->y = destination.top + (item->y - sourceBounds.top) * scaleY;
                    item->width *= scaleX;
                    item->height *= scaleY;
                }
                item->dockState = ModuleDockState::Snapped;
            }
        }
        if (!freeDestination && splitTargetFound) {
            if (auto* target = Find(targetRoot)) {
                const auto targetBounds = Bounds(*target);
                // Retain the other half of the occupied target. The destination
                // is the newly claimed half and the target becomes its remainder.
                const bool horizontal = destination.left == targetBounds.left &&
                                        destination.right == targetBounds.right;
                ModuleNormalizedRect remainder = targetBounds;
                if (horizontal) {
                    if (destination.top <= targetBounds.top) remainder.top = destination.bottom;
                    else remainder.bottom = destination.top;
                } else {
                    if (destination.left <= targetBounds.left) remainder.left = destination.right;
                    else remainder.right = destination.left;
                }
                if (remainder.right - remainder.left < 0.10F ||
                    remainder.bottom - remainder.top < 0.10F) {
                    *this = before;
                    return false;
                }
                target->x = remainder.left;
                target->y = remainder.top;
                target->width = remainder.right - remainder.left;
                target->height = remainder.bottom - remainder.top;
                target->dockState = ModuleDockState::Snapped;
                for (std::size_t tab = 0; tab < TabCount(); ++tab) {
                    if (auto* tabItem = Find(tabOrder[tab])) {
                        if (TabRoot(tabItem->id) == targetRoot) {
                            tabItem->x = target->x;
                            tabItem->y = target->y;
                            tabItem->width = target->width;
                            tabItem->height = target->height;
                        }
                    }
                }
                for (std::size_t index = 0; index < movingCount; ++index) {
                    if (auto* moved = Find(moving[index])) {
                        snapGroup[static_cast<std::size_t>(moved - items.data())] = targetRoot;
                    }
                }
            }
        }
        if (HasNewConflictingGeometry(before)) {
            *this = before;
            return false;
        }
        return true;
    }
};

class UiModule final {
public:
    constexpr UiModule(ModuleId id, std::string_view key, std::wstring_view title) noexcept
        : id_(id), key_(key), title_(title) {}

    [[nodiscard]] constexpr ModuleId Id() const noexcept { return id_; }
    // ASCII key intended for future persisted layouts and module lookup.
    [[nodiscard]] constexpr std::string_view Key() const noexcept { return key_; }
    // Human-readable default title used by the current panel renderer.
    [[nodiscard]] constexpr std::wstring_view Title() const noexcept { return title_; }

private:
    ModuleId id_{};
    std::string_view key_{};
    std::wstring_view title_{};
};

// The initial module catalog is deliberately static. It provides identity and metadata
// only; section layout, visibility, replacement, and tab behaviour will be layered on top
// of these stable entries in a later change.
class UiModuleRegistry final {
public:
    [[nodiscard]] static constexpr std::span<const UiModule> Modules() noexcept {
        return kModules;
    }

    [[nodiscard]] static constexpr const UiModule* Find(ModuleId id) noexcept {
        for (const auto& module : kModules) {
            if (module.Id() == id) return &module;
        }
        return nullptr;
    }

    // All ids used by the built-in layout are guaranteed to be present. Returning the
    // first entry for an invalid id keeps rendering code noexcept and defensive.
    [[nodiscard]] static constexpr const UiModule& Get(ModuleId id) noexcept {
        if (const auto* module = Find(id)) return *module;
        return kModules.front();
    }

private:
    inline static constexpr std::array kModules{
         UiModule{ModuleId::Rivan, "rivan", L"PLAYER"},
        UiModule{ModuleId::AllMusic, "all_music", L"ALL MUSIC"},
        UiModule{ModuleId::GraphicEqualizer, "graphic_equalizer", L"GRAPHIC EQUALIZER"},
        UiModule{ModuleId::RivanLibrary, "rivan_library", L"RIVAN LIBRARY"},
        UiModule{ModuleId::VideoPreview, "video_preview", L"VIDEO PREVIEW"},
    };
};

static_assert(UiModuleRegistry::Modules().size() == 5,
              "The initial main-window module catalog must contain five sections.");

} // namespace rivan::ui
