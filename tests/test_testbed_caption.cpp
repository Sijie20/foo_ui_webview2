#include "pch.h"
#include "window/TestbedCaptionPolicy.h"

TEST(TestbedCaption, OnlyExactEnvironmentValueOneEnablesHiding) {
    EXPECT_TRUE(testbed_caption::ParseHideButtons(L"1"));
    for (const auto value : {L"", L"0", L"true", L"TRUE", L"yes", L"01", L"11", L"1 ", L" 1", L"--fbx-testbed"}) {
        EXPECT_FALSE(testbed_caption::ParseHideButtons(value));
    }
}

TEST(TestbedCaption, ProductionKeepsThreeButtonsAtEveryDpi) {
    for (const int width : {46, 58, 69, 92}) {
        EXPECT_EQ(testbed_caption::ButtonsWidth(width, false), width * 3);
        EXPECT_EQ(testbed_caption::ButtonsWidth(width, true), 0);
    }
}

TEST(TestbedCaption, BlankReservedStripUsesPhysicalDimensionsAndHalfOpenEdges) {
    EXPECT_FALSE(testbed_caption::InReservedStrip(1141, 10, 1280, 32, 46));
    EXPECT_TRUE(testbed_caption::InReservedStrip(1142, 0, 1280, 32, 46));
    EXPECT_TRUE(testbed_caption::InReservedStrip(1279, 31, 1280, 32, 46));
    EXPECT_FALSE(testbed_caption::InReservedStrip(1280, 10, 1280, 32, 46));
    EXPECT_FALSE(testbed_caption::InReservedStrip(1200, 32, 1280, 32, 46));
    EXPECT_FALSE(testbed_caption::InReservedStrip(1200, -1, 1280, 32, 46));
    // 150%: 69px buttons, 48px titlebar. The public layout is empty but this
    // former button area still cannot become a draggable/clickable titlebar.
    EXPECT_FALSE(testbed_caption::InReservedStrip(1072, 20, 1280, 48, 69));
    EXPECT_TRUE(testbed_caption::InReservedStrip(1073, 47, 1280, 48, 69));
}

TEST(TestbedCaption, ReservedStripStaysInsideNarrowOrEmptyClient) {
    EXPECT_TRUE(testbed_caption::InReservedStrip(0, 10, 100, 32, 46));
    EXPECT_TRUE(testbed_caption::InReservedStrip(99, 10, 100, 32, 46));
    EXPECT_FALSE(testbed_caption::InReservedStrip(-1, 10, 100, 32, 46));
    EXPECT_FALSE(testbed_caption::InReservedStrip(0, 10, 0, 32, 46));
    EXPECT_FALSE(testbed_caption::InReservedStrip(99, 0, 100, 0, 46));
}
