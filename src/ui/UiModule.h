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
// exposes a small handle while collapsed.  Inside drops expand over the target module;
// outside drops expand in the adjacent free space.
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
        return first.collapseMode == ModuleCollapseMode::Inside &&
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

    // Collapse a module to a narrow handle on a client-window edge. Its stored geometry
    // is the rectangle revealed when the handle is expanded again.
    [[nodiscard]] bool CollapseToWindow(ModuleId source, ModuleCollapseSide side) noexcept {
        if (!CanCollapseSource(source) || !IsCollapseSide(side)) return false;
        const auto* sourceItem = Find(source);
        if (!sourceItem) return false;
        const ModuleLayout before = *this;
        const float width = sourceItem->width;
        const float height = sourceItem->height;
        if (width < 0.10F || height < 0.10F) return false;

        ModuleNormalizedRect expanded{sourceItem->x, sourceItem->y,
                                      sourceItem->x + width, sourceItem->y + height};
        ModuleNormalizedRect handle = expanded;
        if (IsHorizontalCollapseSide(side)) {
            constexpr float handleWidth = 0.06F;
            const float handleHeight = std::clamp(height * 0.22F, 0.08F, 0.18F);
            expanded.left = side == ModuleCollapseSide::Left ? 0.0F : 1.0F - width;
            expanded.right = expanded.left + width;
            handle.left = side == ModuleCollapseSide::Left ? 0.0F : 1.0F - handleWidth;
            handle.right = handle.left + handleWidth;
            const float center = expanded.top + height * 0.5F;
            handle.top = std::clamp(center - handleHeight * 0.5F, 0.0F, 1.0F - handleHeight);
            handle.bottom = handle.top + handleHeight;
        } else {
            constexpr float handleHeight = 0.06F;
            const float handleWidth = std::clamp(width * 0.22F, 0.08F, 0.18F);
            expanded.top = side == ModuleCollapseSide::Top ? 0.0F : 1.0F - height;
            expanded.bottom = expanded.top + height;
            handle.top = side == ModuleCollapseSide::Top ? 0.0F : 1.0F - handleHeight;
            handle.bottom = handle.top + handleHeight;
            const float center = expanded.left + width * 0.5F;
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
        if (HasConflictingGeometry()) {
            *this = before;
            return false;
        }
        return true;
    }

    // Collapse a module against a module side. Inside drops reveal the source over the
    // corresponding half of the target. Outside drops reserve the source's current size
    // immediately beside the target, with no overlap with unrelated modules.
    [[nodiscard]] bool CollapseToModule(ModuleId source, ModuleId target,
                                        ModuleCollapseSide side,
                                        ModuleCollapseMode mode) noexcept {
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
            const float handleWidth = IsHorizontalCollapseSide(side) ? 0.06F : 0.16F;
            const float handleHeight = IsHorizontalCollapseSide(side) ? 0.16F : 0.06F;
            const float centerX = (targetBounds.left + targetBounds.right) * 0.5F;
            const float centerY = (targetBounds.top + targetBounds.bottom) * 0.5F;
            if (side == ModuleCollapseSide::Left) {
                handle = {targetBounds.left, centerY - handleHeight * 0.5F,
                          targetBounds.left + handleWidth, centerY + handleHeight * 0.5F};
            } else if (side == ModuleCollapseSide::Right) {
                handle = {targetBounds.right - handleWidth, centerY - handleHeight * 0.5F,
                          targetBounds.right, centerY + handleHeight * 0.5F};
            } else if (side == ModuleCollapseSide::Top) {
                handle = {centerX - handleWidth * 0.5F, targetBounds.top,
                          centerX + handleWidth * 0.5F, targetBounds.top + handleHeight};
            } else {
                handle = {centerX - handleWidth * 0.5F, targetBounds.bottom - handleHeight,
                          centerX + handleWidth * 0.5F, targetBounds.bottom};
            }
            if (HasCollapseExpansionConflict(source, handle, targetRoot)) return false;
        } else {
            const float width = sourceItem->width;
            const float height = sourceItem->height;
            if (width < 0.10F || height < 0.10F) return false;
            const float centerX = (targetBounds.left + targetBounds.right) * 0.5F;
            const float centerY = (targetBounds.top + targetBounds.bottom) * 0.5F;
            if (side == ModuleCollapseSide::Left) {
                expanded = {targetBounds.left - width, centerY - height * 0.5F,
                            targetBounds.left, centerY + height * 0.5F};
            } else if (side == ModuleCollapseSide::Right) {
                expanded = {targetBounds.right, centerY - height * 0.5F,
                            targetBounds.right + width, centerY + height * 0.5F};
            } else if (side == ModuleCollapseSide::Top) {
                expanded = {centerX - width * 0.5F, targetBounds.top - height,
                            centerX + width * 0.5F, targetBounds.top};
            } else {
                expanded = {centerX - width * 0.5F, targetBounds.bottom,
                            centerX + width * 0.5F, targetBounds.bottom + height};
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
            handle.right - handle.left < 0.04F || handle.bottom - handle.top < 0.04F) return false;

        auto* item = Find(source);
        if (!item) return false;
        // The collapsed handle is a visual child of the target, not a normal snapped
        // member. Keeping it in the target snap group causes a later drag to move a
        // hidden handle with the target and makes the arrow impossible to pick up.
        snapGroup[static_cast<std::size_t>(item - items.data())] = source;
        SetCollapsedGeometry(*item, handle, expanded, mode, side, targetRoot, false);
        if (HasConflictingGeometry()) {
            *this = before;
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ToggleCollapsedModule(ModuleId id) noexcept {
        auto* item = Find(id);
        if (!item || item->collapseMode == ModuleCollapseMode::None) return false;
        const ModuleLayout before = *this;
        if (!item->collapsed) {
            if (item->handleWidth < 0.04F || item->handleHeight < 0.04F) return false;
            item->x = item->handleX;
            item->y = item->handleY;
            item->width = item->handleWidth;
            item->height = item->handleHeight;
            item->collapsed = true;
            return true;
        }
        if (item->expandedWidth < 0.10F || item->expandedHeight < 0.10F) {
            return false;
        }
        item->x = item->expandedX;
        item->y = item->expandedY;
        item->width = item->expandedWidth;
        item->height = item->expandedHeight;
        item->collapsed = false;
        if (HasConflictingGeometry()) {
            *this = before;
            return false;
        }
        return true;
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

    [[nodiscard]] bool HasConflictingGeometry() const noexcept {
        for (std::size_t first = 0; first < items.size(); ++first) {
            if (!items[first].visible) continue;
            for (std::size_t second = first + 1; second < items.size(); ++second) {
                if (!items[second].visible) continue;
                // Tabs intentionally occupy the same rectangle.
                if (IsTabbed(items[first].id) && IsTabbed(items[second].id) &&
                    TabRoot(items[first].id) == TabRoot(items[second].id)) {
                    continue;
                }
                if (IsInsideCollapseOverlap(items[first], items[second]) ||
                    IsInsideCollapseOverlap(items[second], items[first])) {
                    continue;
                }
                if (items[first].collapsed || items[second].collapsed) continue;
                if (Intersects(Bounds(items[first]), Bounds(items[second]))) return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool FindplusWindowRectangle(ModuleId source, ModuleNormalizedRect region,
                                               float pointerX, float pointerY,
                                               float minimumWidth, float minimumHeight,
                                               ModuleNormalizedRect& result) const noexcept {
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
        if (HasConflictingGeometry()) {
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
        if (HasConflictingGeometry()) {
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
        UiModule{ModuleId::Rivan, "rivan", L"RIVAN"},
        UiModule{ModuleId::AllMusic, "all_music", L"ALL MUSIC"},
        UiModule{ModuleId::GraphicEqualizer, "graphic_equalizer", L"GRAPHIC EQUALIZER"},
        UiModule{ModuleId::RivanLibrary, "rivan_library", L"RIVAN LIBRARY"},
        UiModule{ModuleId::VideoPreview, "video_preview", L"VIDEO PREVIEW"},
    };
};

static_assert(UiModuleRegistry::Modules().size() == 5,
              "The initial main-window module catalog must contain five sections.");

} // namespace rivan::ui
