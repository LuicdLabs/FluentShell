#pragma once

#include "WindowSnapshot.h"

namespace FluentShell::Bridge::Translation {

inline constexpr size_t kMaxOwnerDrawComboItems = 64;
inline constexpr int kMaxOwnerDrawComboWidth = 512;
inline constexpr int kMaxOwnerDrawComboHeight = 128;
inline constexpr size_t kMaxOwnerDrawComboBytes = 1024 * 1024;

// Called only by the ComboBox adapter on its HWND's GUI thread. Opaque item data
// is passed back to the owner unchanged and never crosses the protocol boundary.
// `labels`, when given, are the items' text as the control's owner stores them (a ComboBoxEx
// owns its inner combo, which holds no strings), one per item.
bool CaptureOwnerDrawComboBox(HWND combo, ControlNode& node, std::wstring& reason,
    const std::vector<std::wstring>* labels = nullptr);

// Called only by the ListBox adapter on its HWND's GUI thread: an LBS_OWNERDRAWFIXED or
// LBS_OWNERDRAWVARIABLE list whose rows its owner paints through WM_DRAWITEM.  Every row
// carries its normal face and the selected rows their selected face, under the same item,
// size and pixel bounds as an owner-draw combo.
bool CaptureOwnerDrawListBox(HWND list, ControlNode& node, std::wstring& reason);

} // namespace FluentShell::Bridge::Translation
