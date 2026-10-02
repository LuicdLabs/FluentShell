#pragma once

#include "WindowSnapshot.h"

namespace FluentShell::Bridge::Translation {

// Reads capability on the ListView's owner thread without running an action.
// Implemented alongside deferred activation so admission and invocation resolve
// the same native item IDs and the same MSAA item contract.
bool ListViewItemsHaveNativeDefaultActions(HWND listView, const ControlNode& node) noexcept;

// Native client coordinates only. A candidate is admitted only when the control
// itself hit-tests it to this item's first-column label or icon, never its state
// image, another subitem, or empty row space. Does not scroll or change selection.
bool ReadListViewNativeActivationPoint(HWND listView, int index, POINT& point) noexcept;

} // namespace FluentShell::Bridge::Translation
