#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <limits>
#include <cmath>

namespace fullscreen_geometry {
struct Rect { int left, top, right, bottom; };

// All coordinates are desktop physical pixels (including negative origins).
inline Rect ClampToWorkArea(Rect window, Rect work) {
    const int width = std::min(std::max(1, window.right - window.left), std::max(1, work.right - work.left));
    const int height = std::min(std::max(1, window.bottom - window.top), std::max(1, work.bottom - work.top));
    const int left = std::clamp(window.left, work.left, work.right - width);
    const int top = std::clamp(window.top, work.top, work.bottom - height);
    return {left, top, left + width, top + height};
}

inline Rect RecoverToVisibleArea(Rect window, const Rect* areas, std::size_t count, bool clampPartial) {
    std::size_t nearest = 0;
    std::int64_t bestArea = -1, bestDistance = std::numeric_limits<std::int64_t>::max();
    for (std::size_t i = 0; i < count; ++i) {
        const Rect work = areas[i];
        const auto width = std::max(0, std::min(window.right, work.right) - std::max(window.left, work.left));
        const auto height = std::max(0, std::min(window.bottom, work.bottom) - std::max(window.top, work.top));
        const auto overlap = static_cast<std::int64_t>(width) * height;
        if (!clampPartial && overlap > 0) return window; // exact restore with surviving display
        const auto dx = static_cast<std::int64_t>(std::max({work.left-window.right, window.left-work.right, 0}));
        const auto dy = static_cast<std::int64_t>(std::max({work.top-window.bottom, window.top-work.bottom, 0}));
        const auto distance = dx*dx + dy*dy;
        if (overlap > bestArea || (overlap == bestArea && distance < bestDistance)) {
            nearest = i; bestArea = overlap; bestDistance = distance;
        }
    }
    return count ? ClampToWorkArea(window, areas[nearest]) : window;
}

enum class BoundsMode { RawPixels, RasterizationScale };

// Live composition matrix at 144 DPI, scale=1.5, zoom=1:
// RAW 1280/853/1920 => innerWidth 854/569/1280; scaled mode => 1280/853/1920.
// DComp clips always remain physical, even when controller bounds use DIPs.
inline Rect ControllerBoundsForScale(Rect physical, double scale, BoundsMode mode) {
    if (mode == BoundsMode::RawPixels) return physical;
    if (!(scale > 0) || !std::isfinite(scale)) scale = 1;
    const auto dip = [scale](int value) { return static_cast<int>(std::lround(value / scale)); };
    return {dip(physical.left), dip(physical.top), dip(physical.right), dip(physical.bottom)};
}
inline Rect ControllerBoundsFromPhysical(Rect physical, unsigned dpi,
                                        BoundsMode mode = BoundsMode::RawPixels) {
    return ControllerBoundsForScale(physical, (dpi ? dpi : 96) / 96.0, mode);
}
} // namespace fullscreen_geometry
