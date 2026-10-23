#pragma once

#include <string_view>

namespace testbed_caption {

// Explicit opt-in only. Other values, including "true", cannot accidentally
// change the production window. Read once for each MainWindow instance.
inline bool ParseHideButtons(std::wstring_view value) { return value == L"1"; }

inline int ButtonsWidth(int buttonWidth, bool hidden) {
    return hidden ? 0 : buttonWidth * 3;
}

// The reserved strip stays blank and inert even though the public button
// layout is empty. Bounds and points here are client physical pixels.
inline bool InReservedStrip(int x, int y, int clientWidth, int titlebarHeight,
                            int buttonWidth) {
    return x >= 0 && x < clientWidth && y >= 0 && y < titlebarHeight &&
        x >= clientWidth - ButtonsWidth(buttonWidth, false);
}

// Current MainWindow has no native button painter: fallback HTML and the
// Fang testbed draw their own controls. Install before the first navigation,
// for the main document only. visibility preserves the blank layout slots;
// capture also blocks a parent titlebar's startDrag handler in those slots.
inline constexpr wchar_t kHideButtonsScript[] = LR"JS(
(() => {
    if (window !== window.top) return;
    const selector = '.fbx-titlebar > .tt-btn, .titlebar-buttons > .titlebar-btn, [data-fbx-caption-button]';
    const style = document.createElement('style');
    style.textContent = selector + ' { visibility: hidden !important; pointer-events: none !important; }';
    const install = () => {
        if (!document.documentElement) return false;
        document.documentElement.setAttribute('data-fbx-testbed-hide-caption-buttons', '1');
        document.documentElement.appendChild(style);
        return true;
    };
    if (!install()) {
        const observer = new MutationObserver(() => {
            if (install()) observer.disconnect();
        });
        observer.observe(document, {childList: true});
    }
    const block = event => {
        const path = event.composedPath();
        const inButton = path.some(node => node instanceof Element && node.matches(selector));
        const inSlot = Array.from(document.querySelectorAll(selector)).some(button => {
            const r = button.getBoundingClientRect();
            return event.clientX >= r.left && event.clientX < r.right &&
                event.clientY >= r.top && event.clientY < r.bottom;
        });
        if (inButton || inSlot) {
            event.preventDefault();
            event.stopImmediatePropagation();
        }
    };
    for (const name of ['pointerdown', 'pointerup', 'mousedown', 'mouseup', 'click', 'dblclick', 'contextmenu']) {
        window.addEventListener(name, block, true);
    }
})();
)JS";

} // namespace testbed_caption
