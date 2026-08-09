// ModuleLayout.cpp
#include "ModuleLayout.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rivan::ui {

ModuleLayout ModuleLayout::Defaults() noexcept {
    return ModuleLayout{
        {{{ModuleId::Rivan, 0.0F, 0.0F, 0.44F, 0.24F, true, ModuleDockState::Floating},
           {ModuleId::AllMusic, 0.0F, 0.27F, 0.44F, 0.45F, true, ModuleDockState::Floating},
           {ModuleId::GraphicEqualizer, 0.0F, 0.75F, 0.44F, 0.25F, true,
            ModuleDockState::Floating},
           {ModuleId::RivanLibrary, 0.46F, 0.0F, 0.54F, 0.46F, true,
            ModuleDockState::Floating},
           {ModuleId::VideoPreview, 0.46F, 0.49F, 0.54F, 0.24F, true,
            ModuleDockState::Floating},
           {ModuleId::Lyrics, 0.46F, 0.76F, 0.54F, 0.24F, true,
            ModuleDockState::Floating}}},
        {}, 0, 0,
        {ModuleId::Rivan, ModuleId::AllMusic, ModuleId::GraphicEqualizer,
          ModuleId::RivanLibrary, ModuleId::VideoPreview, ModuleId::Lyrics}};
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
    const auto count = TabCount();
    if (count < 2) return false;
    for (std::size_t index = 0; index < count; ++index) {
        if (Find(tabOrder[index]) != nullptr && tabOrder[index] == id) return true;
    }
    return false;
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
            const bool hasExpandedGeometry = item->expandedWidth >= 0.10F &&
                item->expandedHeight >= 0.10F && item->expandedX >= 0.0F &&
                item->expandedY >= 0.0F && item->expandedX + item->expandedWidth <= 1.0F &&
                item->expandedY + item->expandedHeight <= 1.0F;
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

void ModuleLayout::ClearCollapseReferences(ModuleId target) noexcept {
    for (const auto& candidate : items) {
        if (candidate.id != target && candidate.collapseTarget == target &&
            candidate.collapseMode != ModuleCollapseMode::None) {
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
        SyncExpandedGeometry(candidate);
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
        if (item.collapsed && (!std::isfinite(item.expandedX) ||
                               !std::isfinite(item.expandedY) ||
                               !std::isfinite(item.expandedWidth) ||
                               !std::isfinite(item.expandedHeight) ||
                               item.expandedWidth < 0.10F || item.expandedHeight < 0.10F ||
                               item.expandedX < 0.0F || item.expandedY < 0.0F ||
                               item.expandedX + item.expandedWidth > 1.0F ||
                               item.expandedY + item.expandedHeight > 1.0F)) {
            return false;
        }
    }
    return true;
}

std::size_t ModuleLayout::TabCount() const noexcept {
    return std::min(tabCount, tabOrder.size());
}

std::size_t ModuleLayout::ActiveTabIndex() const noexcept {
    const auto count = TabCount();
    return count == 0 ? 0 : std::min(activeTab, count - 1);
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

bool ModuleLayout::FindplusWindowRectangle(
    ModuleId source, ModuleNormalizedRect region, float pointerX, float pointerY,
    float minimumWidth, float minimumHeight, ModuleNormalizedRect& result,
    std::optional<ModuleCollapseSide> attachedSide, bool attachWindowEdge) const noexcept {
    if (!(region.right > region.left) || !(region.bottom > region.top) ||
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
        if (bounds.right <= region.left || bounds.left >= region.right ||
            bounds.bottom <= region.top || bounds.top >= region.bottom) {
            continue;
        }
        if (obstacleCount < obstacles.size()) obstacles[obstacleCount++] = bounds;
    }

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

void ModuleLayout::ResizeSnapGroup(ModuleId id, float pointerX, float pointerY,
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
            SnapRoot(TabRoot(item.collapseTarget)) != root ||
            !(item.expandedWidth > 0.0F) || !(item.expandedHeight > 0.0F)) {
            continue;
        }
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

        ModuleLayout candidate = *this;
        for (auto& item : candidate.items) {
            if (!item.visible) continue;
            if (horizontal) {
                transform(item.x, item.width);
                if (item.collapseMode != ModuleCollapseMode::None) {
                    transform(item.handleX, item.handleWidth);
                }
                if (item.collapsed) transform(item.expandedX, item.expandedWidth);
            } else {
                transform(item.y, item.height);
                if (item.collapseMode != ModuleCollapseMode::None) {
                    transform(item.handleY, item.handleHeight);
                }
                if (item.collapsed) transform(item.expandedY, item.expandedHeight);
            }
            const float position = horizontal ? item.x : item.y;
            const float length = horizontal ? item.width : item.height;
            if (!std::isfinite(position) || !std::isfinite(length) || !(length > 0.0F) ||
                position < -edgeTolerance || position + length > 1.0F + edgeTolerance) {
                return false;
            }
            if (item.collapsed) {
                const float expandedPosition = horizontal ? item.expandedX : item.expandedY;
                const float expandedLength = horizontal ? item.expandedWidth : item.expandedHeight;
                if (!std::isfinite(expandedPosition) || !std::isfinite(expandedLength) ||
                    !(expandedLength >= 0.10F) || expandedPosition < -edgeTolerance ||
                    expandedPosition + expandedLength > 1.0F + edgeTolerance) {
                    return false;
                }
            }
            ModuleLayout::SyncExpandedGeometry(item);
        }
        *this = candidate;
        return true;
    };

    const bool changedX = preserveAxis(oldWidth, newWidth, true, resizeLeft, resizeRight);
    const bool changedY = preserveAxis(oldHeight, newHeight, false, resizeTop, resizeBottom);
    return changedX || changedY;
}

} // namespace rivan::ui
