// ModuleLayout.cpp
#include "ModuleLayout.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rivan::ui {

namespace {

constexpr float kMinimumModuleExtent = 0.10F;

bool HasUsableExpandedGeometry(const ModuleLayoutItem& item) noexcept {
    if (!std::isfinite(item.expandedX) || !std::isfinite(item.expandedY) ||
        !std::isfinite(item.expandedWidth) || !std::isfinite(item.expandedHeight) ||
        item.expandedWidth < kMinimumModuleExtent ||
        item.expandedHeight < kMinimumModuleExtent) {
        return false;
    }
    return std::isfinite(item.expandedX + item.expandedWidth) &&
           std::isfinite(item.expandedY + item.expandedHeight);
}

bool HasFiniteOrderedRectangle(const ModuleNormalizedRect& bounds) noexcept {
    return std::isfinite(bounds.left) && std::isfinite(bounds.top) &&
           std::isfinite(bounds.right) && std::isfinite(bounds.bottom) &&
           bounds.right > bounds.left && bounds.bottom > bounds.top &&
           std::isfinite(bounds.right - bounds.left) &&
           std::isfinite(bounds.bottom - bounds.top);
}

bool HasFiniteCanvasRectangle(const ModuleNormalizedRect& bounds) noexcept {
    return HasFiniteOrderedRectangle(bounds) && bounds.left >= 0.0F && bounds.top >= 0.0F &&
           bounds.right <= 1.0F && bounds.bottom <= 1.0F;
}

bool HasUsableExpandedRectangle(const ModuleNormalizedRect& bounds) noexcept {
    return HasFiniteOrderedRectangle(bounds) &&
           bounds.right - bounds.left >= kMinimumModuleExtent &&
           bounds.bottom - bounds.top >= kMinimumModuleExtent;
}

bool HasUsableTargetRectangle(const ModuleNormalizedRect& bounds) noexcept {
    return HasFiniteCanvasRectangle(bounds) &&
           bounds.right - bounds.left >= kMinimumModuleExtent &&
           bounds.bottom - bounds.top >= kMinimumModuleExtent;
}

bool HasUsableHandleRectangle(const ModuleNormalizedRect& bounds) noexcept {
    return HasFiniteCanvasRectangle(bounds) && bounds.right - bounds.left >= 0.001F &&
           bounds.bottom - bounds.top >= 0.001F;
}

bool IsMember(const std::array<ModuleId, 6>& members, std::size_t count,
              ModuleId id) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (members[index] == id) return true;
    }
    return false;
}

void SquashResizeObstacles(ModuleLayout& layout, ModuleId resizedId,
                           const ModuleNormalizedRect& oldBounds,
                           ModuleNormalizedRect& newBounds,
                           bool resizeRight, bool resizeBottom,
                           bool resizeLeft, bool resizeTop) noexcept {
    std::array<ModuleId, 6> moving{};
    const auto movingCount = layout.MovingMembers(resizedId, moving);
    for (const auto& item : layout.items) {
        if (!item.visible || IsMember(moving, movingCount, item.id) ||
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
        if (!item.visible || IsMember(moving, movingCount, item.id) ||
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

ModuleLayout ModuleLayout::Defaults() noexcept {
    return ModuleLayout{
        {{{ModuleId::Rivan, 0.0F, 0.0F, 1.0F, 0.32946298F, true, ModuleDockState::Snapped},
           {ModuleId::AllMusic, 0.0F, 0.27F, 0.44F, 0.45F, false, ModuleDockState::Floating},
           {ModuleId::GraphicEqualizer, 0.0F, 0.75F, 0.44F, 0.25F, false,
            ModuleDockState::Floating},
           {ModuleId::RivanLibrary, 0.0F, 0.32946298F, 1.0F, 0.670537F, true,
            ModuleDockState::Snapped},
           {ModuleId::VideoPreview, 0.46F, 0.49F, 0.54F, 0.24F, false,
            ModuleDockState::Floating},
           {ModuleId::Lyrics, 0.46F, 0.76F, 0.54F, 0.24F, false,
            ModuleDockState::Floating}}},
        {}, 0, 0,
        {ModuleId::Rivan, ModuleId::AllMusic, ModuleId::GraphicEqualizer,
          ModuleId::RivanLibrary, ModuleId::VideoPreview, ModuleId::Lyrics},
        {ModuleId::Rivan, ModuleId::AllMusic, ModuleId::GraphicEqualizer,
         ModuleId::RivanLibrary, ModuleId::VideoPreview, ModuleId::Lyrics},
        {}};
}

ModuleLayoutItem* ModuleLayout::Find(ModuleId id) noexcept {
    for (auto& item : items) {
        if (item.id == id) return &item;
    }
    return nullptr;
}

const ModuleLayoutItem* ModuleLayout::Find(ModuleId id) const noexcept {
    for (const auto& item : items) {
        if (item.id == id) return &item;
    }
    return nullptr;
}

bool ModuleLayout::IsTabbed(ModuleId id) const noexcept {
    return GroupTabCount(id) >= 2;
}

bool ModuleLayout::IsSnapped(ModuleId id) const noexcept {
    const auto* item = Find(id);
    return item != nullptr && item->dockState == ModuleDockState::Snapped;
}

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

void ModuleLayout::SyncExpandedGeometry(ModuleLayoutItem& item) noexcept {
    if (item.collapsed) return;
    item.expandedX = item.x;
    item.expandedY = item.y;
    item.expandedWidth = item.width;
    item.expandedHeight = item.height;
}

void ModuleLayout::SetTabGroupGeometry(ModuleId root, ModuleNormalizedRect bounds) noexcept {
    for (auto& candidate : items) {
        const bool belongsToTarget = candidate.id == root ||
            (IsTabbed(candidate.id) && TabRoot(candidate.id) == root);
        if (!belongsToTarget) continue;
        candidate.x = bounds.left;
        candidate.y = bounds.top;
        candidate.width = bounds.right - bounds.left;
        candidate.height = bounds.bottom - bounds.top;
        if (!candidate.collapsed) SyncExpandedGeometry(candidate);
    }
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

ModuleId ModuleLayout::SnapRoot(ModuleId id) const noexcept {
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (items[index].id == id) {
            const auto candidate = snapGroup[index];
            return Find(candidate) != nullptr ? candidate : id;
        }
    }
    return id;
}

bool ModuleLayout::HasValidGeometry() const noexcept {
    for (const auto& item : items) {
        if (!std::isfinite(item.x) || !std::isfinite(item.y) ||
            !std::isfinite(item.width) || !std::isfinite(item.height) ||
            item.width <= 0.0F || item.height <= 0.0F || item.x < 0.0F || item.y < 0.0F ||
            item.x + item.width > 1.0F || item.y + item.height > 1.0F) {
            return false;
        }
        if (!item.collapsed && (item.width < 0.10F || item.height < 0.10F)) return false;
        if (item.collapsed && !HasUsableExpandedGeometry(item)) {
            return false;
        }
    }
    return true;
}

std::size_t ModuleLayout::TabCount() const noexcept {
    return std::min(tabCount, tabOrder.size());
}

ModuleNormalizedRect ModuleLayout::Bounds(const ModuleLayoutItem& item) noexcept {
    return {item.x, item.y, item.x + item.width, item.y + item.height};
}

bool ModuleLayout::Intersects(const ModuleNormalizedRect& first,
                              const ModuleNormalizedRect& second) noexcept {
    return first.left < second.right && first.right > second.left &&
           first.top < second.bottom && first.bottom > second.top;
}

bool ModuleLayout::Contains(const std::array<ModuleId, 6>& ids,
                            std::size_t count, ModuleId id) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (ids[index] == id) return true;
    }
    return false;
}

std::size_t ModuleLayout::MovingMembers(ModuleId id,
                                        std::array<ModuleId, 6>& members) const noexcept {
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

bool ModuleLayout::HasGeometryConflict(ModuleId firstId, ModuleId secondId) const noexcept {
    const auto* first = Find(firstId);
    const auto* second = Find(secondId);
    if (!first || !second || !first->visible || !second->visible) return false;
    if (IsTabbed(first->id) && IsTabbed(second->id) &&
        TabRoot(first->id) == TabRoot(second->id)) {
        return false;
    }
    if (IsInsideCollapseOverlap(*first, *second) || IsInsideCollapseOverlap(*second, *first)) {
        return false;
    }
    if (IsEffectivelyCollapsed(firstId) || IsEffectivelyCollapsed(secondId)) return false;
    return Intersects(Bounds(*first), Bounds(*second));
}

bool ModuleLayout::HasConflictingGeometry() const noexcept {
    for (std::size_t first = 0; first < items.size(); ++first) {
        for (std::size_t second = first + 1; second < items.size(); ++second) {
            if (HasGeometryConflict(items[first].id, items[second].id)) return true;
        }
    }
    return false;
}

bool ModuleLayout::DisableDuplicateIndependentModules() noexcept {
    constexpr float tolerance = 0.0001F;
    bool disabled = false;
    for (std::size_t first = 0; first < items.size(); ++first) {
        const auto& firstItem = items[first];
        if (!firstItem.visible) continue;
        for (std::size_t second = first + 1; second < items.size(); ++second) {
            auto& secondItem = items[second];
            if (!secondItem.visible) continue;
            if (std::abs(firstItem.x - secondItem.x) > tolerance ||
                std::abs(firstItem.y - secondItem.y) > tolerance ||
                std::abs(firstItem.width - secondItem.width) > tolerance ||
                std::abs(firstItem.height - secondItem.height) > tolerance) {
                continue;
            }
            if (!HasGeometryConflict(firstItem.id, secondItem.id)) continue;
            secondItem.visible = false;
            RemoveTab(secondItem.id);
            disabled = true;
        }
    }
    return disabled;
}

bool ModuleLayout::HasNewConflictingGeometry(const ModuleLayout& before) const noexcept {
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

bool ModuleLayout::FindplusWindowRectangle(
    ModuleId source, ModuleNormalizedRect region, float pointerX, float pointerY,
    float minimumWidth, float minimumHeight, ModuleNormalizedRect& result,
    std::optional<ModuleCollapseSide> attachedSide, bool attachWindowEdge) const noexcept {
    if (!std::isfinite(region.left) || !std::isfinite(region.top) ||
        !std::isfinite(region.right) || !std::isfinite(region.bottom) ||
        !std::isfinite(pointerX) || !std::isfinite(pointerY) ||
        !std::isfinite(minimumWidth) || !std::isfinite(minimumHeight) ||
        !(region.right > region.left) || !(region.bottom > region.top) ||
        !(minimumWidth > 0.0F) || !(minimumHeight > 0.0F)) {
        return false;
    }

    std::array<ModuleId, 6> moving{};
    const auto movingCount = MovingMembers(source, moving);
    std::array<ModuleNormalizedRect, 6> obstacles{};
    std::size_t obstacleCount = 0;
    for (const auto& item : items) {
        if (!item.visible ||
            (IsEffectivelyCollapsed(item.id) && !item.collapsed) ||
            Contains(moving, movingCount, item.id)) continue;
        if (IsTabbed(item.id) && TabRoot(item.id) != item.id) continue;
        const auto bounds = Bounds(item);
        if (!HasFiniteOrderedRectangle(bounds)) return false;
        if (bounds.right <= region.left || bounds.left >= region.right ||
            bounds.bottom <= region.top || bounds.top >= region.bottom) {
            continue;
        }
        if (obstacleCount < obstacles.size()) obstacles[obstacleCount++] = bounds;
    }

    std::array<float, 16> xCoordinates{};
    std::array<float, 16> yCoordinates{};
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
    std::sort(xCoordinates.begin(),
              xCoordinates.begin() + static_cast<std::ptrdiff_t>(xCount));
    std::sort(yCoordinates.begin(),
              yCoordinates.begin() + static_cast<std::ptrdiff_t>(yCount));

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
                    const float score = (containsPointer ? 1000.0F : 0.0F) + area -
                        (distanceX * distanceX + distanceY * distanceY);
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

bool ModuleLayout::IsSnapGrouped(ModuleId id) const noexcept {
    const auto root = SnapRoot(id);
    std::size_t count = 0;
    for (const auto& item : items) {
        if (item.visible && SnapRoot(item.id) == root) ++count;
    }
    return count > 1;
}

void ModuleLayout::DetachSnapModule(ModuleId id) noexcept {
    if (!IsSnapGrouped(id)) return;
    const auto oldRoot = SnapRoot(id);
    std::array<ModuleId, 6> remaining{};
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

void ModuleLayout::DetachSnapMembers(const std::array<ModuleId, 6>& members,
                                     std::size_t count) noexcept {
    count = std::min(count, members.size());
    if (count == 0) return;

    std::array<ModuleId, 6> oldRoots{};
    for (std::size_t index = 0; index < items.size(); ++index) {
        oldRoots[index] = SnapRoot(items[index].id);
    }

    const auto isDetached = [&members, count](ModuleId id) noexcept {
        return ModuleLayout::Contains(members, count, id);
    };
    std::array<ModuleId, 6> affectedRoots{};
    std::size_t affectedCount = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const auto* item = Find(members[index]);
        if (!item) continue;
        const auto itemIndex = static_cast<std::size_t>(item - items.data());
        const ModuleId root = oldRoots[itemIndex];
        if (!Contains(affectedRoots, affectedCount, root) &&
            affectedCount < affectedRoots.size()) {
            affectedRoots[affectedCount++] = root;
        }
    }

    for (std::size_t index = 0; index < count; ++index) {
        if (auto* item = Find(members[index])) {
            const auto itemIndex = static_cast<std::size_t>(item - items.data());
            snapGroup[itemIndex] = item->id;
            item->dockState = ModuleDockState::Floating;
        }
    }

    for (std::size_t rootIndex = 0; rootIndex < affectedCount; ++rootIndex) {
        const ModuleId oldRoot = affectedRoots[rootIndex];
        std::array<std::size_t, 6> remaining{};
        std::size_t remainingCount = 0;
        for (std::size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex) {
            if (items[itemIndex].visible && !isDetached(items[itemIndex].id) &&
                oldRoots[itemIndex] == oldRoot && remainingCount < remaining.size()) {
                remaining[remainingCount++] = itemIndex;
            }
        }
        const ModuleId newRoot = remainingCount == 0
            ? oldRoot
            : (isDetached(oldRoot) ? items[remaining[0]].id : oldRoot);
        for (std::size_t index = 0; index < remainingCount; ++index) {
            auto& item = items[remaining[index]];
            snapGroup[remaining[index]] = newRoot;
            item.dockState = remainingCount > 1
                ? ModuleDockState::Snapped : ModuleDockState::Floating;
        }
    }
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

bool ModuleLayout::PreservePixelGeometry(float oldWidth, float oldHeight,
                                          float newWidth, float newHeight,
                                          bool resizeRight, bool resizeBottom,
                                          bool resizeLeft, bool resizeTop) noexcept {
    if (!(oldWidth > 0.0F) || !(oldHeight > 0.0F) ||
        !(newWidth > 0.0F) || !(newHeight > 0.0F)) {
        return false;
    }

    const auto preserveAxis = [this](float oldExtent, float newExtent, bool horizontal,
                                     bool resizeLeading, bool resizeTrailing) {
        if (std::abs(oldExtent - newExtent) < 0.01F) return false;

        constexpr float edgeTolerance = 0.01F;
        const bool edgeResize = resizeLeading || resizeTrailing;
        const float delta = newExtent - oldExtent;
        bool hasVisibleItem = false;
        float shift = 0.0F;
        float minimum = std::numeric_limits<float>::infinity();
        float maximum = -std::numeric_limits<float>::infinity();
        if (!edgeResize) {
            for (const auto& item : items) {
                if (!item.visible) continue;
                hasVisibleItem = true;
                const float position = (horizontal ? item.x : item.y) * oldExtent;
                const float length = (horizontal ? item.width : item.height) * oldExtent;
                minimum = std::min(minimum, position);
                maximum = std::max(maximum, position + length);
            }
            if (!std::isfinite(minimum) || !std::isfinite(maximum) ||
                maximum - minimum > newExtent) {
                return false;
            }
            shift = std::clamp(0.0F, -minimum, newExtent - maximum);
        } else {
            for (const auto& item : items) {
                if (item.visible) {
                    hasVisibleItem = true;
                    break;
                }
            }
        }
        if (!hasVisibleItem) return false;

        const auto transform = [oldExtent, newExtent, resizeLeading, resizeTrailing,
                                edgeResize, delta, shift](float& position, float& length) {
            constexpr float edgeTolerance = 0.01F;
            float leading = position * oldExtent;
            float trailing = leading + length * oldExtent;
            if (edgeResize) {
                const bool touchesLeading = leading <= edgeTolerance;
                const bool touchesTrailing = trailing >= oldExtent - edgeTolerance;
                if (resizeLeading) {
                    leading += delta;
                    trailing += delta;
                    if (touchesLeading) leading = 0.0F;
                }
                if (resizeTrailing && touchesTrailing) trailing = newExtent;
            } else {
                leading += shift;
                trailing += shift;
            }
            position = leading / newExtent;
            length = (trailing - leading) / newExtent;
        };
        const auto transformCollapsedHandle = [oldExtent, newExtent, resizeLeading,
                                               resizeTrailing, edgeResize, delta,
                                               shift](float& position, float& length) {
            constexpr float edgeTolerance = 0.01F;
            float leading = position * oldExtent;
            const float originalLength = std::max(0.0F, length * oldExtent);
            float trailing = leading + originalLength;
            const bool touchesLeading = leading <= edgeTolerance;
            const bool touchesTrailing = trailing >= oldExtent - edgeTolerance;
            if (edgeResize) {
                if (resizeLeading) {
                    leading += delta;
                    trailing = leading + originalLength;
                    if (touchesLeading) {
                        leading = 0.0F;
                        trailing = originalLength;
                    }
                }
                if (resizeTrailing && touchesTrailing) {
                    trailing = newExtent;
                    leading = trailing - originalLength;
                }
            } else {
                leading += shift;
                trailing += shift;
            }
            const float clampedLength = std::min(originalLength, newExtent);
            leading = std::clamp(leading, 0.0F, newExtent - clampedLength);
            trailing = leading + clampedLength;
            position = leading / newExtent;
            length = clampedLength / newExtent;
        };
        const auto transformCollapsedExpanded = [oldExtent, newExtent, resizeLeading,
                                                 edgeResize, delta, shift](float& position,
                                                                            float& length) {
            constexpr float edgeTolerance = 0.01F;
            float leading = position * oldExtent;
            const float physicalLength = length * oldExtent;
            if (edgeResize) {
                if (resizeLeading) {
                    leading += delta;
                    if (leading <= edgeTolerance) leading = 0.0F;
                }
            } else {
                leading += shift;
            }
            position = leading / newExtent;
            length = physicalLength / newExtent;
        };

        ModuleLayout candidate = *this;
        for (auto& item : candidate.items) {
            if (!item.visible) continue;
            if (horizontal) {
                if (item.collapsed) {
                    transformCollapsedHandle(item.x, item.width);
                    item.handleX = item.x;
                    item.handleWidth = item.width;
                } else {
                    transform(item.x, item.width);
                    if (item.collapseMode != ModuleCollapseMode::None) {
                        transform(item.handleX, item.handleWidth);
                    }
                }
                if (item.collapsed) {
                    transformCollapsedExpanded(item.expandedX, item.expandedWidth);
                }
            } else {
                if (item.collapsed) {
                    transformCollapsedHandle(item.y, item.height);
                    item.handleY = item.y;
                    item.handleHeight = item.height;
                } else {
                    transform(item.y, item.height);
                    if (item.collapseMode != ModuleCollapseMode::None) {
                        transform(item.handleY, item.handleHeight);
                    }
                }
                if (item.collapsed) {
                    transformCollapsedExpanded(item.expandedY, item.expandedHeight);
                }
            }
            const float position = horizontal ? item.x : item.y;
            const float length = horizontal ? item.width : item.height;
            const bool outsideModuleCollapse = item.collapsed &&
                item.collapseMode == ModuleCollapseMode::Outside &&
                !item.collapseTargetIsWindow;
            if (!std::isfinite(position) || !std::isfinite(length) || !(length > 0.0F) ||
                (!outsideModuleCollapse &&
                 (position < -edgeTolerance || position + length > 1.0F + edgeTolerance))) {
                return false;
            }
            if (item.collapsed) {
                if (!HasUsableExpandedGeometry(item)) {
                    return false;
                }
            }
            if (!item.collapsed) ModuleLayout::SyncExpandedGeometry(item);
        }
        bool hasOutsideModuleCollapse = false;
        for (const auto& item : candidate.items) {
            if (item.visible && item.collapsed &&
                item.collapseMode == ModuleCollapseMode::Outside &&
                !item.collapseTargetIsWindow) {
                hasOutsideModuleCollapse = true;
                break;
            }
        }
        if (hasOutsideModuleCollapse && !candidate.ReattachOutsideCollapseHandles(*this)) {
            return false;
        }
        *this = candidate;
        return true;
    };

    const bool changedX = preserveAxis(oldWidth, newWidth, true, resizeLeft, resizeRight);
    const bool changedY = preserveAxis(oldHeight, newHeight, false, resizeTop, resizeBottom);
    return changedX || changedY;
}

bool ModuleLayout::PreserveCollapsedExpandedGeometry(
    float oldWidth, float oldHeight, float newWidth, float newHeight,
    bool /*resizeRight*/, bool /*resizeBottom*/, bool resizeLeft, bool resizeTop) noexcept {
    if (!std::isfinite(oldWidth) || !std::isfinite(oldHeight) ||
        !std::isfinite(newWidth) || !std::isfinite(newHeight) ||
        !(oldWidth > 0.0F) || !(oldHeight > 0.0F) ||
        !(newWidth > 0.0F) || !(newHeight > 0.0F)) {
        return false;
    }

    const auto preserveAxis = [](float oldExtent, float newExtent,
                                  bool resizeLeading, float& position,
                                  float& length) noexcept {
        const float physicalPosition = position * oldExtent;
        const float physicalLength = length * oldExtent;
        if (!std::isfinite(physicalPosition) || !std::isfinite(physicalLength)) {
            return false;
        }

        const float leading = physicalPosition +
            (resizeLeading ? newExtent - oldExtent : 0.0F);
        position = leading / newExtent;
        length = physicalLength / newExtent;
        return std::isfinite(position) && std::isfinite(length);
    };

    ModuleLayout candidate = *this;
    bool changed = false;
    for (auto& item : candidate.items) {
        if (!item.collapsed) continue;
        if (!HasUsableExpandedGeometry(item)) return false;

        const float oldX = item.expandedX;
        const float oldY = item.expandedY;
        const float itemOldWidth = item.expandedWidth;
        const float itemOldHeight = item.expandedHeight;
        if (!preserveAxis(oldWidth, newWidth, resizeLeft,
                          item.expandedX, item.expandedWidth) ||
            !preserveAxis(oldHeight, newHeight, resizeTop,
                          item.expandedY, item.expandedHeight) ||
            !HasUsableExpandedGeometry(item)) {
            return false;
        }
        changed = changed || item.expandedX != oldX || item.expandedY != oldY ||
            item.expandedWidth != itemOldWidth || item.expandedHeight != itemOldHeight;
    }

    if (!changed) return false;
    *this = candidate;
    return true;
}

} // namespace rivan::ui
