// SongRowLayoutGeometry.h
// Pixel-space helpers for editing normalized song-row geometry.
#pragma once

#include "SongRowLayout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace rivan::ui {

inline constexpr float kSongRowSnapDistancePixels = 8.0F;
inline constexpr float kSongRowFieldHorizontalInsetPixels = 1.0F;
inline constexpr float kSongRowFluidTextSafetyPixels = 1.0F;

// Runtime-only adjustments. They never alter persisted user geometry.
struct SongRowTransientLayout final {
    std::optional<float> numberMinimumX;
};

[[nodiscard]] inline float SongRowFluidFieldWidth(
    const float textWidthPixels, const float canvasWidth,
    const float horizontalInsetPixels = kSongRowFieldHorizontalInsetPixels,
    const float textSafetyPixels = kSongRowFluidTextSafetyPixels) noexcept {
    if (canvasWidth <= 0.0F || !std::isfinite(canvasWidth)) return 0.0F;
    const float measuredTextWidth = std::isfinite(textWidthPixels)
        ? std::max(0.0F, textWidthPixels) : 0.0F;
    const float inset = std::max(0.0F, horizontalInsetPixels);
    const float safety = std::max(0.0F, textSafetyPixels);
    return std::clamp((measuredTextWidth + 2.0F * (inset + safety)) / canvasWidth,
                      0.0F, 1.0F);
}

[[nodiscard]] inline std::optional<SongRowSnapSide> SongRowSnapHoverSide(
    const float pointerX,
    const float pointerY,
    const float targetVisualLeft,
    const float targetVisualRight,
    const float targetVisualTop,
    const float targetVisualBottom,
    const float thresholdPixels = kSongRowSnapDistancePixels) noexcept {
    if (pointerY < targetVisualTop || pointerY > targetVisualBottom) return std::nullopt;
    const float leftDistance = std::abs(pointerX - targetVisualLeft);
    const float rightDistance = std::abs(pointerX - targetVisualRight);
    if (std::min(leftDistance, rightDistance) > thresholdPixels) return std::nullopt;
    return leftDistance <= rightDistance ? SongRowSnapSide::Left : SongRowSnapSide::Right;
}

// Returns normalized x for a field whose rendered edge sits gapPixels from the
// corresponding rendered edge of target. Left means selected field precedes target;
// Right means selected field follows target.
[[nodiscard]] inline float SongRowSnappedFieldX(
    const float targetVisualLeft,
    const float targetVisualRight,
    const float canvasLeft,
    const float canvasWidth,
    const float fieldWidthNormalized,
    const float gapPixels,
    const SongRowSnapSide side,
    const float horizontalInsetPixels = kSongRowFieldHorizontalInsetPixels) noexcept {
    if (canvasWidth <= 0.0F) return 0.0F;
    const float fieldWidthPixels = fieldWidthNormalized * canvasWidth;
    const float renderedWidthPixels = std::max(0.0F, fieldWidthPixels - 2.0F * horizontalInsetPixels);
    const float renderedLeft = side == SongRowSnapSide::Left
        ? targetVisualLeft - gapPixels - renderedWidthPixels
        : targetVisualRight + gapPixels;
    return (renderedLeft - horizontalInsetPixels - canvasLeft) / canvasWidth;
}

// Keeps an independently positioned cover from painting over a fluid track number.
// The correction is render-time only: users retain their saved Cover coordinate, and
// fields attached to Cover are resolved again from its corrected position.
[[nodiscard]] inline float SongRowCoverXAfterNumberClearance(
    const float numberX, const float coverX, const float canvasLeft, const float canvasWidth,
    const float numberWidthNormalized, const float coverWidthNormalized) noexcept {
    const float numberWidth = std::isfinite(numberWidthNormalized)
        ? std::clamp(numberWidthNormalized, 0.0F, 1.0F) : 0.0F;
    const float coverWidth = std::isfinite(coverWidthNormalized)
        ? std::clamp(coverWidthNormalized, 0.0F, 1.0F) : 0.0F;
    const float boundedCoverX = std::clamp(
        std::isfinite(coverX) ? coverX : 0.0F, 0.0F, 1.0F - coverWidth);
    if (canvasWidth <= 0.0F || !std::isfinite(canvasWidth)) return boundedCoverX;

    const float boundedNumberX = std::clamp(
        std::isfinite(numberX) ? numberX : 0.0F, 0.0F, 1.0F - numberWidth);
    const float numberVisualLeft = canvasLeft + boundedNumberX * canvasWidth +
        kSongRowFieldHorizontalInsetPixels;
    const float numberVisualRight = canvasLeft + (boundedNumberX + numberWidth) * canvasWidth -
        kSongRowFieldHorizontalInsetPixels;
    const float coverVisualLeft = canvasLeft + boundedCoverX * canvasWidth +
        kSongRowFieldHorizontalInsetPixels;
    if (coverVisualLeft < numberVisualLeft) return boundedCoverX;

    const float requiredCoverX = SongRowSnappedFieldX(
        numberVisualLeft, numberVisualRight, canvasLeft, canvasWidth, coverWidth,
        static_cast<float>(kSongRowDefaultSnapGapPixels), SongRowSnapSide::Right);
    return std::clamp(std::max(boundedCoverX, requiredCoverX), 0.0F, 1.0F - coverWidth);
}

[[nodiscard]] inline bool SongRowSnapIsValid(
    const SongRowLayout& layout, const SongRowField source) noexcept {
    const auto sourceIndex = static_cast<std::size_t>(source);
    if (sourceIndex >= kSongRowFieldCount) return false;
    const auto& snap = layout.Field(source).snap;
    if (!snap) return true;
    const auto targetIndex = static_cast<std::size_t>(snap->target);
    return targetIndex < kSongRowFieldCount && snap->target != source &&
           layout.Field(snap->target).visible &&
           snap->gapPixels >= kSongRowMinimumSnapGapPixels &&
           snap->gapPixels <= kSongRowMaximumSnapGapPixels;
}

[[nodiscard]] inline bool SongRowHasSnapCycle(const SongRowLayout& layout) noexcept {
    enum class VisitState : std::uint8_t { Unvisited, Visiting, Complete };
    std::array<VisitState, kSongRowFieldCount> states{};
    const auto visitsCycle = [&layout, &states](auto&& self, const SongRowField source) noexcept -> bool {
        const auto sourceIndex = static_cast<std::size_t>(source);
        if (states[sourceIndex] == VisitState::Visiting) return true;
        if (states[sourceIndex] == VisitState::Complete) return false;
        states[sourceIndex] = VisitState::Visiting;
        const auto& snap = layout.Field(source).snap;
        const bool cycle = snap && SongRowSnapIsValid(layout, source) &&
            self(self, snap->target);
        states[sourceIndex] = VisitState::Complete;
        return cycle;
    };
    for (std::size_t index = 0; index < kSongRowFieldCount; ++index) {
        if (visitsCycle(visitsCycle, static_cast<SongRowField>(index))) return true;
    }
    return false;
}

// Resolves durable attachment relationships in pixel space. Field widths are
// supplied by the renderer because fluid text widths depend on the row content.
// Invalid or cyclic attachments deliberately fall back to stored x so malformed
// settings can never make drawing recursive or undefined.
[[nodiscard]] inline std::array<float, kSongRowFieldCount> SongRowResolvedFieldXs(
    const SongRowLayout& layout, const float canvasLeft, const float canvasWidth,
    const std::array<float, kSongRowFieldCount>& fieldWidths,
    const SongRowTransientLayout& transient = {}) noexcept {
    std::array<float, kSongRowFieldCount> result{};
    if (SongRowHasSnapCycle(layout)) {
        for (std::size_t index = 0; index < kSongRowFieldCount; ++index) {
            const float width = std::isfinite(fieldWidths[index])
                ? std::clamp(fieldWidths[index], 0.0F, 1.0F) : 0.0F;
            const float x = std::isfinite(layout.fields[index].x) ? layout.fields[index].x : 0.0F;
            result[index] = std::clamp(x, 0.0F, 1.0F - width);
        }
        return result;
    }
    const auto widthFor = [&fieldWidths](const std::size_t index) noexcept {
        const float width = fieldWidths[index];
        return std::isfinite(width) ? std::clamp(width, 0.0F, 1.0F) : 0.0F;
    };
    const auto baseXFor = [&layout, &widthFor](const std::size_t index) noexcept {
        const float x = layout.fields[index].x;
        const float boundedX = std::isfinite(x) ? x : 0.0F;
        return std::clamp(boundedX, 0.0F, 1.0F - widthFor(index));
    };
    const auto resolvePass = [&layout, canvasLeft, canvasWidth, &widthFor, &baseXFor, &transient](
                                 const std::optional<float> correctedCoverX) noexcept {
        std::array<float, kSongRowFieldCount> resolved{};
        enum class VisitState : std::uint8_t { Unvisited, Visiting, Complete };
        std::array<VisitState, kSongRowFieldCount> states{};
        const auto resolve = [&layout, canvasLeft, canvasWidth, &widthFor, &baseXFor,
                              &transient, correctedCoverX, &resolved, &states](
                                 auto&& self, const SongRowField field) noexcept -> float {
            const auto index = static_cast<std::size_t>(field);
            const bool correctedCover = field == SongRowField::Cover && correctedCoverX;
            const float baseX = correctedCover
                ? std::clamp(*correctedCoverX, 0.0F, 1.0F - widthFor(index))
                : baseXFor(index);
            if (states[index] == VisitState::Complete) return resolved[index];
            if (states[index] == VisitState::Visiting) return baseX;
            states[index] = VisitState::Visiting;

            float resolvedX = baseX;
            const auto& snap = layout.fields[index].snap;
            if (canvasWidth > 0.0F && snap && SongRowSnapIsValid(layout, field)) {
                const float targetX = self(self, snap->target);
                const auto targetIndex = static_cast<std::size_t>(snap->target);
                const float targetWidth = widthFor(targetIndex);
                const float targetVisualLeft = canvasLeft + targetX * canvasWidth +
                    kSongRowFieldHorizontalInsetPixels;
                const float targetVisualRight = canvasLeft + (targetX + targetWidth) * canvasWidth -
                    kSongRowFieldHorizontalInsetPixels;
                resolvedX = std::clamp(SongRowSnappedFieldX(
                    targetVisualLeft, targetVisualRight, canvasLeft, canvasWidth, widthFor(index),
                    static_cast<float>(snap->gapPixels), snap->side),
                    0.0F, 1.0F - widthFor(index));
            }
            if (field == SongRowField::Number && transient.numberMinimumX) {
                resolvedX = std::clamp(std::max(resolvedX, *transient.numberMinimumX),
                                       0.0F, 1.0F - widthFor(index));
            }
            resolved[index] = resolvedX;
            states[index] = VisitState::Complete;
            return resolvedX;
        };
        for (std::size_t index = 0; index < kSongRowFieldCount; ++index) {
            (void)resolve(resolve, static_cast<SongRowField>(index));
        }
        return resolved;
    };

    result = resolvePass(std::nullopt);
    constexpr std::size_t numberIndex = static_cast<std::size_t>(SongRowField::Number);
    constexpr std::size_t coverIndex = static_cast<std::size_t>(SongRowField::Cover);
    const auto& number = layout.Field(SongRowField::Number);
    const auto& cover = layout.Field(SongRowField::Cover);
    if (!number.visible || !number.fluid || !cover.visible || cover.snap ||
        result[coverIndex] < result[numberIndex]) {
        return result;
    }
    const float correctedCoverX = SongRowCoverXAfterNumberClearance(
        result[numberIndex], result[coverIndex], canvasLeft, canvasWidth,
        widthFor(numberIndex), widthFor(coverIndex));
    if (correctedCoverX > result[coverIndex]) result = resolvePass(correctedCoverX);
    return result;
}

} // namespace rivan::ui
