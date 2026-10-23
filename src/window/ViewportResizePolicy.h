#pragma once

#include "window/FullscreenGeometry.h"

namespace viewport_resize {

// SetWindowPos does not clear WS_MAXIMIZE. Only trim the off-screen frame
// when the proposed window really covers the work area; never expand a small
// proposed client back to monitor size just because IsZoomed is still true.
inline fullscreen_geometry::Rect MaximizedClientRect(
    fullscreen_geometry::Rect proposed, fullscreen_geometry::Rect work) {
    if (work.right > work.left && work.bottom > work.top &&
        proposed.left <= work.left && proposed.top <= work.top &&
        proposed.right >= work.right && proposed.bottom >= work.bottom) {
        return work;
    }
    return proposed;
}

// The trailing flush deliberately stores no rectangle: the HWND's current
// GetClientRect is authoritative, even if a newer programmatic resize or DPI
// change overtakes a queued interactive resize.
class Schedule {
public:
    bool Request(bool interactive, bool force, double nowMs, double minIntervalMs) {
        if (!interactive || force || nowMs < 0 || lastCommitMs_ < 0 ||
            nowMs < lastCommitMs_ || nowMs - lastCommitMs_ >= minIntervalMs) {
            pending_ = false;
            lastCommitMs_ = nowMs;
            return false;
        }
        pending_ = true;
        return true;
    }

    bool HasPending() const { return pending_; }
    void Reset() { pending_ = false; lastCommitMs_ = -1; }

private:
    bool pending_ = false;
    double lastCommitMs_ = -1;
};

} // namespace viewport_resize
