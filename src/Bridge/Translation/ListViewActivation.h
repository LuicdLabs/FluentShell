#pragma once

#include "WindowSnapshot.h"

namespace FluentShell::Bridge::Translation {

// Reads capability on the ListView's owner thread without running an action.
// Implemented alongside deferred activation so admission and invocation resolve
// the same native item IDs and the same MSAA item contract.
bool ListViewItemsHaveNativeDefaultActions(HWND listView, const ControlNode& node) noexcept;

} // namespace FluentShell::Bridge::Translation
