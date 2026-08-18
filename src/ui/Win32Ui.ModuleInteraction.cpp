// Win32Ui.ModuleInteraction.cpp
// Module resize, drag, drop preview, tab detachment, and cursor methods.
#include "Win32UiImpl.h"

namespace rivan::ui {

[[nodiscard]] bool Win32Ui::Impl::BeginModuleResize(float x, float y) {
    if (model.miniPlayer || lastCanvas.width <= 0.0F || lastCanvas.height <= 0.0F) return false;
    constexpr float edge = 7.0F;
    // If the cursor shows a resize cursor here, always allow resize — hit-test guards
    // (title bar, tabs, collapse toggle) only apply when the cursor would be an arrow.
    const HCURSOR cursor = ModuleCursor(x, y);
    const bool isResizeCursor = cursor && cursor != LoadCursorW(nullptr, IDC_SIZEALL) &&
                                cursor != LoadCursorW(nullptr, IDC_HAND) &&
                                cursor != LoadCursorW(nullptr, IDC_ARROW);
    for (auto iterator = moduleRegions.rbegin(); iterator != moduleRegions.rend(); ++iterator) {
        if (!Contains(iterator->bounds, x, y)) continue;
        const bool right = std::abs(x - iterator->bounds.right) <= edge;
        const bool bottom = std::abs(y - iterator->bounds.bottom) <= edge;
        const bool left = std::abs(x - iterator->bounds.left) <= edge;
        const bool top = std::abs(y - iterator->bounds.top) <= edge;
        if (!right && !bottom && !left && !top) continue;
        if (!isResizeCursor) {
            // The tab strip is inset by 4px from the panel edge. The resize zone is 7px.
            // Only skip resize for the top edge when actually over the title bar (not the
            // outer 3px where the resize cursor shows). For left/right/bottom edges, no conflict.
            if (const auto* hit = HitTestContent(x, y);
                hit && (hit->kind == HitKind::ModuleTab || hit->kind == HitKind::ModuleCollapseToggle ||
                        (hit->kind == HitKind::ModuleTitle && top && y > iterator->bounds.top + 3.0F))) {
                continue;
            }
        }
        const auto geometryId = model.moduleLayout.TabRoot(iterator->id);
        const auto* item = model.moduleLayout.Find(geometryId);
        if (!item) return false;
        moduleLayoutDraft = model.moduleLayout;
        // Resize the visible rectangle, not the hidden rectangle belonging to the
        // currently selected tab.  This keeps all tabs at the same usable size.
        draggingModule = geometryId;
        moduleGesture = ModuleGesture::Resize;
        moduleDragActive = true;
        moduleResizeRight = right;
        moduleResizeBottom = bottom;
        moduleResizeLeft = left;
        moduleResizeTop = top;
        moduleDragStart = {x, y};
        ResetModuleDropPreview();
        SetCapture(window);
        return true;
    }
    return false;
}

void Win32Ui::Impl::BeginModuleDrag(ModuleId id, float x, float y,
                                     const ModuleLayout* layoutOverride,
                                     bool detachTabOnMove) {
    // If the cursor is showing a resize cursor at this position, don't start a drag.
    // This prevents accidentally moving a module when the user intends to resize.
    if (auto cursor = ModuleCursor(x, y);
        cursor && cursor != LoadCursorW(nullptr, IDC_SIZEALL)) {
        return;
    }
    const ModuleLayout& sourceLayout = layoutOverride ? *layoutOverride : model.moduleLayout;
    if (!sourceLayout.HasValidGeometry()) return;
    const auto geometryId = sourceLayout.TabRoot(id);
    const auto* sourceItem = sourceLayout.Find(geometryId);
    if (!sourceItem) return;
    if (sourceLayout.IsTabbed(id) && sourceLayout.Find(id) == nullptr) return;
    moduleLayoutDraft = sourceLayout;
    moduleDragFromCollapsedArrow = false;
    moduleCollapsedArrowOrigin = {};
    if (const auto* collapsed = moduleLayoutDraft.Find(id); collapsed && collapsed->collapsed &&
        collapsed->expandedWidth >= 0.10F && collapsed->expandedHeight >= 0.10F) {
        // Inside collapse expansion also returns target space. Bypassing model toggle
        // here restores source over target's collapsed bounds and causes an overlap.
        if (!moduleLayoutDraft.ToggleCollapsedModule(id)) return;
    }
    if (const auto* restored = moduleLayoutDraft.Find(id); restored && restored->collapseMode != ModuleCollapseMode::None) {
        if (auto* mutableRestored = moduleLayoutDraft.Find(id)) {
            mutableRestored->collapseMode = ModuleCollapseMode::None;
            mutableRestored->collapseSide = ModuleCollapseSide::None;
            mutableRestored->collapseTarget = id;
            mutableRestored->collapseTargetIsWindow = false;
        }
    }
    // The source-layout pointer may still refer to the collapsed handle. Use the
    // draft's restored rectangle for the drag offset so the first motion does not jump.
    const auto* item = moduleLayoutDraft.Find(geometryId);
    if (!item) return;
    // A tab may have retained its old standalone rectangle.  Seed the drag from
    // the rectangle the user can actually see so it does not jump or appear tiny
    // when it is detached from the group.
    if (geometryId != id) {
        if (auto* dragged = moduleLayoutDraft.Find(id)) {
            dragged->x = item->x;
            dragged->y = item->y;
            dragged->width = item->width;
            dragged->height = item->height;
        }
    }
    draggingModule = id;
    moduleGesture = ModuleGesture::Move;
    moduleDragActive = false;
    moduleDetachTabOnMove = detachTabOnMove;
    moduleMoveTabbedGroup = false;
    moduleMoveSnapGroup = !moduleLayoutDraft.IsTabbed(id) &&
                          moduleLayoutDraft.IsSnapped(id) &&
                          moduleLayoutDraft.IsSnapGrouped(id) &&
                          moduleLayoutDraft.SnapRoot(id) == id;
    moduleDragSnapRoot = moduleLayoutDraft.SnapRoot(id);
    moduleDragStart = {x, y};
    moduleDragOffset = {x - item->x * lastCanvas.width, y - item->y * lastCanvas.height};
    ResetModuleDropPreview();
    SetCapture(window);
}

void Win32Ui::Impl::UpdateModuleDrag(float x, float y) {
    if (!draggingModule || lastCanvas.width <= 0.0F || lastCanvas.height <= 0.0F) return;
    if (!moduleDragActive) {
        const float dx = x - moduleDragStart.x;
        const float dy = y - moduleDragStart.y;
        if (dx * dx + dy * dy < 25.0F) return;
        moduleDragActive = true;
    }
    const auto draggedId = *draggingModule;
    if (moduleGesture == ModuleGesture::Move) {
        const auto collapsedBeforeDrag = moduleLayoutDraft.Find(draggedId);
        const bool wasInsideCollapsed = collapsedBeforeDrag != nullptr &&
            collapsedBeforeDrag->collapsed &&
            collapsedBeforeDrag->collapseMode == ModuleCollapseMode::Inside &&
            !collapsedBeforeDrag->collapseTargetIsWindow;
        const ModuleId insideTarget = wasInsideCollapsed
            ? collapsedBeforeDrag->collapseTarget : draggedId;
        moduleLayoutDraft.ClearInsideCollapseReferences(draggedId);
        moduleLayoutDraft.ClearModuleCollapse(draggedId);
        if (wasInsideCollapsed) {
            // Restore target's full rectangle before detached source starts moving.
            if (const auto* source = moduleLayoutDraft.Find(draggedId)) {
                const ModuleNormalizedRect expanded{source->expandedX, source->expandedY,
                                                     source->expandedX + source->expandedWidth,
                                                     source->expandedY + source->expandedHeight};
                if (const auto* targetGeometry = moduleLayoutDraft.Find(moduleLayoutDraft.TabRoot(insideTarget))) {
                    const auto shared = ModuleNormalizedRect{
                        std::min(expanded.left, targetGeometry->x), std::min(expanded.top, targetGeometry->y),
                        std::max(expanded.right, targetGeometry->x + targetGeometry->width),
                        std::max(expanded.bottom, targetGeometry->y + targetGeometry->height)};
                    moduleLayoutDraft.SetTabGroupGeometry(moduleLayoutDraft.TabRoot(insideTarget), shared);
                }
            }
        }
    }
    if (moduleGesture == ModuleGesture::Move && moduleDetachTabOnMove &&
        moduleLayoutDraft.IsTabbed(draggedId)) {
        // Detach as soon as the movement threshold is crossed.  The detached panel
        // then follows the pointer during the drag, while a drop on another panel
        // can re-create the tab group in FinishModuleDrag.
        moduleLayoutDraft.DetachModuleFromTabs(draggedId);
    }
    if (moduleGesture == ModuleGesture::Move && !moduleDetachTabOnMove) {
        moduleMoveTabbedGroup = moduleLayoutDraft.IsTabbed(draggedId);
        moduleMoveSnapGroup = !moduleLayoutDraft.IsTabbed(draggedId) &&
                               moduleLayoutDraft.IsSnapped(draggedId) &&
                               moduleLayoutDraft.IsSnapGrouped(draggedId) &&
                               moduleLayoutDraft.SnapRoot(draggedId) == draggedId;
        moduleDragSnapRoot = moduleLayoutDraft.SnapRoot(draggedId);
    }
    if (moduleGesture == ModuleGesture::Move && moduleMoveTabbedGroup &&
        moduleLayoutDraft.IsSnapGrouped(draggedId)) {
        std::array<ModuleId, 6> tabMembers{};
        const auto tabRoot = moduleLayoutDraft.TabRoot(draggedId);
        const auto tabCount = moduleLayoutDraft.GroupTabCount(tabRoot);
        for (std::size_t index = 0; index < tabCount; ++index) {
            tabMembers[index] = moduleLayoutDraft.GroupMember(tabRoot, index);
        }
        moduleLayoutDraft.DetachSnapMembers(tabMembers, tabCount);
    }
    auto* item = moduleLayoutDraft.Find(draggedId);
    if (!item) return;
    if (moduleGesture == ModuleGesture::Resize) {
        (void)moduleLayoutDraft.ResizeModule(
            draggedId, x / lastCanvas.width, y / lastCanvas.height,
            moduleResizeRight, moduleResizeBottom, moduleResizeLeft, moduleResizeTop,
            model.moduleResizeBehavior == ModuleResizeBehavior::Squash);
    } else {
        const ModuleLayout beforeMove = moduleLayoutDraft;
        if (item->collapsed) {
            item->x = item->expandedX;
            item->y = item->expandedY;
            item->width = item->expandedWidth;
            item->height = item->expandedHeight;
            item->collapsed = false;
        }
        const float nextX = std::clamp((x - moduleDragOffset.x) / lastCanvas.width,
                                       0.0F, 1.0F - item->width);
        const float nextY = std::clamp((y - moduleDragOffset.y) / lastCanvas.height,
                                       0.0F, 1.0F - item->height);
        if (moduleMoveSnapGroup) {
            const float deltaX = nextX - item->x;
            const float deltaY = nextY - item->y;
            const auto tabRoot = moduleLayoutDraft.TabRoot(draggedId);
            for (auto& member : moduleLayoutDraft.items) {
                const bool inSnapGroup = moduleLayoutDraft.SnapRoot(member.id) == moduleDragSnapRoot;
                const bool inTabGroup = moduleLayoutDraft.IsTabbed(member.id) &&
                                        moduleLayoutDraft.TabRoot(member.id) == tabRoot;
                if (inSnapGroup || inTabGroup) {
                    member.x = std::clamp(member.x + deltaX, 0.0F, 1.0F - member.width);
                    member.y = std::clamp(member.y + deltaY, 0.0F, 1.0F - member.height);
                }
            }
        } else if (moduleMoveTabbedGroup) {
            const float deltaX = nextX - item->x;
            const float deltaY = nextY - item->y;
            const auto tabRoot = moduleLayoutDraft.TabRoot(draggedId);
            const auto tabCount = moduleLayoutDraft.GroupTabCount(tabRoot);
            for (std::size_t i = 0; i < tabCount; ++i) {
                if (auto* member = moduleLayoutDraft.Find(
                        moduleLayoutDraft.GroupMember(tabRoot, i))) {
                    member->x = std::clamp(member->x + deltaX, 0.0F, 1.0F - member->width);
                    member->y = std::clamp(member->y + deltaY, 0.0F, 1.0F - member->height);
                }
            }
        } else {
            // Picking up a standalone snapped member detaches it from its snap
            // group and leaves it floating until another side drop.
            if (moduleLayoutDraft.IsSnapped(draggedId)) {
                moduleLayoutDraft.DetachSnapModule(draggedId);
            }
            item = moduleLayoutDraft.Find(draggedId);
            if (!item) return;
            item->dockState = ModuleDockState::Floating;
            item->x = nextX;
            item->y = nextY;
        }
        if (!moduleLayoutDraft.ReattachOutsideCollapseHandles(beforeMove)) {
            moduleLayoutDraft = beforeMove;
        }
        ResolveModuleDropPreview(x, y);
    }
    InvalidateRect(window, nullptr, FALSE);
}

void Win32Ui::Impl::ResetModuleDropPreview() noexcept {
    moduleDropTarget.reset();
    moduleDropZone = ModuleDropZone::None;
    moduleWindowDropZone = ModuleWindowDropZone::None;
    moduleCollapseTarget.reset();
    moduleCollapseSide = ModuleCollapseSide::None;
    moduleCollapseMode = ModuleCollapseMode::None;
    moduleCollapseTargetIsWindow = false;
    moduleDropPreviewValid = false;
    moduleLayoutPreview = moduleLayoutDraft;
    moduleDropLastPointer = {-1.0F, -1.0F};
}

void Win32Ui::Impl::ResolveModuleDropPreview(float x, float y) {
    if (!draggingModule || moduleGesture != ModuleGesture::Move ||
        !moduleDragActive || lastCanvas.width <= 0.0F || lastCanvas.height <= 0.0F) {
        ResetModuleDropPreview();
        return;
    }

    std::optional<ModuleId> targetModule;
    ModuleDropZone targetZone = ModuleDropZone::None;
     const auto boundsFor = [this](const ModuleLayoutItem& item) {
         return ModulePixelBounds(item, lastCanvas);
     };
    // Resolve against the draft, rather than the last painted hit regions.  This
    // remains correct while the idle preview is showing a different layout.
    for (auto iterator = moduleLayoutDraft.items.rbegin();
         iterator != moduleLayoutDraft.items.rend(); ++iterator) {
        if (!iterator->visible || moduleLayoutDraft.IsEffectivelyCollapsed(iterator->id) ||
            iterator->id == *draggingModule) continue;
        if (moduleLayoutDraft.SnapRoot(iterator->id) ==
            moduleLayoutDraft.SnapRoot(*draggingModule)) continue;
        if (moduleLayoutDraft.IsTabbed(*draggingModule) &&
            moduleLayoutDraft.IsTabbed(iterator->id) &&
            moduleLayoutDraft.TabRoot(iterator->id) == moduleLayoutDraft.TabRoot(*draggingModule)) {
            continue;
        }
        if (moduleLayoutDraft.IsTabbed(iterator->id)) {
            const auto activeTab = moduleLayoutDraft.GroupActiveMember(iterator->id);
            if (activeTab != iterator->id) continue;
        }
        const auto* geometry = moduleLayoutDraft.Find(moduleLayoutDraft.TabRoot(iterator->id));
        if (!geometry) continue;
        const auto zone = ResolveModuleDropZone(x, y, boundsFor(*geometry).left,
                                                boundsFor(*geometry).top,
                                                boundsFor(*geometry).right,
                                                boundsFor(*geometry).bottom);
        if (zone != ModuleDropZone::None) {
            targetModule = iterator->id;
            targetZone = zone;
            break;
        }
    }

    ModuleWindowDropZone windowZone = ModuleWindowDropZone::None;
    std::optional<ModuleId> collapseTarget;
    ModuleCollapseSide collapseSide = ModuleCollapseSide::None;
    ModuleCollapseMode collapseMode = ModuleCollapseMode::None;
    bool collapseTargetIsWindow = false;
    ModuleLayout candidate = moduleLayoutDraft;
    bool previewCanApply = false;

    // These strips are intentionally narrow so existing side/corner snapping wins
    // everywhere except immediately beside an edge or module edge.
    const auto edgeSide = [&]() {
        if (x <= kModuleCollapseZonePixels) return ModuleCollapseSide::Left;
        if (x >= lastCanvas.width - kModuleCollapseZonePixels) return ModuleCollapseSide::Right;
        if (y <= kModuleCollapseZonePixels) return ModuleCollapseSide::Top;
        if (y >= lastCanvas.height - kModuleCollapseZonePixels) return ModuleCollapseSide::Bottom;
        return ModuleCollapseSide::None;
    };
    if (const auto side = edgeSide(); side != ModuleCollapseSide::None &&
        candidate.CollapseToWindow(*draggingModule, side,
            (side == ModuleCollapseSide::Left || side == ModuleCollapseSide::Right)
                ? y / lastCanvas.height : x / lastCanvas.width)) {
        targetModule.reset();
        targetZone = ModuleDropZone::None;
        windowZone = ModuleWindowDropZone::None;
        collapseSide = side;
        collapseMode = ModuleCollapseMode::Outside;
        collapseTargetIsWindow = true;
        previewCanApply = true;
    }

    const auto resolveCollapse = [this, x, y](const ModuleLayoutItem& item,
                                              ModuleCollapseSide& side,
                                              ModuleCollapseMode& mode) {
        const auto bounds = ModuleRawPixelBounds(item, lastCanvas);
        const float strip = kModuleCollapseZonePixels;
        const bool onVerticalEdge = y >= bounds.top && y <= bounds.bottom;
        const bool onHorizontalEdge = x >= bounds.left && x <= bounds.right;
        if (onVerticalEdge && x >= bounds.left - strip && x <= bounds.left + strip) {
            side = ModuleCollapseSide::Left;
            mode = x < bounds.left ? ModuleCollapseMode::Outside : ModuleCollapseMode::Inside;
            return true;
        }
        if (onVerticalEdge && x >= bounds.right - strip && x <= bounds.right + strip) {
            side = ModuleCollapseSide::Right;
            mode = x > bounds.right ? ModuleCollapseMode::Outside : ModuleCollapseMode::Inside;
            return true;
        }
        if (onHorizontalEdge && y >= bounds.top - strip && y <= bounds.top + strip) {
            side = ModuleCollapseSide::Top;
            mode = y < bounds.top ? ModuleCollapseMode::Outside : ModuleCollapseMode::Inside;
            return true;
        }
        if (onHorizontalEdge && y >= bounds.bottom - strip && y <= bounds.bottom + strip) {
            side = ModuleCollapseSide::Bottom;
            mode = y > bounds.bottom ? ModuleCollapseMode::Outside : ModuleCollapseMode::Inside;
            return true;
        }
        return false;
    };
    if (!previewCanApply) {
        for (auto iterator = moduleLayoutDraft.items.rbegin();
             iterator != moduleLayoutDraft.items.rend(); ++iterator) {
            if (!iterator->visible ||
                moduleLayoutDraft.IsEffectivelyCollapsed(iterator->id) ||
                iterator->id == *draggingModule) continue;
            if (moduleLayoutDraft.IsTabbed(iterator->id) &&
                moduleLayoutDraft.GroupActiveMember(iterator->id) != iterator->id) {
                continue;
            }
            if (moduleDragFromCollapsedArrow && Contains(moduleCollapsedArrowOrigin, x, y)) continue;
            if (moduleLayoutDraft.SnapRoot(iterator->id) ==
                moduleLayoutDraft.SnapRoot(*draggingModule)) continue;
            ModuleCollapseSide side = ModuleCollapseSide::None;
            ModuleCollapseMode mode = ModuleCollapseMode::None;
            if (!resolveCollapse(*iterator, side, mode)) continue;
            candidate = moduleLayoutDraft;
            if (!candidate.CollapseToModule(
                    *draggingModule, iterator->id, side, mode,
                    ModuleCollapseHandleTrackThickness(side, lastCanvas),
                    (side == ModuleCollapseSide::Left || side == ModuleCollapseSide::Right)
                        ? y / lastCanvas.height : x / lastCanvas.width)) {
                continue;
            }
            targetModule.reset();
            targetZone = ModuleDropZone::None;
            windowZone = ModuleWindowDropZone::None;
            collapseTarget = iterator->id;
            collapseSide = side;
            collapseMode = mode;
            previewCanApply = true;
            break;
        }
    }

    // Modules which touch a client edge render with only an 8-pixel visual gap,
    // while the first 12 pixels remain reserved for collapse handles.  Reserve a
    // small band immediately after that collapse strip for window drops so an
    // edge-adjacent module does not make the application target unreachable.
    constexpr float kModuleWindowDropBandPixels = 40.0F;
    const bool preferWindowDrop =
        (x >= kModuleCollapseZonePixels && x <= kModuleWindowDropBandPixels) ||
        (x >= lastCanvas.width - kModuleWindowDropBandPixels &&
         x <= lastCanvas.width - kModuleCollapseZonePixels) ||
        (y >= kModuleCollapseZonePixels && y <= kModuleWindowDropBandPixels) ||
        (y >= lastCanvas.height - kModuleWindowDropBandPixels &&
         y <= lastCanvas.height - kModuleCollapseZonePixels);
    if (!previewCanApply && preferWindowDrop) {
        windowZone = ResolveModuleWindowDropZone(
            x / lastCanvas.width, y / lastCanvas.height);
        candidate = moduleLayoutDraft;
        previewCanApply = candidate.SnapToWindow(*draggingModule, windowZone,
                                                 x / lastCanvas.width, y / lastCanvas.height);
        if (previewCanApply) {
            targetModule.reset();
            targetZone = ModuleDropZone::None;
        } else {
            windowZone = ModuleWindowDropZone::None;
        }
    }

    if (!previewCanApply) {
        if (targetModule) {
            if (targetZone == ModuleDropZone::Center) {
                candidate.TabWith(*draggingModule, *targetModule);
                previewCanApply = candidate.IsTabbed(*draggingModule);
            } else {
                previewCanApply = candidate.SnapTo(*draggingModule, *targetModule, targetZone);
            }
            // A module can be visually close to a target whose split is already too
            // small. In that case fall through to the window target below instead of
            // showing a drop preview that cannot be committed.
            if (!previewCanApply) {
                targetModule.reset();
                targetZone = ModuleDropZone::None;
            }
        }
        if (!targetModule) {
            windowZone = ResolveModuleWindowDropZone(
                x / lastCanvas.width, y / lastCanvas.height);
            if (windowZone != ModuleWindowDropZone::None) {
                candidate = moduleLayoutDraft;
                previewCanApply = candidate.SnapToWindow(*draggingModule, windowZone,
                                                         x / lastCanvas.width, y / lastCanvas.height);
                if (!previewCanApply) windowZone = ModuleWindowDropZone::None;
            }
        }
    }

    moduleDropLastPointer = {x, y};
    if (!targetModule && windowZone == ModuleWindowDropZone::None && !collapseTarget &&
        collapseMode == ModuleCollapseMode::None && !collapseTargetIsWindow) {
        ResetModuleDropPreview();
        return;
    }
    moduleDropTarget = targetModule;
    moduleDropZone = targetZone;
    moduleWindowDropZone = windowZone;
    moduleCollapseTarget = collapseTarget;
    moduleCollapseSide = collapseSide;
    moduleCollapseMode = collapseMode;
    moduleCollapseTargetIsWindow = collapseTargetIsWindow;
    moduleLayoutPreview = candidate;
    moduleDropPreviewValid = previewCanApply;
}

void Win32Ui::Impl::FinishModuleDrag() noexcept {
    const auto dragged = draggingModule;
    const bool active = moduleDragActive;
    const bool moving = moduleGesture == ModuleGesture::Move;
    const auto drop = moduleDropTarget;
    const auto zone = moduleDropZone;
    const auto windowDrop = moduleWindowDropZone;
    const auto collapseDrop = moduleCollapseTarget;
    const auto collapseSideDrop = moduleCollapseSide;
    const auto collapseModeDrop = moduleCollapseMode;
    const bool collapseWindowDrop = moduleCollapseTargetIsWindow;
    const bool previewValid = moduleDropPreviewValid;
    const bool detachTabOnMove = moduleDetachTabOnMove;
    const bool moveTabbedGroup = moduleMoveTabbedGroup;
    const bool moveSnapGroup = moduleMoveSnapGroup;
    moduleGesture = ModuleGesture::None;
    moduleDragActive = false;
    draggingModule.reset();
    moduleDropTarget.reset();
    moduleDropZone = ModuleDropZone::None;
    moduleWindowDropZone = ModuleWindowDropZone::None;
    moduleCollapseTarget.reset();
    moduleCollapseSide = ModuleCollapseSide::None;
    moduleCollapseMode = ModuleCollapseMode::None;
    moduleCollapseTargetIsWindow = false;
    moduleDragFromCollapsedArrow = false;
    moduleCollapsedArrowOrigin = {};
    moduleDropPreviewValid = false;
    moduleResizeRight = false;
    moduleResizeBottom = false;
    moduleResizeLeft = false;
    moduleResizeTop = false;
    moduleDetachTabOnMove = false;
    moduleMoveTabbedGroup = false;
    moduleMoveSnapGroup = false;
    if (dragged && active) {
        if (moving && ((drop && *drop != *dragged && zone != ModuleDropZone::None) ||
                      windowDrop != ModuleWindowDropZone::None ||
                      collapseModeDrop != ModuleCollapseMode::None || collapseWindowDrop)) {
            if (previewValid && (windowDrop != ModuleWindowDropZone::None ||
                                 zone != ModuleDropZone::None ||
                                 collapseModeDrop != ModuleCollapseMode::None || collapseWindowDrop)) {
                moduleLayoutDraft = moduleLayoutPreview;
            } else if (collapseModeDrop != ModuleCollapseMode::None) {
                if (collapseWindowDrop) {
                    (void)moduleLayoutDraft.CollapseToWindow(*dragged, collapseSideDrop,
                        (collapseSideDrop == ModuleCollapseSide::Left ||
                         collapseSideDrop == ModuleCollapseSide::Right)
                            ? moduleDropLastPointer.y / lastCanvas.height
                            : moduleDropLastPointer.x / lastCanvas.width);
                } else if (collapseDrop) {
                    (void)moduleLayoutDraft.CollapseToModule(*dragged, *collapseDrop,
                                                                collapseSideDrop, collapseModeDrop,
                                                                ModuleCollapseHandleTrackThickness(
                                                                    collapseSideDrop, lastCanvas),
                                                                (collapseSideDrop == ModuleCollapseSide::Left ||
                                                                 collapseSideDrop == ModuleCollapseSide::Right)
                                                                    ? moduleDropLastPointer.y / lastCanvas.height
                                                                    : moduleDropLastPointer.x / lastCanvas.width);
                }
            } else if (zone == ModuleDropZone::Center) {
                moduleLayoutDraft.TabWith(*dragged, *drop);
            } else if (windowDrop != ModuleWindowDropZone::None) {
                (void)moduleLayoutDraft.SnapToWindow(
                    *dragged, windowDrop, moduleDropLastPointer.x / lastCanvas.width,
                    moduleDropLastPointer.y / lastCanvas.height);
            } else {
                (void)moduleLayoutDraft.SnapTo(*dragged, *drop, zone);
            }
        } else if (moving) {
            if (detachTabOnMove || (!moveTabbedGroup && !moveSnapGroup)) {
                moduleLayoutDraft.DetachModuleFromTabs(*dragged);
            }
            if (auto* item = moduleLayoutDraft.Find(*dragged)) {
                if (detachTabOnMove || (!moveTabbedGroup && !moveSnapGroup)) {
                    item->dockState = ModuleDockState::Floating;
                }
            }
        }
        try { host.SetModuleLayout(moduleLayoutDraft); } catch (...) {}
    }
    if (GetCapture() == window) ReleaseCapture();
    InvalidateRect(window, nullptr, FALSE);
}

[[nodiscard]] HCURSOR Win32Ui::Impl::ModuleCursor(float x, float y) const noexcept {
    if (windowKind != WindowKind::Main || model.miniPlayer) return nullptr;
    constexpr float edge = 7.0F;
    for (auto iterator = moduleRegions.rbegin(); iterator != moduleRegions.rend(); ++iterator) {
        if (!Contains(iterator->bounds, x, y)) continue;
        const bool right = std::abs(x - iterator->bounds.right) <= edge;
        const bool bottom = std::abs(y - iterator->bounds.bottom) <= edge;
        const bool left = std::abs(x - iterator->bounds.left) <= edge;
        const bool top = std::abs(y - iterator->bounds.top) <= edge;
        if (right && bottom) return LoadCursorW(nullptr, IDC_SIZENWSE);
        if (left && top) return LoadCursorW(nullptr, IDC_SIZENWSE);
        if ((right && top) || (left && bottom)) return LoadCursorW(nullptr, IDC_SIZENESW);
        if (right) return LoadCursorW(nullptr, IDC_SIZEWE);
        if (left) return LoadCursorW(nullptr, IDC_SIZEWE);
        if (bottom) return LoadCursorW(nullptr, IDC_SIZENS);
        if (top) return LoadCursorW(nullptr, IDC_SIZENS);
    }
    if (moduleGesture == ModuleGesture::Move) return LoadCursorW(nullptr, IDC_SIZEALL);
    return nullptr;
}

} // namespace rivan::ui
