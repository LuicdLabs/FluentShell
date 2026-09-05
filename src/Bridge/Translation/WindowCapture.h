#pragma once

#include "WindowSnapshot.h"

#include <string>
#include <unordered_map>

namespace FluentShell::Bridge::Translation {

struct CaptureContext final {
    struct NodeIdentity final {
        uint64_t generation = 0;
        uint64_t nodeId = 0;
    };

    std::wstring surfaceId;
    uint64_t generation = 0;
    uint64_t revision = 0;
    uint64_t nextNodeId = 1;
    uint64_t nextNodeGeneration = 1;
    std::unordered_map<HWND, NodeIdentity> nodeIds;
    // A menu bar drawn with a toolbar is read once and reused: the read opens the
    // application's own popups, so repeating it on every reconcile would be both
    // expensive and visible.  Empty until the first capture finds one.
    std::vector<MenuItemSnapshot> menuBarToolbarMenu;
    HWND menuBarToolbar = nullptr;
    // Set by the stage that is allowed to ask the application to open its own menus:
    // after the projection is committed and the native window is cloaked.  A capture
    // that runs before that only identifies the bar.
    bool menuBarToolbarReadable = false;
    // A menu-bar toolbar whose popups could not be read is projected as a toolbar
    // instead, and the reason is logged once rather than on every capture.
    bool menuBarToolbarRejected = false;
};

bool CaptureWindow(
    HWND root,
    CaptureContext& context,
    WindowSnapshot& snapshot,
    std::wstring& rejectionReason) noexcept;

bool CaptureTopLevelMenu(
    HWND root,
    std::vector<MenuItemSnapshot>& menu,
    std::wstring& rejectionReason) noexcept;

// Captures one HMENU as projected menu items.  Used for a window's own menu bar and for
// a popup an application opened from a menu-bar toolbar button, which is read from the
// handle rather than from the window that would have shown it.
bool CaptureMenuHandle(
    HMENU menu,
    std::wstring_view itemIdPath,
    std::vector<MenuItemSnapshot>& items,
    std::wstring& rejectionReason) noexcept;

uint64_t SnapshotFingerprint(const WindowSnapshot& snapshot) noexcept;

} // namespace FluentShell::Bridge::Translation
