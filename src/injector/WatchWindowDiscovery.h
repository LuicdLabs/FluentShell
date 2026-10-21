#pragma once

#include <windows.h>

namespace FluentShell::Injector {

// Control-panel property sheets may be owned by a hidden StubWindow32. Like
// Bridge discovery, treat hidden or zero-area owners as scaffolding, but walk
// the whole chain so a visible ancestor still keeps an owned dialog out.
inline bool IsWatchWindowCandidate(HWND window) noexcept {
    if (!IsWindowVisible(window)) return false;
    if (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    const HWND desktop = GetDesktopWindow();
    HWND owner = GetWindow(window, GW_OWNER);
    for (unsigned depth = 0; owner && depth < 256; ++depth) {
        RECT bounds{};
        if (owner != desktop && IsWindowVisible(owner)) {
            if (!GetWindowRect(owner, &bounds) || !IsRectEmpty(&bounds)) return false;
        }
        const HWND next = GetWindow(owner, GW_OWNER);
        if (next == owner) return false;
        owner = next;
    }
    return owner == nullptr;
}

} // namespace FluentShell::Injector
