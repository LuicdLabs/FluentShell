#pragma once

#include "FluentShell.h"

namespace FluentShell {

// A file identity check, not an executable-name exception. Copies and renamed
// programs do not qualify for the watcher's narrow shell-dialog scope.
inline bool IsSystemExplorerImage(const std::wstring& image) noexcept {
    wchar_t windows[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    try {
        const std::wstring expected = std::wstring(windows) + L"\\explorer.exe";
        HANDLE actual = CreateFileW(image.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (actual == INVALID_HANDLE_VALUE) return false;
        HANDLE system = CreateFileW(expected.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        BY_HANDLE_FILE_INFORMATION a{}, b{};
        const bool same = system != INVALID_HANDLE_VALUE &&
            GetFileInformationByHandle(actual, &a) && GetFileInformationByHandle(system, &b) &&
            a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
            a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
        if (system != INVALID_HANDLE_VALUE) CloseHandle(system);
        CloseHandle(actual);
        return same;
    } catch (...) { return false; }
}

// Only real, visible Win32 dialog roots in the selected process. Every child
// must still pass normal capture, revision, and UIA gates; this is not an
// exemption for shell/XAML controls or partial-window projection.
inline bool IsExplorerDialogWindow(HWND window, DWORD pid) noexcept {
    DWORD actualPid = 0;
    GetWindowThreadProcessId(window, &actualPid);
    if (!pid || actualPid != pid || GetAncestor(window, GA_ROOT) != window ||
        !IsWindowVisible(window)) return false;
    if (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD) return false;
    if (GetWindowLongPtrW(window, GWL_EXSTYLE) & (WS_EX_TOOLWINDOW | WS_EX_LAYERED)) return false;
    wchar_t name[64]{};
    RECT bounds{};
    return GetClassNameW(window, name, static_cast<int>(std::size(name))) &&
        EqualsIgnoreCase(name, L"#32770") &&
        GetWindowRect(window, &bounds) && !IsRectEmpty(&bounds);
}

inline std::wstring ExplorerDialogGrantName(DWORD pid, ULONGLONG created) {
    return L"Local\\FluentShell.ExplorerDialogs." + std::to_wstring(pid) + L"." + std::to_wstring(created);
}

// A modal dialog may keep its real Explorer folder owner. The application,
// not the renderer, owns the disabled state and will re-enable it on dismissal.
inline HWND ExplorerNativeModalOwner(HWND dialog, DWORD pid) noexcept {
    if (!IsExplorerDialogWindow(dialog, pid)) return nullptr;
    HWND owner = GetWindow(dialog, GW_OWNER);
    for (unsigned depth = 0; owner && depth < 256; ++depth) {
        RECT bounds{};
        if (IsWindowVisible(owner) && GetWindowRect(owner, &bounds) && !IsRectEmpty(&bounds)) {
            DWORD ownerPid = 0;
            GetWindowThreadProcessId(owner, &ownerPid);
            wchar_t name[64]{};
            GetClassNameW(owner, name, static_cast<int>(std::size(name)));
            return ownerPid == pid && !IsWindowEnabled(owner) &&
                (EqualsIgnoreCase(name, L"CabinetWClass") || EqualsIgnoreCase(name, L"ExploreWClass"))
                ? owner : nullptr;
        }
        const HWND next = GetWindow(owner, GW_OWNER);
        if (next == owner) break;
        owner = next;
    }
    return nullptr;
}

// Never retain a handle while checking: the watcher owns the grant's lifetime.
inline bool HasExplorerDialogGrant(DWORD pid, ULONGLONG created) noexcept {
    try {
        HANDLE watcher = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\FluentShell.Watch");
        if (!watcher) return false;
        CloseHandle(watcher);
        HANDLE grant = OpenEventW(SYNCHRONIZE, FALSE, ExplorerDialogGrantName(pid, created).c_str());
        if (!grant) return false;
        const bool active = WaitForSingleObject(grant, 0) == WAIT_OBJECT_0;
        CloseHandle(grant);
        return active;
    } catch (...) { return false; }
}

inline bool CurrentExplorerDialogGrant() noexcept {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return false;
    ULARGE_INTEGER value{};
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    return HasExplorerDialogGrant(GetCurrentProcessId(), value.QuadPart);
}

} // namespace FluentShell
