#pragma once

#include "WindowSnapshot.h"

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
// Called by the hook.  Keeps only the first popup of a bracket, which is the one the
// button opened.
void RecordInterceptedPopup(HMENU menu) noexcept;

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

// How many top-level menus the bar publishes, and the name of one of them.  The index is
// the accessible child index, so it is 1-based.
int MenuBarButtonCount(HWND toolbar) noexcept;
std::wstring MenuBarButtonName(HWND toolbar, int index) noexcept;

// Arms interception and performs one top-level button's own accessible default action.
// The popup the application opens for it is captured by the hook, possibly after this
// returns, and collected by TakeRecordedMenu.
bool DriveMenuBarButton(
    HWND toolbar,
    int index,
    std::wstring itemIdPath,
    std::wstring& reason) noexcept;

// Arms interception for the calling thread for its lifetime.
class PopupInterceptionScope final {
public:
    PopupInterceptionScope() noexcept;
    ~PopupInterceptionScope();
    PopupInterceptionScope(const PopupInterceptionScope&) = delete;
    PopupInterceptionScope& operator=(const PopupInterceptionScope&) = delete;

    // The popup the bracketed call opened, or nullptr when it opened none.
    HMENU Recorded() const noexcept;
};

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

// Reads the whole menu bar: one accessible default action per top-level button, with
// popups intercepted, and each recorded HMENU captured into the projected menu shape.
// Returns false with a reason when any part of the bar cannot be projected, because a
// menu bar missing one of its menus is worse than one that is not projected at all.
bool CaptureMenuBarToolbar(
    HWND toolbar,
    std::vector<MenuItemSnapshot>& menu,
    std::wstring& reason);

} // namespace FluentShell::Bridge::Translation
