#pragma once

#include "WindowSnapshot.h"

#include <string>

namespace FluentShell::Bridge::Translation {

struct UiAutomationValidationOptions final {
    HWND proxy = nullptr;
    HWND nativeRoot = nullptr;
    HWND expectedOwner = nullptr;
    DWORD rendererProcessId = 0;
    uint64_t rendererCreated = 0;
    bool committed = false;
    bool requireVisible = false;
    bool requireFocus = false;
};

// Runtime UIA and window-manager checks for a native-to-proxy cutover.
bool ValidateProjectedSurface(
    const UiAutomationValidationOptions& options,
    const WindowSnapshot& snapshot,
    std::wstring& error) noexcept;

} // namespace FluentShell::Bridge::Translation
