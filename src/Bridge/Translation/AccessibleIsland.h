// Accessible islands.
//
// Some private container classes host their content as objects that own no HWND at
// all: DirectUI is the common one.  Those objects are unreachable through window
// messages, but they are reachable through the accessibility contract the window
// itself answers, and MMC's Actions pane is the case that forced this lane.
//
// The elements are read through MSAA rather than UI Automation on purpose.  UIA
// normalizes DirectUI's "More Actions" row into a Menu that exposes no actionable
// pattern at all, so a UIA-only projection would have to refuse it; the same element
// answers IAccessible with a real accDefaultAction.  MSAA is also answered inline by
// the provider on the window's own thread, so an island is captured inside the
// ordinary source-thread pass instead of needing an out-of-band worker.
//
// The items travel as typed state on the island's own HWND-backed node, which is the
// shape a Toolbar's buttons and an HMENU's items already use.  No node on a generic
// surface becomes virtual, so the "every projected node is HWND-backed" rule holds.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace FluentShell::Bridge::Translation {

// What the projection renders for one island element.  The set is closed: an element
// whose accessible role is outside it refuses the whole island.
enum class AccessibleItemKind {
    Text,
    Button,
    Link,
};

struct AccessibleIslandItem final {
    AccessibleItemKind kind = AccessibleItemKind::Text;
    // Island-client-relative, matching how a Toolbar publishes its button rectangles.
    RECT rect{};
    std::wstring name;
    std::wstring description;
    // The provider's own accDefaultAction string.  A projected item is driven by
    // asking the provider to perform exactly this, so an empty one means the element
    // offers no action and may only be projected as text.
    std::wstring actionName;
    bool enabled = true;
    // The element opens a menu of its own rather than completing in place, so the
    // projection draws the same affordance the native element draws.
    bool dropDown = false;
};

// True when the class is one of the closed set of accessible-island hosts.  Keyed on
// the class name because these classes belong to Windows rather than to the
// application: DirectUI's window class is part of duser, not of MMC.
// The accessible name a window publishes for one of its own internal children,
// addressed by the one-based child index MSAA uses.  Some common controls carry no
// per-item text of their own -- an icon-only toolbar button is the usual case -- yet
// still publish the name a screen reader reads.  Returns an empty string when the
// window offers no accessible name for that child, which callers treat as "no name".
std::wstring AccessibleChildName(HWND window, int childIndex) noexcept;

bool IsAccessibleIslandClass(std::wstring_view className) noexcept;

// Reads the island's admitted items.  Fails closed with a specific reason: a missing
// accessible object, a role outside the admitted set, an unnamed element, an
// actionable element with no default action, an element that owns children of its
// own, or a count outside the protocol bound.
bool ReadAccessibleIslandItems(
    HWND island,
    std::vector<AccessibleIslandItem>& items,
    std::wstring& reason) noexcept;

// Performs one island item's own default action after revalidating that the element
// at `index` is still the one the snapshot published.  `expectedName` and
// `expectedAction` are the published facts; a mismatch refuses rather than acting on
// whatever moved into that position.
bool InvokeAccessibleIslandItem(
    HWND island,
    int index,
    const std::wstring& expectedName,
    const std::wstring& expectedAction,
    std::wstring& reason) noexcept;

}  // namespace FluentShell::Bridge::Translation
