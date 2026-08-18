// ModuleLayoutSnapping.cpp
#include "ModuleLayout.h"
#include "ModuleLayoutInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rivan::ui {

bool ModuleLayout::SnapTo(ModuleId source, ModuleId target, ModuleDropZone zone) noexcept {
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
    if (!targetItem || IsEffectivelyCollapsed(targetRoot) || !Find(sourceTabRoot)) return false;
    std::array<ModuleId, 6> sourceMembers{};
    const std::size_t sourceMemberCount = MovingMembers(source, sourceMembers);
    const ModuleLayoutItem targetGeometry = *targetItem;
    if (!HasFiniteOrderedRectangle(Bounds(targetGeometry))) return false;
    for (std::size_t index = 0; index < sourceMemberCount; ++index) {
        const auto* sourceItem = Find(sourceMembers[index]);
        if (!sourceItem || !HasFiniteOrderedRectangle(Bounds(*sourceItem))) return false;
    }
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
        sourceDestination.bottom - sourceDestination.top < 0.10F) {
        return false;
    }

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
            if (!item->collapsed) SyncExpandedGeometry(*item);
        }
    }
    ScaleCollapsedInsideModules(sourceSnapRoot, sourceBounds, sourceDestination);
    if (auto* item = Find(targetRoot)) {
        setGeometry(*item, targetDestination, ModuleDockState::Snapped);
        if (!item->collapsed) SyncExpandedGeometry(*item);
    }
    if (targetWasTabbed) {
        for (std::size_t index = 0; index < TabCount(); ++index) {
            if (auto* item = Find(tabOrder[index]);
                item != nullptr && TabRoot(item->id) == targetRoot) {
                setGeometry(*item, targetDestination, ModuleDockState::Snapped);
                if (!item->collapsed) SyncExpandedGeometry(*item);
            }
        }
    }
    ScaleCollapsedInsideTabGroup(targetRoot, Bounds(targetGeometry), targetDestination);
    for (std::size_t index = 0; index < sourceMemberCount; ++index) {
        if (auto* item = Find(sourceMembers[index])) {
            const auto itemIndex = static_cast<std::size_t>(item - items.data());
            snapGroup[itemIndex] = targetSnapRoot;
            item->dockState = ModuleDockState::Snapped;
            if (!item->collapsed) SyncExpandedGeometry(*item);
        }
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

bool ModuleLayout::SnapToWindow(ModuleId source, ModuleWindowDropZone zone,
                                 float pointerX, float pointerY) noexcept {
    if (!IsWindowDrop(zone) || !std::isfinite(pointerX) || !std::isfinite(pointerY)) return false;
    const auto region = ModuleWindowDropBounds(zone);
    ModuleNormalizedRect destination{};
    const ModuleLayout before = *this;
    const ModuleId sourceSnapRoot = SnapRoot(TabRoot(source));
    const auto fail = [this, &before]() noexcept {
        *this = before;
        return false;
    };

    for (const auto& item : items) {
        if (item.visible && !HasFiniteOrderedRectangle(Bounds(item))) return false;
    }
    const bool freeDestination = FindplusWindowRectangle(
        source, region, pointerX, pointerY, 0.10F, 0.10F, destination);

    std::array<ModuleId, 6> moving{};
    const auto movingCount = MovingMembers(source, moving);
    ModuleId targetRoot{};
    bool splitTargetFound = false;
    if (!freeDestination) {
        const ModuleLayoutItem* target = nullptr;
        for (const auto& item : items) {
            if (!item.visible || IsEffectivelyCollapsed(item.id) ||
                Contains(moving, movingCount, item.id)) continue;
            if (IsTabbed(item.id) && TabRoot(item.id) != item.id) continue;
            const auto bounds = Bounds(item);
            if (pointerX >= bounds.left && pointerX <= bounds.right &&
                pointerY >= bounds.top && pointerY <= bounds.bottom &&
                (!target || item.width * item.height > target->width * target->height)) {
                target = &item;
            }
        }
        if (!target) return false;
        targetRoot = TabRoot(target->id);
        const auto* targetRootItem = Find(targetRoot);
        if (!targetRootItem || !HasFiniteOrderedRectangle(Bounds(*targetRootItem))) {
            return false;
        }
        splitTargetFound = true;
        const auto targetGeometry = Bounds(*targetRootItem);
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
            destination.bottom - destination.top < 0.10F) {
            return false;
        }
    }
    if (!HasFiniteOrderedRectangle(destination) ||
        destination.right - destination.left < 0.10F ||
        destination.bottom - destination.top < 0.10F) {
        return false;
    }

    ModuleNormalizedRect sourceBounds{1.0F, 1.0F, 0.0F, 0.0F};
    for (std::size_t index = 0; index < movingCount; ++index) {
        const auto* item = Find(moving[index]);
        if (!item || !HasFiniteOrderedRectangle(Bounds(*item))) return false;
        sourceBounds.left = std::min(sourceBounds.left, item->x);
        sourceBounds.top = std::min(sourceBounds.top, item->y);
        sourceBounds.right = std::max(sourceBounds.right, item->x + item->width);
        sourceBounds.bottom = std::max(sourceBounds.bottom, item->y + item->height);
    }
    const float sourceWidth = sourceBounds.right - sourceBounds.left;
    const float sourceHeight = sourceBounds.bottom - sourceBounds.top;
    if (!(sourceWidth > 0.0F) || !(sourceHeight > 0.0F)) return false;
    const float scaleX = (destination.right - destination.left) / sourceWidth;
    const float scaleY = (destination.bottom - destination.top) / sourceHeight;
    for (std::size_t index = 0; index < movingCount; ++index) {
        auto* item = Find(moving[index]);
        if (!item) return fail();
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
        if (!item->collapsed) SyncExpandedGeometry(*item);
    }
    ScaleCollapsedInsideModules(sourceSnapRoot, sourceBounds, destination);
    if (!freeDestination && splitTargetFound) {
        auto* target = Find(targetRoot);
        if (!target || !HasFiniteOrderedRectangle(Bounds(*target))) return fail();
        const auto targetBounds = Bounds(*target);
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
        if (!HasFiniteOrderedRectangle(remainder) ||
            remainder.right - remainder.left < 0.10F ||
            remainder.bottom - remainder.top < 0.10F) {
            return fail();
        }
        target->x = remainder.left;
        target->y = remainder.top;
        target->width = remainder.right - remainder.left;
        target->height = remainder.bottom - remainder.top;
        target->dockState = ModuleDockState::Snapped;
        if (!target->collapsed) SyncExpandedGeometry(*target);
        for (std::size_t tab = 0; tab < TabCount(); ++tab) {
            if (auto* tabItem = Find(tabOrder[tab]);
                tabItem != nullptr && TabRoot(tabItem->id) == targetRoot) {
                tabItem->x = target->x;
                tabItem->y = target->y;
                tabItem->width = target->width;
                tabItem->height = target->height;
                if (!tabItem->collapsed) SyncExpandedGeometry(*tabItem);
            }
        }
        ScaleCollapsedInsideTabGroup(targetRoot, targetBounds, remainder);
        for (std::size_t index = 0; index < movingCount; ++index) {
            auto* moved = Find(moving[index]);
            if (!moved) return fail();
            snapGroup[static_cast<std::size_t>(moved - items.data())] = targetRoot;
        }
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

} // namespace rivan::ui
