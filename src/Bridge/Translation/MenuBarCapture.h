#pragma once

#include "WindowSnapshot.h"

#include <atomic>
#include <string>
#include <vector>

namespace FluentShell::Bridge::Translation {

// Some applications draw their menu bar with a toolbar control instead of an HMENU, and
// open each popup from the click itself rather than from a WM_COMMAND anyone can post.
// MMC is the case this exists for.  Such a bar is projected as a real menu, not as a row
// of buttons, which means the projection has to learn what each popup contains without a
// native popup ever reaching the screen.
//
// The two halves:
//
//   * The click is performed through the control's own accessible default action, so
//     comctl32 does exactly what a real click does and the application decides to open
//     its menu.
//   * `TrackPopupMenu`/`TrackPopupMenuEx` are intercepted while that click runs.  The
//     hook records the HMENU and answers as if the user had dismissed the menu, so the
//     application's own popup is never shown and there is no native surface on screen.
//
// Interception is per thread and must bracket exactly the call that drives the button:
// arming it around anything else would swallow a popup the application opened for its
// own reasons.

// True while the calling thread wants popups recorded instead of shown.
bool PopupInterceptionArmed() noexcept;
// True while any thread of this process must not put a native popup menu on screen: a
// projected surface is cloaked and its screen belongs to the proxy, so a popup the
// application opens there would be a native window over a WinUI projection.  The read
// below arms it for the whole sequence, not per button, because an application can open
// its popup after the call that asked for it has already returned.
bool PopupSuppressionActive() noexcept;
// A read may have queued a popup before cancellation. Keep those popups hidden
// for a previously read owner while its native root remains application-cloaked.
bool ShouldSuppressPopup(HWND owner) noexcept;
// Called by the hook. Keeps only the first popup of a bracket. Reads dismiss it;
// an explicitly bound selection returns the native tracking result. Owner and
// flags may be omitted only by capture-only test callers.
BOOL RecordInterceptedPopup(HMENU menu, UINT flags = 0, HWND owner = nullptr) noexcept;

// The staged form of the same bracket, for the case the scope below cannot serve: an
// application that opens its popup from its own message loop rather than from inside the
// call that drove the button.  Arm, drive, return to that loop, then collect.  Arming and
// collecting must happen on the same thread, which is the window's own GUI thread.
//
// The menu is captured inside the hook rather than collected as a handle, because an
// application destroys the popup as soon as the tracking call answers -- the handle is
// alive only for the length of that call.
void ArmPopupInterception(std::wstring itemIdPath) noexcept;
void DisarmPopupInterception() noexcept;
// Brackets the whole staged read.  While it is held no popup of this process reaches the
// screen, so a popup that arrives late -- after the button that asked for it was already
// given up on -- is still swallowed instead of appearing over the projection.
class PopupSuppressionScope final {
public:
    PopupSuppressionScope() noexcept;
    ~PopupSuppressionScope();
    PopupSuppressionScope(const PopupSuppressionScope&) = delete;
    PopupSuppressionScope& operator=(const PopupSuppressionScope&) = delete;
};
// True once the hook has captured a popup for the armed bracket.  Lets the waiter stop
// pumping the moment the application has answered.
bool MenuPopupRecorded() noexcept;

// Collects what the hook captured.  Returns false when the bracket recorded no popup.
bool TakeRecordedMenu(
    std::vector<MenuItemSnapshot>& items,
    bool& systemMenu,
    std::wstring& reason) noexcept;

struct MenuBarButton final {
    std::wstring text;
    bool enabled = true;
    bool operator==(const MenuBarButton&) const = default;
};

// Projected command IDs are unique across the complete bar. Native applications
// can reuse an ID in several ephemeral popups, so dispatch retains the exact
// toolbar slot and tracking contract instead of posting that ID to the frame.
struct MenuBarCommandBinding final {
    uint32_t commandId = 0;
    uint32_t nativeCommandId = 0;
    int toolbarIndex = 0; // One-based accessible child; includes skipped system menus.
    std::wstring itemId;
    std::wstring text;
    std::vector<std::wstring> ancestors;
    HWND owner = nullptr;
    UINT trackingFlags = 0;
};

// Metadata is read on the source thread without opening any popup.  It is also the
// signature that makes a replaced or relabelled toolbar invalidate its cached menu.
bool ReadMenuBarButtons(HWND toolbar, std::vector<MenuBarButton>& buttons) noexcept;

// Refresh only after meaningful changes, with a debounce and at most three attempts
// per change. A quiet, unreadable bar must not repeatedly steal input from the proxy.
struct MenuBarRefreshPolicy final {
    bool pending = true;
    unsigned attempts = 0;
    uint64_t due = 0;
    uint64_t nextAllowed = 0;

    void Invalidate(uint64_t now) noexcept;
    bool ShouldRead(uint64_t now) const noexcept;
    void Complete(uint64_t now, bool success) noexcept;
};

// True throughout the source-thread pump and its cleanup. Other Bridge commands,
// including ones for a sibling surface on the same thread, must remain queued.
bool MenuBarReadInProgress() noexcept;

// Arms interception and performs one top-level button's own accessible default action.
// The popup the application opens for it is captured by the hook, possibly after this
// returns, and collected by TakeRecordedMenu.
bool DriveMenuBarButton(
    HWND toolbar,
    int index,
    std::wstring itemIdPath,
    std::wstring& reason) noexcept;

// True when the toolbar publishes its buttons as menu items rather than as buttons.
// That is the control's own statement about what it is, which is why it decides here
// instead of a class name or a style bit.
bool IsMenuBarToolbar(HWND toolbar) noexcept;

// True when the menu is a window's system menu: it carries system commands, which need
// WM_SYSCOMMAND rather than WM_COMMAND.  A menu bar's leading document-icon button opens
// exactly this, and the projection already draws those commands on the window's own
// Fluent caption, so it is skipped rather than refused.
bool IsWindowSystemMenu(HMENU menu) noexcept;

// The toolbar a window uses as its menu bar, or null when it has none.
HWND FindMenuBarToolbar(HWND root) noexcept;

// Reads the whole bar on its own thread with a bounded application-message pump.
// Intercepted menus are copied while their HMENU is alive. Cancellation, WM_QUIT,
// exceptions and timeouts all leave interception, activation and tracking balanced.
bool CaptureMenuBarToolbar(
    HWND root,
    HWND toolbar,
    const std::vector<MenuBarButton>& buttons,
    DWORD popupWaitMs,
    DWORD totalWaitMs,
    const std::atomic<bool>& cancelled,
    std::vector<MenuItemSnapshot>& menu,
    std::wstring& reason,
    std::vector<MenuBarCommandBinding>* commands = nullptr) noexcept;

// Reopens just the bound native slot and resolves the selection while its HMENU
// is alive. The hook returns TPM_RETURNCMD or notifies the actual tracking owner.
bool InvokeMenuBarToolbarCommand(
    HWND root, HWND toolbar, const std::vector<MenuBarButton>& buttons,
    const MenuBarCommandBinding& command, const std::atomic<bool>& cancelled,
    std::wstring& reason) noexcept;

} // namespace FluentShell::Bridge::Translation
