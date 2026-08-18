// ModuleLayoutCollapse.cpp
// Collapse, expansion, squash, and resize behavior.
#include "ModuleLayout.h"
#include "ModuleLayoutInternal.h"

#include <algorithm>
#include <cmath>

namespace rivan::ui {

namespace {

ModuleNormalizedRect AttachOutsideExpanded(ModuleNormalizedRect expanded,
                                           ModuleNormalizedRect targetBounds,
                                           ModuleCollapseSide side) noexcept {
    switch (side) {
    case ModuleCollapseSide::Left:
        {
        const float width = expanded.right - expanded.left;
        expanded.right = targetBounds.left;
        expanded.left = expanded.right - width;
        }
        break;
    case ModuleCollapseSide::Right:
        {
        const float width = expanded.right - expanded.left;
        expanded.left = targetBounds.right;
        expanded.right = expanded.left + width;
        }
        break;
    case ModuleCollapseSide::Top:
        {
        const float height = expanded.bottom - expanded.top;
        expanded.bottom = targetBounds.top;
        expanded.top = expanded.bottom - height;
        }
        break;
    case ModuleCollapseSide::Bottom:
        {
        const float height = expanded.bottom - expanded.top;
        expanded.top = targetBounds.bottom;
        expanded.bottom = expanded.top + height;
        }
        break;
    case ModuleCollapseSide::None:
        break;
    }
    return expanded;
}

ModuleNormalizedRect MakeOutsideCollapseHandle(ModuleNormalizedRect expanded,
                                                 ModuleCollapseSide side,
                                                 float thickness) noexcept {
    switch (side) {
    case ModuleCollapseSide::Left:
        expanded.left = expanded.right - thickness;
        break;
    case ModuleCollapseSide::Right:
        expanded.right = expanded.left + thickness;
        break;
    case ModuleCollapseSide::Top:
        expanded.top = expanded.bottom - thickness;
        break;
    case ModuleCollapseSide::Bottom:
        expanded.bottom = expanded.top + thickness;
        break;
    case ModuleCollapseSide::None:
        break;
    }
    return expanded;
}

void SquashResizeObstacles(ModuleLayout& layout, ModuleId resizedId,
                           const ModuleNormalizedRect& oldBounds,
                           ModuleNormalizedRect& newBounds,
                           bool resizeRight, bool resizeBottom,
                           bool resizeLeft, bool resizeTop) noexcept {
    std::array<ModuleId, 6> moving{};
    const auto movingCount = layout.MovingMembers(resizedId, moving);
    for (const auto& item : layout.items) {
        if (!item.visible || ModuleLayout::Contains(moving, movingCount, item.id) ||
            (layout.IsTabbed(item.id) && layout.TabRoot(item.id) != item.id) ||
            (item.collapsed && item.collapseMode == ModuleCollapseMode::Outside &&
             !item.collapseTargetIsWindow &&
             layout.SnapRoot(layout.TabRoot(item.collapseTarget)) ==
                 layout.SnapRoot(layout.TabRoot(resizedId)))) {
            continue;
        }

        const auto obstacle = ModuleLayout::Bounds(item);
        if (resizeRight && obstacle.left >= oldBounds.right &&
            newBounds.top < obstacle.bottom && newBounds.bottom > obstacle.top) {
            newBounds.right = std::min(newBounds.right, obstacle.right - kMinimumModuleExtent);
        }
        if (resizeLeft && obstacle.right <= oldBounds.left &&
            newBounds.top < obstacle.bottom && newBounds.bottom > obstacle.top) {
            newBounds.left = std::max(newBounds.left, obstacle.left + kMinimumModuleExtent);
        }
        if (resizeBottom && obstacle.top >= oldBounds.bottom &&
            newBounds.left < obstacle.right && newBounds.right > obstacle.left) {
            newBounds.bottom = std::min(newBounds.bottom, obstacle.bottom - kMinimumModuleExtent);
        }
        if (resizeTop && obstacle.bottom <= oldBounds.top &&
            newBounds.left < obstacle.right && newBounds.right > obstacle.left) {
            newBounds.top = std::max(newBounds.top, obstacle.top + kMinimumModuleExtent);
        }
    }

    for (const auto& item : layout.items) {
        if (!item.visible || ModuleLayout::Contains(moving, movingCount, item.id) ||
            (layout.IsTabbed(item.id) && layout.TabRoot(item.id) != item.id) ||
            (item.collapsed && item.collapseMode == ModuleCollapseMode::Outside &&
             !item.collapseTargetIsWindow &&
             layout.SnapRoot(layout.TabRoot(item.collapseTarget)) ==
                 layout.SnapRoot(layout.TabRoot(resizedId)))) {
            continue;
        }
        const auto obstacle = ModuleLayout::Bounds(item);
        ModuleNormalizedRect adjusted = obstacle;
        bool changed = false;
        if (resizeRight && obstacle.left >= oldBounds.right && obstacle.left < newBounds.right &&
            newBounds.top < obstacle.bottom && newBounds.bottom > obstacle.top) {
            adjusted.left = newBounds.right;
            changed = true;
        } else if (resizeLeft && obstacle.right <= oldBounds.left && obstacle.right > newBounds.left &&
                   newBounds.top < obstacle.bottom && newBounds.bottom > obstacle.top) {
            adjusted.right = newBounds.left;
            changed = true;
        } else if (resizeBottom && obstacle.top >= oldBounds.bottom && obstacle.top < newBounds.bottom &&
                   newBounds.left < obstacle.right && newBounds.right > obstacle.left) {
            adjusted.top = newBounds.bottom;
            changed = true;
        } else if (resizeTop && obstacle.bottom <= oldBounds.top && obstacle.bottom > newBounds.top &&
                   newBounds.left < obstacle.right && newBounds.right > obstacle.left) {
            adjusted.bottom = newBounds.top;
            changed = true;
        }
        if (!changed || adjusted.right - adjusted.left < kMinimumModuleExtent ||
            adjusted.bottom - adjusted.top < kMinimumModuleExtent) continue;
        layout.SetTabGroupGeometry(layout.TabRoot(item.id), adjusted);
    }
}

} // namespace

bool ModuleLayout::IsCollapsed(ModuleId id) const noexcept {
    const auto* item = Find(id);
    return item != nullptr && item->collapsed && item->collapseMode != ModuleCollapseMode::None;
}

bool ModuleLayout::IsEffectivelyCollapsed(ModuleId id) const noexcept {
    const auto* item = Find(id);
    if (!item) return false;

    std::array<ModuleId, 6> visited{};
    std::size_t visitedCount = 0;
    ModuleId current = id;
    while (const auto* currentItem = Find(current)) {
        std::array<ModuleId, 6> members{};
        const auto memberCount = MovingMembers(current, members);
        for (std::size_t index = 0; index < memberCount; ++index) {
            const auto* member = Find(members[index]);
            if (member != nullptr && member->collapsed &&
                member->collapseMode != ModuleCollapseMode::None) {
                return true;
            }
        }
        if (currentItem->collapseMode != ModuleCollapseMode::Inside ||
            currentItem->collapseTargetIsWindow) {
            break;
        }
        current = TabRoot(currentItem->collapseTarget);
        if (current == id || Contains(visited, visitedCount, current)) return true;
        if (visitedCount < visited.size()) visited[visitedCount++] = current;
    }
    return false;
}

bool ModuleLayout::IsCollapseHandleVisible(ModuleId id) const noexcept {
    const auto* item = Find(id);
    if (!item || item->collapseMode == ModuleCollapseMode::None) return false;

    std::array<ModuleId, 6> members{};
    const auto memberCount = MovingMembers(id, members);
    for (std::size_t index = 0; index < memberCount; ++index) {
        if (members[index] == id) continue;
        const auto* member = Find(members[index]);
        if (member != nullptr && member->collapsed &&
            member->collapseMode != ModuleCollapseMode::None) {
            return false;
        }
    }

    if (item->collapseMode == ModuleCollapseMode::Inside &&
        !item->collapseTargetIsWindow &&
        TabRoot(item->collapseTarget) != id &&
        IsEffectivelyCollapsed(TabRoot(item->collapseTarget))) return false;
    return true;
}

void ModuleLayout::ClearModuleCollapse(ModuleId id) noexcept {
    if (auto* item = Find(id)) {
        if (item->collapsed) {
            const bool hasExpandedGeometry = HasUsableExpandedGeometry(*item);
            if (hasExpandedGeometry) {
                item->x = item->expandedX;
                item->y = item->expandedY;
                item->width = item->expandedWidth;
                item->height = item->expandedHeight;
            } else {
                item->width = std::clamp(item->width, 0.10F, 1.0F);
                item->height = std::clamp(item->height, 0.10F, 1.0F);
                item->x = std::clamp(item->x, 0.0F, 1.0F - item->width);
                item->y = std::clamp(item->y, 0.0F, 1.0F - item->height);
            }
        }
        item->collapseMode = ModuleCollapseMode::None;
        item->collapseSide = ModuleCollapseSide::None;
        item->collapseTarget = id;
        item->collapseTargetIsWindow = false;
        item->collapsed = false;
    }
}

void ModuleLayout::ClearInsideCollapseReferences(ModuleId target) noexcept {
    for (const auto& candidate : items) {
        if (candidate.id != target && candidate.collapseTarget == target &&
            candidate.collapseMode == ModuleCollapseMode::Inside) {
            ClearModuleCollapse(candidate.id);
        }
    }
}

bool ModuleLayout::IsCollapseSide(ModuleCollapseSide side) noexcept {
    return side != ModuleCollapseSide::None;
}

bool ModuleLayout::IsHorizontalCollapseSide(ModuleCollapseSide side) noexcept {
    return side == ModuleCollapseSide::Left || side == ModuleCollapseSide::Right;
}

bool ModuleLayout::IsInsideCollapseOverlap(const ModuleLayoutItem& first,
                                            const ModuleLayoutItem& second) noexcept {
    return first.collapsed && first.collapseMode == ModuleCollapseMode::Inside &&
           !first.collapseTargetIsWindow && first.collapseTarget == second.id;
}

bool ModuleLayout::CanCollapseSource(ModuleId source) const noexcept {
    const auto* item = Find(source);
    return item != nullptr && item->visible && !IsTabbed(source) &&
           (!IsSnapped(source) || !IsSnapGrouped(source));
}

bool ModuleLayout::HasCollapseExpansionConflict(
    ModuleId source, const ModuleNormalizedRect& destination,
    std::optional<ModuleId> allowedTarget) const noexcept {
    for (const auto& item : items) {
        if (!item.visible || item.id == source || item.collapsed) continue;
        if (allowedTarget && item.id == *allowedTarget) continue;
        if (allowedTarget && IsTabbed(item.id) && TabRoot(item.id) == *allowedTarget) continue;
        if (Intersects(destination, Bounds(item))) return true;
    }
    return false;
}

void ModuleLayout::SetCollapsedGeometry(ModuleLayoutItem& item, ModuleNormalizedRect handle,
                                        ModuleNormalizedRect expanded, ModuleCollapseMode mode,
                                        ModuleCollapseSide side, ModuleId target,
                                        bool targetIsWindow) noexcept {
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

ModuleNormalizedRect ModuleLayout::MakeInsideCollapseHandle(
    ModuleNormalizedRect bounds, ModuleCollapseSide side, float thickness, float length) noexcept {
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

ModuleNormalizedRect ModuleLayout::ScaleBounds(ModuleNormalizedRect value,
                                                const ModuleNormalizedRect& oldBounds,
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

void ModuleLayout::ScaleCollapsedInsideModules(ModuleId targetRoot,
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

void ModuleLayout::ScaleCollapsedInsideTabGroup(
    ModuleId targetRoot, const ModuleNormalizedRect& oldBounds,
    const ModuleNormalizedRect& newBounds) noexcept {
    for (auto& candidate : items) {
        if (!candidate.visible || !candidate.collapsed ||
            candidate.collapseMode != ModuleCollapseMode::Inside ||
            candidate.collapseTargetIsWindow ||
            TabRoot(candidate.collapseTarget) != targetRoot) {
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

bool ModuleLayout::ReattachOutsideCollapseHandles(const ModuleLayout& before) noexcept {
    ModuleLayout candidate = *this;
    const auto fail = [this, &before]() noexcept {
        *this = before;
        return false;
    };
    const auto mapCenter = [](float center, float oldStart, float oldEnd,
                              float newStart, float newEnd) noexcept {
        const float oldLength = oldEnd - oldStart;
        if (!(oldLength > 0.0F)) return (newStart + newEnd) * 0.5F;
        return newStart + (center - oldStart) / oldLength * (newEnd - newStart);
    };
    const auto setAttachedAxis = [](ModuleNormalizedRect& expanded,
                                    ModuleNormalizedRect& handle,
                                    ModuleCollapseSide side,
                                    const ModuleNormalizedRect& targetBounds) noexcept {
        switch (side) {
        case ModuleCollapseSide::Left:
            {
            const float length = expanded.right - expanded.left;
            expanded.right = targetBounds.left;
            expanded.left = expanded.right - length;
            }
            {
            const float length = handle.right - handle.left;
            handle.right = targetBounds.left;
            handle.left = handle.right - length;
            }
            return true;
        case ModuleCollapseSide::Right:
            {
            const float length = expanded.right - expanded.left;
            expanded.left = targetBounds.right;
            expanded.right = expanded.left + length;
            }
            {
            const float length = handle.right - handle.left;
            handle.left = targetBounds.right;
            handle.right = handle.left + length;
            }
            return true;
        case ModuleCollapseSide::Top:
            {
            const float length = expanded.bottom - expanded.top;
            expanded.bottom = targetBounds.top;
            expanded.top = expanded.bottom - length;
            }
            {
            const float length = handle.bottom - handle.top;
            handle.bottom = targetBounds.top;
            handle.top = handle.bottom - length;
            }
            return true;
        case ModuleCollapseSide::Bottom:
            {
            const float length = expanded.bottom - expanded.top;
            expanded.top = targetBounds.bottom;
            expanded.bottom = expanded.top + length;
            }
            {
            const float length = handle.bottom - handle.top;
            handle.top = targetBounds.bottom;
            handle.bottom = handle.top + length;
            }
            return true;
        case ModuleCollapseSide::None:
            return false;
        }
        return false;
    };
    const auto reserveHandleStrip = [&candidate](ModuleId targetRoot,
                                                  ModuleCollapseSide side,
                                                  const ModuleNormalizedRect& handle) noexcept {
        auto* target = candidate.Find(targetRoot);
        if (!target) return false;

        auto bounds = Bounds(*target);
        if (!HasUsableTargetRectangle(bounds) || !HasFiniteOrderedRectangle(handle)) {
            return false;
        }
        const float strip = IsHorizontalCollapseSide(side)
            ? handle.right - handle.left : handle.bottom - handle.top;
        if (!(strip > 0.001F) || !std::isfinite(strip)) return false;

        const auto apply = [&candidate, targetRoot](ModuleNormalizedRect adjusted) noexcept {
            if (!HasUsableTargetRectangle(adjusted)) return false;
            candidate.SetTabGroupGeometry(targetRoot, adjusted);
            return true;
        };
        switch (side) {
        case ModuleCollapseSide::Left:
            if (bounds.left >= strip) return true;
            if (bounds.right - strip >= kMinimumModuleExtent) {
                bounds.left = strip;
                return apply(bounds);
            }
            return false;
        case ModuleCollapseSide::Right:
            if (bounds.right <= 1.0F - strip) return true;
            if (1.0F - strip - bounds.left >= kMinimumModuleExtent) {
                bounds.right = 1.0F - strip;
                return apply(bounds);
            }
            return false;
        case ModuleCollapseSide::Top:
            if (bounds.top >= strip) return true;
            if (bounds.bottom - strip >= kMinimumModuleExtent) {
                bounds.top = strip;
                return apply(bounds);
            }
            return false;
        case ModuleCollapseSide::Bottom:
            if (bounds.bottom <= 1.0F - strip) return true;
            if (1.0F - strip - bounds.top >= kMinimumModuleExtent) {
                bounds.bottom = 1.0F - strip;
                return apply(bounds);
            }
            return false;
        case ModuleCollapseSide::None:
            return false;
        }
        return false;
    };

    for (auto& item : candidate.items) {
        if (!item.visible || item.collapseMode != ModuleCollapseMode::Outside ||
            item.collapseTargetIsWindow || !item.collapsed) {
            continue;
        }
        const ModuleId targetRoot = candidate.TabRoot(item.collapseTarget);
        const auto* oldTarget = before.Find(targetRoot);
        const auto* target = candidate.Find(targetRoot);
        if (!oldTarget || !target || !target->visible || target->collapsed) return fail();

        ModuleNormalizedRect expanded{item.expandedX, item.expandedY,
                                      item.expandedX + item.expandedWidth,
                                      item.expandedY + item.expandedHeight};
        ModuleNormalizedRect handle{item.handleX, item.handleY,
                                    item.handleX + item.handleWidth,
                                    item.handleY + item.handleHeight};
        const auto oldTargetBounds = Bounds(*oldTarget);
        const auto currentTargetBounds = Bounds(*target);
        if (!IsCollapseSide(item.collapseSide) || !HasUsableExpandedRectangle(expanded) ||
            !HasFiniteOrderedRectangle(handle) || !HasUsableTargetRectangle(oldTargetBounds) ||
            !HasUsableTargetRectangle(currentTargetBounds)) {
            return fail();
        }

        // The handle belongs outside this target, not to the client edge.
        // Reserve its full strip before reattaching so edge clamping cannot cover the target.
        if (!reserveHandleStrip(targetRoot, item.collapseSide, handle)) return fail();

        const auto* attachedTarget = candidate.Find(targetRoot);
        if (!attachedTarget) return fail();
        const auto targetBounds = Bounds(*attachedTarget);
        if (!HasUsableTargetRectangle(targetBounds)) return fail();
        if (IsHorizontalCollapseSide(item.collapseSide)) {
            const float expandedCenter = (expanded.top + expanded.bottom) * 0.5F;
            const float expandedLength = expanded.bottom - expanded.top;
            const float handleLength = handle.bottom - handle.top;
            if (!std::isfinite(expandedCenter) || !std::isfinite(expandedLength) ||
                !std::isfinite(handleLength) || !(expandedLength > 0.0F) ||
                !(handleLength > 0.001F) || handleLength > 1.0F) {
                return fail();
            }
            const float expandedMappedCenter = mapCenter(
                expandedCenter, oldTargetBounds.top, oldTargetBounds.bottom,
                targetBounds.top, targetBounds.bottom);
            if (!std::isfinite(expandedMappedCenter)) return fail();
            const float clampedCenter = expandedLength <= 1.0F
                ? std::clamp(expandedMappedCenter, expandedLength * 0.5F,
                             1.0F - expandedLength * 0.5F)
                : expandedMappedCenter;
            if (!std::isfinite(clampedCenter)) return fail();
            expanded.top = clampedCenter - expandedLength * 0.5F;
            expanded.bottom = expanded.top + expandedLength;
            handle.top = clampedCenter - handleLength * 0.5F;
            handle.bottom = handle.top + handleLength;
        } else {
            const float expandedCenter = (expanded.left + expanded.right) * 0.5F;
            const float expandedLength = expanded.right - expanded.left;
            const float handleLength = handle.right - handle.left;
            if (!std::isfinite(expandedCenter) || !std::isfinite(expandedLength) ||
                !std::isfinite(handleLength) || !(expandedLength > 0.0F) ||
                !(handleLength > 0.001F) || handleLength > 1.0F) {
                return fail();
            }
            const float expandedMappedCenter = mapCenter(
                expandedCenter, oldTargetBounds.left, oldTargetBounds.right,
                targetBounds.left, targetBounds.right);
            if (!std::isfinite(expandedMappedCenter)) return fail();
            const float clampedCenter = expandedLength <= 1.0F
                ? std::clamp(expandedMappedCenter, expandedLength * 0.5F,
                             1.0F - expandedLength * 0.5F)
                : expandedMappedCenter;
            if (!std::isfinite(clampedCenter)) return fail();
            expanded.left = clampedCenter - expandedLength * 0.5F;
            expanded.right = expanded.left + expandedLength;
            handle.left = clampedCenter - handleLength * 0.5F;
            handle.right = handle.left + handleLength;
        }
        if (!setAttachedAxis(expanded, handle, item.collapseSide, targetBounds)) return fail();

        const float attachedSpace = item.collapseSide == ModuleCollapseSide::Left
            ? targetBounds.left
            : item.collapseSide == ModuleCollapseSide::Right
                ? 1.0F - targetBounds.right
                : item.collapseSide == ModuleCollapseSide::Top
                    ? targetBounds.top : 1.0F - targetBounds.bottom;
        const float attachedLength = IsHorizontalCollapseSide(item.collapseSide)
            ? handle.right - handle.left : handle.bottom - handle.top;
        if (!HasUsableExpandedRectangle(expanded) || !HasFiniteOrderedRectangle(handle) ||
            !std::isfinite(attachedSpace) || !std::isfinite(attachedLength) ||
            !(attachedSpace > 0.001F) || !(attachedLength > 0.001F)) {
            return fail();
        }
        if (item.collapseSide == ModuleCollapseSide::Left && handle.left < 0.0F) {
            handle.left = 0.0F;
            handle.right = std::min(handle.right, attachedSpace);
        } else if (item.collapseSide == ModuleCollapseSide::Right && handle.right > 1.0F) {
            handle.right = 1.0F;
            handle.left = std::max(handle.left, targetBounds.right);
        } else if (item.collapseSide == ModuleCollapseSide::Top && handle.top < 0.0F) {
            handle.top = 0.0F;
            handle.bottom = std::min(handle.bottom, attachedSpace);
        } else if (item.collapseSide == ModuleCollapseSide::Bottom && handle.bottom > 1.0F) {
            handle.bottom = 1.0F;
            handle.top = std::max(handle.top, targetBounds.bottom);
        }
        if (IsHorizontalCollapseSide(item.collapseSide)) {
            const float length = handle.bottom - handle.top;
            if (!std::isfinite(length) || !(length > 0.001F) || length > 1.0F) {
                return fail();
            }
            handle.top = std::clamp(handle.top, 0.0F, 1.0F - length);
            handle.bottom = handle.top + length;
        } else {
            const float length = handle.right - handle.left;
            if (!std::isfinite(length) || !(length > 0.001F) || length > 1.0F) {
                return fail();
            }
            handle.left = std::clamp(handle.left, 0.0F, 1.0F - length);
            handle.right = handle.left + length;
        }
        if (!HasUsableExpandedRectangle(expanded) || !HasUsableHandleRectangle(handle)) {
            return fail();
        }

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
        item.width = item.handleWidth;
        item.height = item.handleHeight;
    }
    *this = candidate;
    return true;
}

bool ModuleLayout::SquashForExpansion(ModuleId source, ModuleNormalizedRect expanded) noexcept {
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
        const auto sideCandidate = [this, &consider, &expanded, source,
                                    obstacleId = obstacle.id](ModuleNormalizedRect bounds) {
            if (bounds.right - bounds.left < 0.10F ||
                bounds.bottom - bounds.top < 0.10F || Intersects(bounds, expanded)) {
                return;
            }
            for (const auto& candidate : items) {
                if (!candidate.visible || candidate.collapsed || candidate.id == source ||
                    candidate.id == obstacleId) {
                    continue;
                }
                if (Intersects(bounds, Bounds(candidate))) return;
            }
            consider(bounds);
        };
        const float rightWidth = std::min(currentWidth, 1.0F - expanded.right);
        sideCandidate({expanded.right, current.top,
                       expanded.right + rightWidth, current.bottom});
        const float leftWidth = std::min(currentWidth, expanded.left);
        sideCandidate({expanded.left - leftWidth, current.top, expanded.left, current.bottom});
        const float bottomHeight = std::min(currentHeight, 1.0F - expanded.bottom);
        sideCandidate({current.left, expanded.bottom,
                       current.right, expanded.bottom + bottomHeight});
        const float topHeight = std::min(currentHeight, expanded.top);
        sideCandidate({current.left, expanded.top - topHeight, current.right, expanded.top});
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
    if (!ReattachOutsideCollapseHandles(before)) {
        *this = before;
        return false;
    }
    if (HasNewConflictingGeometry(before)) {
        *this = before;
        return false;
    }
    return true;
}

bool ModuleLayout::ResizeForExpansion(ModuleId source, ModuleNormalizedRect expanded,
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
                                        shiftSource, extra, sourceLeft, sourceTop,
                                        sourceRight, sourceBottom](ModuleLayoutItem& item) {
        if (item.collapseMode == ModuleCollapseMode::None) return;
        const float width = horizontal ? extent : canvasWidth;
        const float height = horizontal ? canvasHeight : extent;
        // Only collapsed modules whose bounds overlap the expanding source shift
        // with the growth; unrelated collapsed handles keep their pixel position so
        // they stay visually attached to their own targets.
        const bool overlapsExpansion = ModuleLayout::Intersects(
            {item.handleX * canvasWidth, item.handleY * canvasHeight,
             (item.handleX + item.handleWidth) * canvasWidth,
             (item.handleY + item.handleHeight) * canvasHeight},
            {sourceLeft, sourceTop, sourceRight, sourceBottom});
        const float shift = !shiftSource && overlapsExpansion ? extra : 0.0F;
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
        if (item.collapsed) {
            // Rendering and hit testing use the primary rectangle for collapsed items;
            // keep it identical to the transformed handle rectangle.
            item.x = item.handleX;
            item.y = item.handleY;
            item.width = item.handleWidth;
            item.height = item.handleHeight;
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
        if (item.visible && !item.collapsed) SyncExpandedGeometry(item);
    }
    if (!ReattachOutsideCollapseHandles(before)) {
        *this = before;
        return false;
    }
    if (HasNewConflictingGeometry(before)) {
        *this = before;
        return false;
    }
    newWidth = horizontal ? extent : canvasWidth;
    newHeight = horizontal ? canvasHeight : extent;
    return true;
}

bool ModuleLayout::CollapseToWindow(ModuleId source, ModuleCollapseSide side,
                                    float edgePosition) noexcept {
    if (!CanCollapseSource(source) || !IsCollapseSide(side)) return false;
    const auto* sourceItem = Find(source);
    if (!sourceItem) return false;
    const ModuleLayout before = *this;
    const auto fail = [this, &before]() noexcept {
        *this = before;
        return false;
    };
    const bool sourceWasSnapGrouped = IsSnapGrouped(source);
    if (sourceWasSnapGrouped) {
        std::array<ModuleId, 6> members{};
        const auto memberCount = MovingMembers(source, members);
        for (std::size_t index = 0; index < memberCount; ++index) {
            if (auto* member = Find(members[index]); member != nullptr && !member->collapsed) {
                SyncExpandedGeometry(*member);
            }
        }
    }
    const float width = sourceItem->width;
    const float height = sourceItem->height;
    if (!std::isfinite(width) || !std::isfinite(height) || width < 0.10F || height < 0.10F) {
        return fail();
    }

    edgePosition = std::clamp(std::isfinite(edgePosition) ? edgePosition : 0.5F, 0.0F, 1.0F);
    ModuleNormalizedRect expanded{};
    ModuleNormalizedRect handle{};
    if (IsHorizontalCollapseSide(side)) {
        constexpr float handleWidth = 0.06F;
        const float handleHeight = std::clamp(height * 0.22F, 0.08F, 0.18F);
        ModuleNormalizedRect available{};
        if (!FindplusWindowRectangle(source, {0.0F, 0.0F, 1.0F, 1.0F},
                                     side == ModuleCollapseSide::Left ? 0.0F : 1.0F,
                                     edgePosition, 0.10F, 0.10F, available, side, true)) {
            return fail();
        }
        const float fittedWidth = std::min(width, available.right - available.left);
        const float fittedHeight = std::min(height, available.bottom - available.top);
        if (!HasFiniteCanvasRectangle(available) || !std::isfinite(fittedWidth) ||
            !std::isfinite(fittedHeight) || fittedWidth < 0.10F || fittedHeight < 0.10F) {
            return fail();
        }
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
        if (!FindplusWindowRectangle(source, {0.0F, 0.0F, 1.0F, 1.0F}, edgePosition,
                                     side == ModuleCollapseSide::Top ? 0.0F : 1.0F,
                                     0.10F, 0.10F, available, side, true)) {
            return fail();
        }
        const float fittedWidth = std::min(width, available.right - available.left);
        const float fittedHeight = std::min(height, available.bottom - available.top);
        if (!HasFiniteCanvasRectangle(available) || !std::isfinite(fittedWidth) ||
            !std::isfinite(fittedHeight) || fittedWidth < 0.10F || fittedHeight < 0.10F) {
            return fail();
        }
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
    if (!HasFiniteCanvasRectangle(expanded) || !HasFiniteCanvasRectangle(handle) ||
        expanded.right - expanded.left < 0.10F || expanded.bottom - expanded.top < 0.10F ||
        handle.right - handle.left < 0.001F || handle.bottom - handle.top < 0.001F) {
        return fail();
    }
    if (HasCollapseExpansionConflict(source, expanded)) return fail();
    auto* item = Find(source);
    if (!item) return fail();
    SetCollapsedGeometry(*item, handle, expanded, ModuleCollapseMode::Outside, side, source, true);
    if (!sourceWasSnapGrouped) snapGroup[static_cast<std::size_t>(item - items.data())] = source;
    if (HasNewConflictingGeometry(before)) {
        return fail();
    }
    return true;
}

bool ModuleLayout::CollapseToModule(ModuleId source, ModuleId target, ModuleCollapseSide side,
                                    ModuleCollapseMode mode, float handleTrackThickness,
                                    float edgePosition) noexcept {
    if (!CanCollapseSource(source) || source == target || !IsCollapseSide(side) ||
        (mode != ModuleCollapseMode::Inside && mode != ModuleCollapseMode::Outside)) {
        return false;
    }
    const auto* sourceItem = Find(source);
    const auto targetRoot = TabRoot(target);
    const auto* targetItem = Find(targetRoot);
    if (!sourceItem || !targetItem || !targetItem->visible ||
        IsEffectivelyCollapsed(targetRoot) ||
        (IsTabbed(source) && IsTabbed(target) && TabRoot(source) == targetRoot)) {
        return false;
    }
    std::array<ModuleId, 6> collapseTargets{};
    std::size_t collapseTargetCount = 0;
    ModuleId collapseTarget = targetRoot;
    while (const auto* candidate = Find(collapseTarget)) {
        if (collapseTarget == source) return false;
        if (candidate->collapseMode != ModuleCollapseMode::Inside ||
            candidate->collapseTargetIsWindow) break;
        collapseTarget = TabRoot(candidate->collapseTarget);
        if (Contains(collapseTargets, collapseTargetCount, collapseTarget)) return false;
        if (collapseTargetCount < collapseTargets.size()) {
            collapseTargets[collapseTargetCount++] = collapseTarget;
        }
    }
    const ModuleLayout before = *this;
    const auto fail = [this, &before]() noexcept {
        *this = before;
        return false;
    };
    const bool sourceWasSnapGrouped = IsSnapGrouped(source);
    if (sourceWasSnapGrouped) {
    std::array<ModuleId, 6> members{};
        const auto memberCount = MovingMembers(source, members);
        for (std::size_t index = 0; index < memberCount; ++index) {
            if (auto* member = Find(members[index]); member != nullptr && !member->collapsed) {
                SyncExpandedGeometry(*member);
            }
        }
    }
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
        const float handleLength = IsHorizontalCollapseSide(side)
            ? (targetBounds.bottom - targetBounds.top) * 0.20F
            : (targetBounds.right - targetBounds.left) * 0.20F;
        handle = MakeInsideCollapseHandle(targetBounds, side, handleTrackThickness, handleLength);
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
            return fail();
        }
    } else {
        const float width = sourceItem->width;
        const float height = sourceItem->height;
        if (!std::isfinite(width) || !std::isfinite(height) || width < 0.10F ||
            height < 0.10F) {
            return fail();
        }
        edgePosition = std::clamp(std::isfinite(edgePosition) ? edgePosition : 0.5F, 0.0F, 1.0F);
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
            case ModuleCollapseSide::None: return ModuleNormalizedRect{};
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
            return fail();
        }
        const float expandedWidth = std::min(width, available.right - available.left);
        const float expandedHeight = std::min(height, available.bottom - available.top);
        if (side == ModuleCollapseSide::Left || side == ModuleCollapseSide::Right) {
            const float top = std::clamp(edgePosition - expandedHeight * 0.5F,
                                         available.top, available.bottom - expandedHeight);
            expanded = side == ModuleCollapseSide::Left
                ? ModuleNormalizedRect{targetBounds.left - expandedWidth, top, targetBounds.left,
                                       top + expandedHeight}
                : ModuleNormalizedRect{targetBounds.right, top,
                                       targetBounds.right + expandedWidth, top + expandedHeight};
        } else {
            const float left = std::clamp(edgePosition - expandedWidth * 0.5F,
                                          available.left, available.right - expandedWidth);
            expanded = side == ModuleCollapseSide::Top
                ? ModuleNormalizedRect{left, targetBounds.top - expandedHeight,
                                       left + expandedWidth, targetBounds.top}
                : ModuleNormalizedRect{left, targetBounds.bottom,
                                       left + expandedWidth, targetBounds.bottom + expandedHeight};
        }
        handle = expanded;
        constexpr float handleThickness = 0.06F;
        if (side == ModuleCollapseSide::Left) handle.left = expanded.right - handleThickness;
        else if (side == ModuleCollapseSide::Right) handle.right = expanded.left + handleThickness;
        else if (side == ModuleCollapseSide::Top) handle.top = expanded.bottom - handleThickness;
        else handle.bottom = expanded.top + handleThickness;
        if (expanded.left < 0.0F || expanded.top < 0.0F || expanded.right > 1.0F ||
            expanded.bottom > 1.0F || HasCollapseExpansionConflict(source, expanded, targetRoot)) {
            return fail();
        }
    }
    if (!HasFiniteOrderedRectangle(expanded) || !HasFiniteOrderedRectangle(handle) ||
        expanded.right - expanded.left < 0.10F || expanded.bottom - expanded.top < 0.10F ||
        handle.right - handle.left < 0.001F || handle.bottom - handle.top < 0.001F) {
        return fail();
    }
    auto* item = Find(source);
    if (!item) return fail();
    if (!sourceWasSnapGrouped) snapGroup[static_cast<std::size_t>(item - items.data())] = source;
    if (mode == ModuleCollapseMode::Inside) {
        ScaleCollapsedInsideTabGroup(targetRoot, targetBounds, collapsedTargetBounds);
    }
    SetCollapsedGeometry(*item, handle, expanded, mode, side, targetRoot, false);
    if (mode == ModuleCollapseMode::Inside) {
        SetTabGroupGeometry(targetRoot, collapsedTargetBounds);
    }
    if (!ReattachOutsideCollapseHandles(before)) {
        return fail();
    }
    if (HasNewConflictingGeometry(before)) {
        return fail();
    }
    return true;
}

bool ModuleLayout::ToggleCollapsedModule(ModuleId id, ModuleExpansionBehavior behavior,
                                         float canvasWidth, float canvasHeight,
                                         float* resizedWidth, float* resizedHeight) noexcept {
    auto* item = Find(id);
    if (!item || item->collapseMode == ModuleCollapseMode::None) return false;
    const ModuleLayout before = *this;
    if (!item->collapsed) {
        if (item->handleWidth < 0.001F || item->handleHeight < 0.001F) return false;
        if (item->collapseMode == ModuleCollapseMode::Inside && !item->collapseTargetIsWindow) {
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
            if (!ReattachOutsideCollapseHandles(before)) {
                *this = before;
                return false;
            }
        } else if (item->collapseMode == ModuleCollapseMode::Outside &&
                   !item->collapseTargetIsWindow) {
            const ModuleId targetRoot = TabRoot(item->collapseTarget);
            const auto* target = Find(targetRoot);
            if (!target || !target->visible || target->collapsed) return false;
            const auto targetBounds = Bounds(*target);
            auto expanded = AttachOutsideExpanded(Bounds(*item), targetBounds,
                                                   item->collapseSide);
            const bool horizontal = IsHorizontalCollapseSide(item->collapseSide);
            const float thickness = horizontal ? item->handleWidth : item->handleHeight;
            if (!(thickness > 0.001F)) return false;
            const auto handle = MakeOutsideCollapseHandle(expanded, item->collapseSide,
                                                          thickness);
            if (expanded.right - expanded.left < 0.10F ||
                expanded.bottom - expanded.top < 0.10F) {
                return false;
            }
            SetCollapsedGeometry(*item, handle, expanded, item->collapseMode,
                                 item->collapseSide, targetRoot, false);
            if (!ReattachOutsideCollapseHandles(before) ||
                HasNewConflictingGeometry(before)) {
                *this = before;
                return false;
            }
        } else {
            item->x = item->handleX;
            item->y = item->handleY;
            item->width = item->handleWidth;
            item->height = item->handleHeight;
            item->collapsed = true;
        }
        return true;
    }
    if (item->expandedWidth < 0.10F || item->expandedHeight < 0.10F) return false;
    ModuleNormalizedRect expanded{item->expandedX, item->expandedY,
                                  item->expandedX + item->expandedWidth,
                                  item->expandedY + item->expandedHeight};
    if (item->collapseMode == ModuleCollapseMode::Outside && !item->collapseTargetIsWindow) {
        const ModuleId targetRoot = TabRoot(item->collapseTarget);
        const auto* target = Find(targetRoot);
        if (!target || !target->visible || target->collapsed) return false;
        expanded = AttachOutsideExpanded(expanded, Bounds(*target), item->collapseSide);
        item->expandedX = expanded.left;
        item->expandedY = expanded.top;
        item->expandedWidth = expanded.right - expanded.left;
        item->expandedHeight = expanded.bottom - expanded.top;
    }
    if (item->collapseMode == ModuleCollapseMode::Inside && !item->collapseTargetIsWindow) {
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
        if (!ReattachOutsideCollapseHandles(before)) {
            *this = before;
            return false;
        }
    } else {
        item->x = expanded.left;
        item->y = expanded.top;
        item->width = expanded.right - expanded.left;
        item->height = expanded.bottom - expanded.top;
        item->collapsed = false;
    }
    if (!ReattachOutsideCollapseHandles(before)) {
        *this = before;
        return false;
    }
    const bool introducesConflict = HasNewConflictingGeometry(before);
    const bool outsideCanvas = expanded.left < 0.0F || expanded.top < 0.0F ||
        expanded.right > 1.0F || expanded.bottom > 1.0F;
    if (introducesConflict && !outsideCanvas && behavior == ModuleExpansionBehavior::Squash &&
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

bool ModuleLayout::ResizeModule(ModuleId id, float pointerX, float pointerY,
                                bool resizeRight, bool resizeBottom,
                                bool resizeLeft, bool resizeTop,
                                bool squashOverlapping) noexcept {
    const ModuleId geometryId = TabRoot(id);
    if (IsSnapGrouped(geometryId)) {
        return ResizeSnapGroup(geometryId, pointerX, pointerY, resizeRight,
                               resizeBottom, resizeLeft, resizeTop,
                               squashOverlapping);
    }

    auto* item = Find(geometryId);
    if (!item) return false;
    const ModuleLayout before = *this;
    const auto oldBounds = Bounds(*item);
    ModuleNormalizedRect newBounds = oldBounds;
    if (resizeLeft) {
        newBounds.left = std::clamp(pointerX, 0.0F,
                                    oldBounds.right - kMinimumModuleExtent);
    }
    if (resizeTop) {
        newBounds.top = std::clamp(pointerY, 0.0F,
                                   oldBounds.bottom - kMinimumModuleExtent);
    }
    if (resizeRight) {
        newBounds.right = std::clamp(pointerX,
                                     oldBounds.left + kMinimumModuleExtent, 1.0F);
    }
    if (resizeBottom) {
        newBounds.bottom = std::clamp(pointerY,
                                      oldBounds.top + kMinimumModuleExtent, 1.0F);
    }
    if (squashOverlapping) {
        SquashResizeObstacles(*this, geometryId, oldBounds, newBounds,
                              resizeRight, resizeBottom, resizeLeft, resizeTop);
    }
    if (newBounds.right - newBounds.left < kMinimumModuleExtent ||
        newBounds.bottom - newBounds.top < kMinimumModuleExtent) return false;

    if (IsTabbed(geometryId)) SetTabGroupGeometry(geometryId, newBounds);
    else {
        item->x = newBounds.left;
        item->y = newBounds.top;
        item->width = newBounds.right - newBounds.left;
        item->height = newBounds.bottom - newBounds.top;
        if (!item->collapsed) SyncExpandedGeometry(*item);
    }
    ScaleCollapsedInsideModules(geometryId, oldBounds, newBounds);
    if (!ReattachOutsideCollapseHandles(before)) {
        *this = before;
        return false;
    }
    if (squashOverlapping && HasNewConflictingGeometry(before)) {
        *this = before;
        return false;
    }
    return true;
}

bool ModuleLayout::ResizeSnapGroup(ModuleId id, float pointerX, float pointerY,
                                   bool resizeRight, bool resizeBottom,
                                   bool resizeLeft, bool resizeTop,
                                   bool squashOverlapping) noexcept {
    if (!IsSnapGrouped(id)) return false;
    const ModuleLayout before = *this;
    const auto root = SnapRoot(id);
    const auto* selected = Find(id);
    if (!selected) return false;

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
        const float basisWidth = item.collapsed ? item.expandedWidth : item.width;
        const float basisHeight = item.collapsed ? item.expandedHeight : item.height;
        if (std::isfinite(basisWidth) && basisWidth > 0.0F) {
            minimumScaleX = std::max(minimumScaleX, 0.10F / basisWidth);
        }
        if (std::isfinite(basisHeight) && basisHeight > 0.0F) {
            minimumScaleY = std::max(minimumScaleY, 0.10F / basisHeight);
        }
    }
    for (const auto& item : items) {
        if (!item.visible || !item.collapsed ||
            item.collapseMode != ModuleCollapseMode::Inside ||
            item.collapseTargetIsWindow ||
            SnapRoot(TabRoot(item.collapseTarget)) != root ||
            !(item.expandedWidth > 0.0F) || !(item.expandedHeight > 0.0F)) {
            continue;
        }
        minimumScaleX = std::max(minimumScaleX, 0.10F / item.expandedWidth);
        minimumScaleY = std::max(minimumScaleY, 0.10F / item.expandedHeight);
    }
    const float groupWidth = groupRight - groupLeft;
    const float groupHeight = groupBottom - groupTop;
    if (!(groupWidth > 0.0F) || !(groupHeight > 0.0F)) return false;

    float newLeft = groupLeft;
    float newTop = groupTop;
    float newRight = groupRight;
    float newBottom = groupBottom;
    if (resizeRight) newRight += pointerX - (selected->x + selected->width);
    if (resizeLeft) newLeft += pointerX - selected->x;
    if (resizeBottom) newBottom += pointerY - (selected->y + selected->height);
    if (resizeTop) newTop += pointerY - selected->y;

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

    for (const auto& item : items) {
        if (!item.visible || SnapRoot(item.id) == root ||
            (item.collapsed && item.collapseMode == ModuleCollapseMode::Outside &&
             !item.collapseTargetIsWindow &&
             SnapRoot(TabRoot(item.collapseTarget)) == root)) {
            continue;
        }
        const auto obstacle = Bounds(item);
        if (squashOverlapping && resizeRight && obstacle.left >= groupRight &&
            newTop < obstacle.bottom && newBottom > obstacle.top) {
            newRight = std::min(newRight, obstacle.right - kMinimumModuleExtent);
        }
        if (squashOverlapping && resizeLeft && obstacle.right <= groupLeft &&
            newTop < obstacle.bottom && newBottom > obstacle.top) {
            newLeft = std::max(newLeft, obstacle.left + kMinimumModuleExtent);
        }
        if (squashOverlapping && resizeBottom && obstacle.top >= groupBottom &&
            newLeft < obstacle.right && newRight > obstacle.left) {
            newBottom = std::min(newBottom, obstacle.bottom - kMinimumModuleExtent);
        }
        if (squashOverlapping && resizeTop && obstacle.bottom <= groupTop &&
            newLeft < obstacle.right && newRight > obstacle.left) {
            newTop = std::max(newTop, obstacle.top + kMinimumModuleExtent);
        }
    }
    if (newRight - newLeft < minimumWidth || newBottom - newTop < minimumHeight) return false;

    const float scaleX = (newRight - newLeft) / groupWidth;
    const float scaleY = (newBottom - newTop) / groupHeight;
    for (auto& item : items) {
        if (!item.visible || SnapRoot(item.id) != root) continue;
        item.x = newLeft + (item.x - groupLeft) * scaleX;
        item.y = newTop + (item.y - groupTop) * scaleY;
        item.width *= scaleX;
        item.height *= scaleY;
        if (!item.collapsed) SyncExpandedGeometry(item);
    }
    ScaleCollapsedInsideModules(root, {groupLeft, groupTop, groupRight, groupBottom},
                                {newLeft, newTop, newRight, newBottom});
    if (squashOverlapping) {
        ModuleNormalizedRect newBounds{newLeft, newTop, newRight, newBottom};
        SquashResizeObstacles(*this, root, {groupLeft, groupTop, groupRight, groupBottom},
                              newBounds, resizeRight, resizeBottom, resizeLeft, resizeTop);
        if (HasNewConflictingGeometry(before)) {
            *this = before;
            return false;
        }
    }
    if (!ReattachOutsideCollapseHandles(before)) {
        *this = before;
        return false;
    }
    return true;
}

} // namespace rivan::ui
