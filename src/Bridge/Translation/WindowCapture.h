#pragma once

#include "WindowSnapshot.h"
#include "MenuBarCapture.h"

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
    // Owned exclusively by the source thread. Capture only observes the bar; the
    // post-commit refresh command opens its popups and publishes a complete menu.
    std::vector<MenuItemSnapshot> menuBarToolbarMenu;
    std::vector<MenuBarCommandBinding> menuBarToolbarCommands;
    uint64_t menuBarBindingGeneration = 0;
    HWND menuBarToolbar = nullptr;
    uint64_t menuBarToolbarGeneration = 0;
    std::vector<MenuBarButton> menuBarButtons;
    MenuBarRefreshPolicy menuBarRefresh;
};

// Revalidates HWND lifetime and published menu labels without driving the control.
// Removes cached commands immediately when the bar disappears or changes identity.
void ObserveMenuBarToolbar(HWND root, CaptureContext& context) noexcept;

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
// Only the toolbar binding lane may disable command uniqueness; it remaps duplicate
// native IDs before publication and revalidates the original live item on invocation.
bool CaptureMenuHandle(
    HMENU menu,
    std::wstring_view itemIdPath,
    std::vector<MenuItemSnapshot>& items,
    std::wstring& rejectionReason,
    bool requireUniqueCommands = true) noexcept;

uint64_t SnapshotFingerprint(const WindowSnapshot& snapshot) noexcept;

} // namespace FluentShell::Bridge::Translation
