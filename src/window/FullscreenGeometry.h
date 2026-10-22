#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <limits>

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

// RAW_PIXELS is intentional: measured at 144 DPI, 900 physical px =>
// innerWidth=600,dpr=1.5. Dividing the bounds again gives innerWidth=400.
inline Rect ControllerBoundsFromPhysical(Rect physical, unsigned /*dpi*/) { return physical; }
} // namespace fullscreen_geometry
