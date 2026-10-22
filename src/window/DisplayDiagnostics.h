#pragma once
#include <windows.h>
class WindowShellBase;

namespace display_diagnostics {
inline constexpr UINT kDeferredRecoveryMessage = WM_APP + 0x5B7;
// Always-on, low-frequency evidence, console + profile/webview_display.log.
void Record(HWND hwnd, WindowShellBase* shell, const char* event,
            const char* source, const char* detail = "") noexcept;
// Logs receipt before any mutation. Returns true if recovered or in a geometry
// transition, so a nested DPI suggested rect must not overwrite recovery.
bool OnEnvironmentChange(HWND hwnd, WindowShellBase* shell, UINT msg,
                         WPARAM wp, LPARAM lp);
void OnDeferredRecovery(HWND hwnd, WPARAM originalMessage);
}
