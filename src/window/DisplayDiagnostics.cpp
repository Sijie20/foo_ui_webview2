#include "pch.h"
#include "window/DisplayDiagnostics.h"
#include "window/DisplayEvidenceFile.h"
#include "window/DisplayRecoveryPolicy.h"
#include "window/WindowShellBase.h"
#include "window/WindowManager.h"
#include "window/MainWindow.h"
#include "api/WindowApi.h"
#include <sstream>
#include <stdexcept>

namespace display_diagnostics {
namespace {
std::string RectText(const RECT& r) {
    return "[" + std::to_string(r.left) + "," + std::to_string(r.top) + "," +
        std::to_string(r.right) + "," + std::to_string(r.bottom) + "]";
}

// Open/append/flush each low-frequency record: evidence survives a later UI hang.
// Do not use the optional behavior trace or console logging preference gates.
void Emit(const std::string& line) {
    static bool reportedFailure = false;
    try {
        pfc::string8 profile;
        filesystem::g_get_display_path(core_api::get_profile_path(), profile);
        std::wstring path = pfc::stringcvt::string_wide_from_utf8(profile.get_ptr()).get_ptr();
        if (path.empty()) throw std::runtime_error("empty profile path");
        if (path.back() != L'\\') path += L'\\';
        path += L"webview_display.log";
        const DWORD error = AppendEvidenceRecord(path.c_str(), line);
        if (error != ERROR_SUCCESS) { SetLastError(error); throw std::runtime_error("append failed"); }
    } catch (...) {
        if (!reportedFailure) {
            reportedFailure = true;
            console::printf("[DisplayEvidence] file-log-failed win32=%lu (console evidence remains)", GetLastError());
        }
    }
    console::print(line.c_str());
}

constexpr int kHotkeyId = 0x507B;
constexpr UINT kModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT;
constexpr wchar_t kRecoveryClass[] = L"foo_ui_webview2.DisplayRecovery";
HWND receiver = nullptr;
unsigned registeredKey = 0;

LRESULT CALLBACK RecoveryProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_HOTKEY && wp == kHotkeyId && registeredKey) {
        Record(hwnd, nullptr, "emergency-hotkey", "WM_HOTKEY");
        auto& manager = WindowManager::GetInstance();
        // Snapshot handles before recovery can synchronously dispatch callbacks.
        std::vector<HWND> windows;
        if (auto* main = manager.GetMainWindow()) windows.push_back(main->GetShellHwnd());
        for (const auto& id : manager.GetAllWindowIds()) {
            if (auto* popup = manager.GetPopup(id); popup && popup->IsFullscreen())
                windows.push_back(popup->GetShellHwnd());
        }
        for (HWND window : windows) RecoverWindowToPrimary(window, "emergency-hotkey");
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

class RecoveryLifecycle : public initquit {
public:
    void on_init() override {
        WNDCLASSW wc{};
        wc.hInstance = core_api::get_my_instance();
        wc.lpfnWndProc = RecoveryProc;
        wc.lpszClassName = kRecoveryClass;
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            Record(nullptr, nullptr, "hotkey-receiver-failed", "on_init");
            return;
        }
        receiver = CreateWindowExW(0, kRecoveryClass, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (!receiver) {
            Record(nullptr, nullptr, "hotkey-receiver-failed", "on_init");
            return;
        }
        DWORD error = 0;
        registeredKey = display_recovery::RegisterEmergencyKey([&](unsigned key) {
            const bool success = RegisterHotKey(receiver, kHotkeyId, kModifiers, key) != FALSE;
            error = success ? ERROR_SUCCESS : GetLastError();
            return success;
        }, [&](unsigned key, bool success) {
            const std::string detail = "key=Ctrl+Alt+Shift+F" + std::to_string(key - VK_F1 + 1) +
                " win32=" + std::to_string(error);
            Record(receiver, nullptr, success ? "hotkey-registered" : "hotkey-register-failed",
                "on_init", detail.c_str());
        });
        if (!registeredKey) Record(receiver, nullptr, "hotkey-unavailable", "on_init");
    }
    void on_quit() override {
        if (receiver) {
            if (registeredKey) UnregisterHotKey(receiver, kHotkeyId);
            registeredKey = 0;
            DestroyWindow(receiver);
            receiver = nullptr;
        }
        UnregisterClassW(kRecoveryClass, core_api::get_my_instance());
    }
};
initquit_factory_t<RecoveryLifecycle> lifecycle;
}

void Record(HWND hwnd, WindowShellBase* shell, const char* event,
            const char* source, const char* detail) noexcept {
    const DWORD savedError = GetLastError();
    try {
        SYSTEMTIME utc{}; GetSystemTime(&utc);
        char timestamp[40]{};
        snprintf(timestamp, sizeof(timestamp), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
            utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
        RECT rect{}; const BOOL rectOk = hwnd && GetWindowRect(hwnd, &rect);
        std::ostringstream out;
        out << "[DisplayEvidence] utc=" << timestamp << " tick=" << GetTickCount64()
            << " pid=" << GetCurrentProcessId() << " tid=" << GetCurrentThreadId()
            << " event=" << event << " source=" << source << " hwnd=" << hwnd
            << " id=" << (shell ? shell->GetShellWindowId() : "receiver")
            << " fullscreen=" << (shell && shell->IsFullscreen())
            << " updating=" << (shell && shell->fullscreenGeometryUpdating_)
            << " topmost=" << !!(hwnd && (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST))
            << " visible=" << !!(hwnd && IsWindowVisible(hwnd))
            << " iconic=" << !!(hwnd && IsIconic(hwnd))
            << " zoomed=" << !!(hwnd && IsZoomed(hwnd))
            << " window=" << RectText(rect) << " rectOk=" << !!rectOk;
        if (hwnd) out << " dpi=" << GetDpiForWindow(hwnd)
            << " selectedMonitor=" << MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (shell && shell->savedWindowInfo_)
            out << " restore=" << RectText(shell->savedWindowInfo_->window_rect);
        if (detail && *detail) out << " " << detail;
        const BOOL enumerated = EnumDisplayMonitors(nullptr, nullptr,
            [](HMONITOR monitor, HDC, LPRECT, LPARAM context) -> BOOL {
                auto& stream = *reinterpret_cast<std::ostringstream*>(context);
                MONITORINFOEXW info{}; info.cbSize = sizeof(info);
                if (GetMonitorInfoW(monitor, &info)) {
                    stream << " monitor={handle=" << monitor
                        << " device=" << pfc::stringcvt::string_utf8_from_wide(info.szDevice).get_ptr()
                        << " primary=" << !!(info.dwFlags & MONITORINFOF_PRIMARY)
                        << " rcMonitor=" << RectText(info.rcMonitor) << " rcWork=" << RectText(info.rcWork) << "}";
                } else stream << " monitor-query-failed=" << GetLastError();
                return TRUE;
            }, reinterpret_cast<LPARAM>(&out));
        out << " enumOk=" << !!enumerated;
        Emit(out.str());
    } catch (...) {}
    SetLastError(savedError);
}

bool OnEnvironmentChange(HWND hwnd, WindowShellBase* shell, UINT msg, WPARAM wp, LPARAM lp) {
    const char* source = msg == WM_DISPLAYCHANGE ? "WM_DISPLAYCHANGE" :
        msg == WM_DPICHANGED ? "WM_DPICHANGED" : "WM_SETTINGCHANGE";
    std::string detail = "wp=" + std::to_string(wp) + " lp=" + std::to_string(lp);
    if (msg == WM_DISPLAYCHANGE) detail += " bpp=" + std::to_string(wp) +
        " desktopWidth=" + std::to_string(LOWORD(lp)) + " desktopHeight=" + std::to_string(HIWORD(lp));
    if (msg == WM_DPICHANGED && lp) detail += " suggested=" + RectText(*reinterpret_cast<RECT*>(lp));
    Record(hwnd, shell, "message-received", source, detail.c_str());
    if (!shell) return false;
    if (shell->fullscreenGeometryUpdating_) {
        // DPI reentrancy from our own SetWindowPos is expected. Real display /
        // settings notifications during entry must still recover after entry.
        if (display_recovery::ShouldDeferNotification(shell->IsFullscreen(), true, msg == WM_DPICHANGED)) {
            const bool posted = PostMessageW(hwnd, kDeferredRecoveryMessage, msg, 0) != FALSE;
            const std::string result = "win32=" + std::to_string(posted ? ERROR_SUCCESS : GetLastError());
            Record(hwnd, shell, posted ? "recovery-deferred" : "recovery-post-failed", source, result.c_str());
        }
        return true;
    }
    if (!shell->IsFullscreen()) return false;
    return RecoverWindowToPrimary(hwnd, source, true);
}

void OnDeferredRecovery(HWND hwnd, WPARAM originalMessage) {
    RecoverWindowToPrimary(hwnd, originalMessage == WM_DISPLAYCHANGE ?
        "WM_DISPLAYCHANGE-deferred" : "WM_SETTINGCHANGE-deferred", true);
}
}
