// Snapshot builders for the translated dialog lane.
//
// A MessageBox or TaskDialog call is answered by the Bridge itself: no native
// dialog window is ever created, so these snapshots describe fully virtual
// surfaces whose nodes carry no HWND.  They live outside RendererSession so the
// native test suite can hold them to the same cross-node invariants the renderer
// enforces on admission -- unique nonzero node IDs, unique z-index, and nodes that
// claim no native window.
#pragma once

#include <windows.h>
#include <commctrl.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "WindowSnapshot.h"

namespace FluentShell::Bridge::Translation {

// Centers a dialog of the requested size over its owner, or over the primary
// monitor's work area when the call has no owner window.
RECT CenteredBounds(HWND owner, LONG width, LONG height);

std::wstring MessageBoxIcon(UINT type);
std::wstring TaskDialogIcon(PCWSTR icon);

// `results` receives the node ID to dialog result code mapping the surface needs
// in order to answer the original API call.
WindowSnapshot BuildMessageBoxSnapshot(
    HWND owner,
    std::wstring_view text,
    std::wstring_view caption,
    UINT type,
    std::unordered_map<uint64_t, int>& results);

// `verificationNode` receives the checkbox node ID when the configuration asks
// for a verification checkbox, so its state can be reported back to the caller.
WindowSnapshot BuildTaskDialogSnapshot(
    const TASKDIALOGCONFIG& config,
    const std::vector<std::pair<int, std::wstring>>& buttons,
    std::wstring_view title,
    std::wstring_view instruction,
    std::wstring_view content,
    std::wstring_view footer,
    std::wstring_view verification,
    std::unordered_map<uint64_t, int>& results,
    std::optional<uint64_t>& verificationNode);

}  // namespace FluentShell::Bridge::Translation
