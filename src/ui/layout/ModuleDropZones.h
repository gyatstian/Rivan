// ModuleDropZones.h
// Pure mapping between pointer coordinates and module drop targets.
#pragma once

#include "ModuleTypes.h"

namespace rivan::ui {

[[nodiscard]] bool IsWindowDrop(ModuleWindowDropZone zone) noexcept;
[[nodiscard]] ModuleWindowDropZone ResolveModuleWindowDropZone(float x, float y) noexcept;
[[nodiscard]] ModuleNormalizedRect ModuleWindowDropBounds(ModuleWindowDropZone zone) noexcept;
[[nodiscard]] bool IsSideDrop(ModuleDropZone zone) noexcept;
[[nodiscard]] ModuleDropZone ResolveModuleDropZone(float x, float y,
                                                    float left, float top,
                                                    float right, float bottom) noexcept;

} // namespace rivan::ui
