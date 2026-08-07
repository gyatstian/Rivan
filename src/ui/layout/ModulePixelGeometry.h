// ModulePixelGeometry.h
// Shared Direct2D geometry for module input and rendering.
#pragma once

#include "ModuleLayout.h"

#include <d2d1.h>

namespace rivan::ui {

inline constexpr float kModuleWindowGap = 8.0F;
inline constexpr float kModuleCollapseZonePixels = 12.0F;
inline constexpr float kModuleCollapseHandleWidthFraction = 0.20F;
inline constexpr float kModuleCollapseHandleHeight = 18.0F;
inline constexpr float kCollapsedHandleDragThreshold = 8.0F;

[[nodiscard]] float ModuleCollapseHandleTrackThickness(ModuleCollapseSide side,
                                                        D2D1_SIZE_F size) noexcept;
[[nodiscard]] D2D1_RECT_F ModuleRawPixelBounds(const ModuleLayoutItem& item,
                                                D2D1_SIZE_F size) noexcept;
[[nodiscard]] D2D1_RECT_F ModulePixelBounds(const ModuleLayoutItem& item,
                                             D2D1_SIZE_F size) noexcept;
[[nodiscard]] D2D1_RECT_F ModuleCollapseHandleBounds(const ModuleLayoutItem& item,
                                                      D2D1_SIZE_F size) noexcept;
[[nodiscard]] const wchar_t* ModuleCollapseArrow(ModuleCollapseSide side) noexcept;
[[nodiscard]] const wchar_t* ModuleCollapseArrow(const ModuleLayoutItem& item) noexcept;

} // namespace rivan::ui
