#include "pch.h"
#include "../src/window/FullscreenGeometry.h"
#include "../src/window/WindowGeometryMath.h"

namespace {
using fullscreen_geometry::Rect;
void ExpectRect(Rect actual, Rect expected) {
    EXPECT_EQ(actual.left, expected.left); EXPECT_EQ(actual.top, expected.top);
    EXPECT_EQ(actual.right, expected.right); EXPECT_EQ(actual.bottom, expected.bottom);
}
}

TEST(FullscreenGeometry, PreservesOriginalMonitorAndRectangleWhenDisplaysSurvive) {
    const Rect work[] = {{-1920,-100,0,980},{0,0,2560,1400},{2560,200,4480,1240}};
    for (const Rect window : {Rect{-1700,20,-600,800}, Rect{300,200,1800,1000}, Rect{2800,300,4100,1100}})
        ExpectRect(fullscreen_geometry::RecoverToVisibleArea(window,work,3,false),window);
    // A deliberately spanning original window is also restored exactly.
    const Rect spanning{-500,100,800,800};
    ExpectRect(fullscreen_geometry::RecoverToVisibleArea(spanning,work,3,false),spanning);
}

TEST(FullscreenGeometry, RemovedRightMonitorClampsToNearestSurvivingWorkArea) {
    const Rect work[] = {{-1920,0,0,1040},{0,0,1920,1040}};
    ExpectRect(fullscreen_geometry::RecoverToVisibleArea({2800,200,4000,1000},work,2,true),{720,200,1920,1000});
}

TEST(FullscreenGeometry, NegativeCoordinateMonitorIsNeverClampedToPrimaryByAccident) {
    const Rect work[] = {{-1920,-1080,0,-40},{0,0,1920,1040}};
    ExpectRect(fullscreen_geometry::RecoverToVisibleArea({-2400,-900,-1400,-100},work,2,true),{-1920,-900,-920,-100});
}

TEST(FullscreenGeometry, ChoosesLargestIntersectionBeforeNearestDistance) {
    const Rect work[] = {{-1920,0,0,1040},{0,0,1920,1040}};
    ExpectRect(fullscreen_geometry::RecoverToVisibleArea({-100,100,900,900},work,2,true),{0,100,1000,900});
}

TEST(FullscreenGeometry, ResolutionShrinkKeepsCloseCornerAndWholeWindowVisible) {
    ExpectRect(fullscreen_geometry::ClampToWorkArea({1800,900,3500,2000},{0,0,1280,680}),{0,0,1280,680});
    ExpectRect(fullscreen_geometry::ClampToWorkArea({1000,600,1500,900},{0,0,1280,680}),{780,380,1280,680});
}

TEST(FullscreenGeometry, DpiTransitionsUsePhysicalBoundsWithoutDoubleScaling) {
    for (const int dpi : {96,120,144}) {
        const int width = window_geometry::DipToPhysical(800,dpi);
        const int height = window_geometry::DipToPhysical(600,dpi);
        const Rect physical{0,0,width,height};
        ExpectRect(fullscreen_geometry::ControllerBoundsFromPhysical(physical,dpi),physical);
        EXPECT_EQ(window_geometry::PhysicalToDip(width,dpi),800);
        const Rect work{0,0,1000,740};
        const auto clamped = fullscreen_geometry::ClampToWorkArea({50,40,width+50,height+40},work);
        EXPECT_GE(clamped.left,work.left); EXPECT_GE(clamped.top,work.top);
        EXPECT_LE(clamped.right,work.right); EXPECT_LE(clamped.bottom,work.bottom);
    }
}

TEST(FullscreenGeometry, ZeroBoundsRemainHiddenAtEveryDpi) {
    for (const unsigned dpi : {0u,96u,120u,144u,192u})
        ExpectRect(fullscreen_geometry::ControllerBoundsFromPhysical({0,0,0,0},dpi),{0,0,0,0});
}

TEST(FullscreenGeometry, NoMonitorQueryResultPreservesLastKnownRestoreGeometry) {
    ExpectRect(fullscreen_geometry::RecoverToVisibleArea({100,100,900,700},nullptr,0,true),{100,100,900,700});
}
