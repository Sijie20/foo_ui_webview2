#include "pch.h"
#include "window/ViewportResizePolicy.h"

namespace {
using fullscreen_geometry::Rect;

void ExpectRect(Rect actual, Rect expected) {
    EXPECT_EQ(actual.left, expected.left);
    EXPECT_EQ(actual.top, expected.top);
    EXPECT_EQ(actual.right, expected.right);
    EXPECT_EQ(actual.bottom, expected.bottom);
}

// A fake resize sink consumes the same geometry and scheduling policy as
// MainWindow. No HWND, running testbed, WebView process or CDP port is needed.
struct Viewport {
    viewport_resize::Schedule schedule;
    Rect client{};
    Rect lastResizeBounds{};
    unsigned dpi = 144;
    unsigned commits = 0;

    void Sync(bool interactive, bool force, double now) {
        if (schedule.Request(interactive, force, now, 8.0)) return;
        lastResizeBounds = client;
        ++commits;
    }
};
}

TEST(ViewportResize, ThreeMaximizeSetWindowPosRestoreCyclesKeepPhysicalClientSize) {
    const Rect work{0, 0, 3840, 2120};
    const Rect maximized{-12, -12, 3852, 2132};
    const Rect smallAtMaximizedOrigin{-12, -12, 1268, 708};
    const Rect restored{100, 100, 1380, 820};
    Viewport viewport;
    for (int cycle = 0; cycle != 3; ++cycle) {
        for (const auto proposed : {maximized, smallAtMaximizedOrigin, restored, maximized}) {
            // Deliberately keep the zoomed flag true for the SetWindowPos case.
            const auto client = viewport_resize::MaximizedClientRect(proposed, work);
            const bool large = proposed.right == maximized.right;
            ExpectRect(client, large ? work : proposed);
            viewport.client = {0, 0, client.right - client.left, client.bottom - client.top};
            viewport.Sync(false, false, cycle * 10.0);
            const Rect expected = large ? Rect{0, 0, 3840, 2120} : Rect{0, 0, 1280, 720};
            ExpectRect(viewport.lastResizeBounds, expected);
            ExpectRect(fullscreen_geometry::ControllerBoundsFromPhysical(
                viewport.lastResizeBounds, viewport.dpi), expected);
        }
    }
    EXPECT_EQ(viewport.commits, 12u);
}

TEST(ViewportResize, ZoomedPartialWidthOrHeightNeverExpandsToWorkArea) {
    const Rect work{-3840, -200, 0, 1920};
    for (const Rect proposed : {Rect{-3852, -212, -2572, 1932},
                                Rect{-3852, -212, 12, 508},
                                Rect{-3800, -160, -2520, 560},
                                Rect{100, 100, 1380, 820}}) {
        ExpectRect(viewport_resize::MaximizedClientRect(proposed, work), proposed);
    }
    ExpectRect(viewport_resize::MaximizedClientRect({-3852, -212, 12, 1932}, work), work);
}

TEST(ViewportResize, InteractiveTrailingFlushUsesLatestClientInsteadOfQueuedRectangle) {
    Viewport viewport;
    viewport.client = {0, 0, 1920, 1080};
    viewport.Sync(true, false, 100);
    viewport.client = {0, 0, 1800, 1000};
    viewport.Sync(true, false, 102);
    ASSERT_TRUE(viewport.schedule.HasPending());
    EXPECT_EQ(viewport.commits, 1u);
    // A later SetWindowPos can change GetClientRect before the trailing timer.
    viewport.client = {0, 0, 1280, 720};
    viewport.Sync(true, true, 103);
    EXPECT_FALSE(viewport.schedule.HasPending());
    ExpectRect(viewport.lastResizeBounds, {0, 0, 1280, 720});
    EXPECT_EQ(viewport.commits, 2u);
}

TEST(ViewportResize, ProgrammaticResizeAndExitCancelInteractiveTailImmediately) {
    for (const bool interactive : {false, true}) {
        Viewport viewport;
        viewport.client = {0, 0, 3840, 2120};
        viewport.Sync(true, false, 100);
        viewport.client = {0, 0, 3000, 1800};
        viewport.Sync(true, false, 101);
        ASSERT_TRUE(viewport.schedule.HasPending());
        viewport.client = {0, 0, 1280, 720};
        viewport.Sync(interactive, interactive, 102);
        EXPECT_FALSE(viewport.schedule.HasPending());
        ExpectRect(viewport.lastResizeBounds, {0, 0, 1280, 720});
        // A subsequent forced exit sync also works without any pending tail.
        viewport.client = {0, 0, 1300, 740};
        viewport.Sync(false, true, 103);
        ExpectRect(viewport.lastResizeBounds, {0, 0, 1300, 740});
    }
}

TEST(ViewportResize, DpiChangeBypassesThrottleWithIdenticalPhysicalSize) {
    Viewport viewport;
    viewport.client = {0, 0, 1280, 720};
    viewport.Sync(true, false, 100);
    viewport.Sync(true, false, 101);
    ASSERT_TRUE(viewport.schedule.HasPending());
    for (const unsigned dpi : {96u, 120u, 144u, 192u, 144u}) {
        viewport.dpi = dpi;
        viewport.Sync(true, true, 102);
        EXPECT_FALSE(viewport.schedule.HasPending());
        ExpectRect(fullscreen_geometry::ControllerBoundsFromPhysical(
            viewport.lastResizeBounds, viewport.dpi), {0, 0, 1280, 720});
    }
    EXPECT_EQ(viewport.commits, 6u);
}

TEST(ViewportResize, CoalescesBurstButCommitsAtIntervalAndAfterResetOrClockFailure) {
    viewport_resize::Schedule schedule;
    EXPECT_FALSE(schedule.Request(true, false, 100, 8));
    EXPECT_TRUE(schedule.Request(true, false, 101, 8));
    EXPECT_TRUE(schedule.Request(true, false, 107, 8));
    EXPECT_FALSE(schedule.Request(true, false, 108, 8));
    EXPECT_FALSE(schedule.HasPending());
    EXPECT_TRUE(schedule.Request(true, false, 109, 8));
    schedule.Reset(); // minimized/new interactive loop cannot retain an old tail
    EXPECT_FALSE(schedule.HasPending());
    EXPECT_FALSE(schedule.Request(true, false, 110, 8));
    EXPECT_FALSE(schedule.Request(true, false, -1, 8));
    EXPECT_FALSE(schedule.Request(true, false, 111, 8));
    EXPECT_FALSE(schedule.Request(true, false, 100, 8));
}
