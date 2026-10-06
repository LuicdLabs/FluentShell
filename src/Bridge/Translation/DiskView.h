#pragma once

#include "WindowSnapshot.h"
#include <windows.h>
#include <string>

struct IAccessible;

namespace FluentShell::Bridge::Translation {

bool IsDiskManagementView(HWND window) noexcept;
bool IsDiskManagementLegend(HWND window) noexcept;
bool ReadDiskView(HWND window, DiskViewSnapshot& snapshot, std::wstring& reason) noexcept;
bool SelectDiskViewItem(HWND window, const DiskViewSnapshot& expected,
    int index, std::wstring& reason) noexcept;
// Selection is completed and corroborated before the native keyboard context-menu
// request. The caller must defer this modal-capable method and keep its bounded
// popup interception active until it returns.
bool RequestDiskViewContextMenu(HWND window, const DiskViewSnapshot& expected,
    int index, std::wstring& reason) noexcept;
bool ScrollDiskView(HWND window, const DiskViewSnapshot& expected,
    int position, std::wstring& reason) noexcept;

namespace DiskViewDetail {
// The production entry additionally pins the Windows control's provenance and
// owning GUI thread. Tests supply a real IAccessible provider with known facts.
bool ReadItems(IAccessible* table, const RECT& screenClient,
    std::vector<DiskViewItemSnapshot>& items, std::wstring& name,
    std::wstring& reason) noexcept;
bool SelectItem(IAccessible* table, const std::vector<DiskViewItemSnapshot>& expected,
    int index, const RECT& screenClient, std::wstring& reason) noexcept;
}
}
