#pragma once

#include "WindowSnapshot.h"

namespace FluentShell::Bridge::Translation {

inline constexpr size_t kMaxOwnerDrawComboItems = 64;
inline constexpr int kMaxOwnerDrawComboWidth = 512;
inline constexpr int kMaxOwnerDrawComboHeight = 128;
inline constexpr size_t kMaxOwnerDrawComboBytes = 1024 * 1024;

// Called only by the ComboBox adapter on its HWND's GUI thread. Opaque item data
// is passed back to the owner unchanged and never crosses the protocol boundary.
bool CaptureOwnerDrawComboBox(HWND combo, ControlNode& node, std::wstring& reason);

} // namespace FluentShell::Bridge::Translation
