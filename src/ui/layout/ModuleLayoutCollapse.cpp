// ModuleLayoutCollapse.cpp
#include "ModuleLayout.h"

#include <algorithm>
#include <cmath>

namespace rivan::ui {

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
        const auto sideCandidate = [&consider, &expanded](ModuleNormalizedRect bounds) {
            if (bounds.right - bounds.left >= 0.10F &&
                bounds.bottom - bounds.top >= 0.10F && !Intersects(bounds, expanded)) {
                consider(bounds);
            }
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
        if (item.visible) SyncExpandedGeometry(item);
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
    if (width < 0.10F || height < 0.10F) return false;

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
        if (!FindplusWindowRectangle(source, {0.0F, 0.0F, 1.0F, 1.0F}, edgePosition,
                                     side == ModuleCollapseSide::Top ? 0.0F : 1.0F,
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
    auto* item = Find(source);
    if (!item) return false;
    SetCollapsedGeometry(*item, handle, expanded, ModuleCollapseMode::Outside, side, source, true);
    if (!sourceWasSnapGrouped) snapGroup[static_cast<std::size_t>(item - items.data())] = source;
    if (HasNewConflictingGeometry(before)) {
        *this = before;
        return false;
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
            return false;
        }
    } else {
        const float width = sourceItem->width;
        const float height = sourceItem->height;
        if (width < 0.10F || height < 0.10F) return false;
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
            return false;
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
            return false;
        }
    }
    if (expanded.right - expanded.left < 0.10F || expanded.bottom - expanded.top < 0.10F ||
        handle.right - handle.left < 0.001F || handle.bottom - handle.top < 0.001F) {
        return false;
    }
    auto* item = Find(source);
    if (!item) return false;
    if (!sourceWasSnapGrouped) snapGroup[static_cast<std::size_t>(item - items.data())] = source;
    SetCollapsedGeometry(*item, handle, expanded, mode, side, targetRoot, false);
    if (mode == ModuleCollapseMode::Inside) SetTabGroupGeometry(targetRoot, collapsedTargetBounds);
    if (HasNewConflictingGeometry(before)) {
        *this = before;
        return false;
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
    const ModuleNormalizedRect expanded{item->expandedX, item->expandedY,
                                        item->expandedX + item->expandedWidth,
                                        item->expandedY + item->expandedHeight};
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

} // namespace rivan::ui
