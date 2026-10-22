#include "pch.h"
#include "window/DisplayRecoveryPolicy.h"
#include "window/DisplayEvidenceFile.h"
#include "window/FullscreenGeometry.h"
#include <filesystem>
#include <fstream>
#include <iterator>

TEST(DisplayRecovery, DisplayChangeExitsBeforeClampingSavedSecondaryRectToPrimary) {
    bool fullscreen = true;
    fullscreen_geometry::Rect window{1920, 0, 4480, 1440};
    const fullscreen_geometry::Rect saved{2400, 200, 3600, 1000};
    const fullscreen_geometry::Rect primary{0, 0, 1920, 1040};
    std::vector<std::string> actions;
    EXPECT_TRUE(display_recovery::RecoverDisplayChange(fullscreen, false, [&] {
        actions.push_back("exit");
        fullscreen = false;
        window = saved;
    }, [&] {
        EXPECT_FALSE(fullscreen);
        actions.push_back("clamp");
        window = fullscreen_geometry::ClampToWorkArea(window, primary);
    }));
    EXPECT_EQ(actions, (std::vector<std::string>{"exit", "clamp"}));
    EXPECT_EQ(window.left, 720); EXPECT_EQ(window.right, 1920);
    EXPECT_EQ(window.top, 200); EXPECT_EQ(window.bottom, 1000);
}

TEST(DisplayRecovery, WindowedDisplayNotificationPreservesExistingWindow) {
    int calls = 0;
    EXPECT_FALSE(display_recovery::RecoverDisplayChange(false, false, [&] { ++calls; }, [&] { ++calls; }));
    EXPECT_EQ(calls, 0);
}

TEST(DisplayRecovery, NestedDpiDuringRestoreCannotReenterOrOverwritePrimaryClamp) {
    bool updating = false;
    int nestedActions = 0;
    {
        display_recovery::UpdatingGuard guard(updating);
        EXPECT_TRUE(updating);
        EXPECT_FALSE(display_recovery::RecoverDisplayChange(true, updating,
            [&] { ++nestedActions; }, [&] { ++nestedActions; }));
        { display_recovery::UpdatingGuard nested(updating); }
        EXPECT_TRUE(updating);
    }
    EXPECT_FALSE(updating);
    EXPECT_EQ(nestedActions, 0);
}

TEST(DisplayRecovery, PrimaryWorkAreaWinsEvenWhenSecondaryStillExists) {
    const auto rect = fullscreen_geometry::ClampToWorkArea(
        {2800, 900, 4800, 2300}, {-1920, -1080, 0, -40});
    EXPECT_EQ(rect.left, -1920); EXPECT_EQ(rect.top, -1080);
    EXPECT_EQ(rect.right, 0); EXPECT_EQ(rect.bottom, -40);
}

TEST(DisplayRecovery, DisplayAndSettingsNotificationsDuringEntryAreDeferred) {
    EXPECT_TRUE(display_recovery::ShouldDeferNotification(true, true, false));
    EXPECT_FALSE(display_recovery::ShouldDeferNotification(false, true, false));
    EXPECT_FALSE(display_recovery::ShouldDeferNotification(true, false, false));
}

TEST(DisplayRecovery, DpiReentrancyFromOurOwnMoveDoesNotScheduleAnotherRecovery) {
    EXPECT_FALSE(display_recovery::ShouldDeferNotification(true, true, true));
    EXPECT_FALSE(display_recovery::ShouldDeferNotification(false, true, true));
}

TEST(DisplayRecovery, EmergencyHotkeyRegistersPreferredAndReportsSuccess) {
    std::vector<unsigned> attempts;
    std::vector<bool> results;
    EXPECT_EQ(display_recovery::RegisterEmergencyKey([&](unsigned key) {
        attempts.push_back(key); return true;
    }, [&](unsigned, bool ok) { results.push_back(ok); }), static_cast<unsigned>(VK_F12));
    EXPECT_EQ(attempts, (std::vector<unsigned>{VK_F12}));
    EXPECT_EQ(results, (std::vector<bool>{true}));
}

TEST(DisplayRecovery, EmergencyHotkeyConflictFallsBackAndReportsEveryAttempt) {
    std::vector<unsigned> attempts, reports;
    std::vector<bool> results;
    EXPECT_EQ(display_recovery::RegisterEmergencyKey([&](unsigned key) {
        attempts.push_back(key); return key == VK_F10;
    }, [&](unsigned key, bool ok) {
        reports.push_back(key); results.push_back(ok);
    }), static_cast<unsigned>(VK_F10));
    EXPECT_EQ(attempts, (std::vector<unsigned>{VK_F12, VK_F11, VK_F10}));
    EXPECT_EQ(reports, attempts);
    EXPECT_EQ(results, (std::vector<bool>{false, false, true}));
}

TEST(DisplayRecovery, EmergencyHotkeyAllConflictsLeaveNoInventedRegistration) {
    int attempts = 0, failures = 0;
    EXPECT_EQ(display_recovery::RegisterEmergencyKey([&](unsigned) {
        ++attempts; return false;
    }, [&](unsigned, bool ok) { if (!ok) ++failures; }), 0u);
    EXPECT_EQ(attempts, 3); EXPECT_EQ(failures, 3);
}

TEST(DisplayRecovery, FullscreenPathsNeverCallDisplayModeRefreshOrHdrMutationApis) {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    // Runtime window paths must remain borderless-only. Keep this guard against
    // accidentally introducing exclusive mode or display-configuration writes.
    for (const auto* directory : {"src/window", "src/api"}) {
        for (const auto& entry : std::filesystem::directory_iterator(root / directory)) {
            if (entry.path().extension() != ".cpp" && entry.path().extension() != ".h") continue;
            std::ifstream file(entry.path());
            ASSERT_TRUE(file.is_open()) << entry.path().string();
            const std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
            for (const auto* api : {"ChangeDisplaySettings", "SetDisplayConfig", "DisplayConfigSetDeviceInfo",
                                    "SetMonitorVCPFeature", "SetPhysicalMonitorConfiguration"})
                EXPECT_EQ(source.find(api), std::string::npos) << entry.path().string() << ": " << api;
        }
    }
}

TEST(DisplayRecovery, EvidenceFileAppendsAndFlushesUtf8RecordsWithoutTruncation) {
    wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
    ASSERT_NE(GetTempPathW(MAX_PATH, directory), 0u);
    ASSERT_NE(GetTempFileNameW(directory, L"dsp", 0, path), 0u);
    struct Cleanup {
        const wchar_t* path;
        ~Cleanup() { DeleteFileW(path); }
    } cleanup{path};
    ASSERT_EQ(display_diagnostics::AppendEvidenceRecord(path, "[DisplayEvidence] first"), ERROR_SUCCESS);
    ASSERT_EQ(display_diagnostics::AppendEvidenceRecord(path, "second 中文"), ERROR_SUCCESS);
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    ASSERT_TRUE(file.is_open());
    const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    EXPECT_EQ(bytes, "[DisplayEvidence] first\r\nsecond 中文\r\n");
}

TEST(DisplayRecovery, EvidenceFileReportsAnUnwritablePathInsteadOfClaimingSuccess) {
    EXPECT_NE(display_diagnostics::AppendEvidenceRecord(L"", "evidence"), ERROR_SUCCESS);
}
