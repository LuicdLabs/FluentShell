#pragma once

#include "FluentShell.h"

#include <string_view>

namespace FluentShell {

// Hard deny: never inject, never L0-touch. Shell / XAML hosts / security.
inline bool IsProcessDenied(std::wstring_view exeName) {
    static const wchar_t* kDenied[] = {
        // Core OS
        L"csrss.exe", L"winlogon.exe", L"services.exe", L"lsass.exe",
        L"smss.exe", L"svchost.exe", L"dwm.exe", L"fontdrvhost.exe",
        L"consent.exe", L"wininit.exe", L"RuntimeBroker.exe",
        // Shell / Start / Quick Settings / Search (WinUI XAML hosts)
        L"explorer.exe",
        L"ShellExperienceHost.exe",
        L"StartMenuExperienceHost.exe",
        L"SearchHost.exe",
        L"SearchApp.exe",
        L"SearchUI.exe",
        L"TextInputHost.exe",
        L"ShellHost.exe",
        L"sihost.exe",
        L"taskhostw.exe",
        L"ApplicationFrameHost.exe",
        L"SystemSettings.exe",
        L"SystemSettingsAdminFlows.exe",
        L"UserOOBEBroker.exe",
        L"LockApp.exe",
        L"LogonUI.exe",
        L"CrossDeviceResume.exe",
        L"Widgets.exe",
        L"WidgetService.exe",
        L"PhoneExperienceHost.exe",
        L"GameBar.exe",
        L"GameBarFTServer.exe",
        // Security / AV
        L"MsMpEng.exe", L"SecurityHealthService.exe", L"smartscreen.exe",
        // FluentShell must never translate or recursively inject its own hosts.
        L"FluentShell.Injector.exe",
        L"FluentShell.Renderer.exe",
        L"FluentShell.Shell.exe",
        L"FluentShell.Server.exe",
        L"IslandDemo.exe",
    };
    for (auto* d : kDenied) {
        if (EqualsIgnoreCase(exeName, d)) {
            return true;
        }
    }
    return false;
}

// Window classes that belong to system XAML / shell chrome — never L0.
inline bool IsShellOrXamlWindowClass(std::wstring_view className) {
    if (className.empty()) return false;
    // WinUI / XAML islands
    if (ContainsIgnoreCase(className, L"Xaml") ||
        ContainsIgnoreCase(className, L"Windows.UI") ||
        ContainsIgnoreCase(className, L"Microsoft.UI") ||
        ContainsIgnoreCase(className, L"CoreWindow") ||
        ContainsIgnoreCase(className, L"ApplicationFrame") ||
        ContainsIgnoreCase(className, L"Shell_") ||
        ContainsIgnoreCase(className, L"Windows.Internal") ||
        EqualsIgnoreCase(className, L"Windows.UI.Core.CoreWindow") ||
        EqualsIgnoreCase(className, L"WindowsDashboard") ||
        EqualsIgnoreCase(className, L"Shell_TrayWnd") ||
        EqualsIgnoreCase(className, L"Shell_SecondaryTrayWnd") ||
        EqualsIgnoreCase(className, L"NotifyIconOverflowWindow") ||
        EqualsIgnoreCase(className, L"TopLevelWindowForOverflowXamlIsland") ||
        EqualsIgnoreCase(className, L"XamlExplorerHostIslandWindow") ||
        EqualsIgnoreCase(className, L"Windows.UI.Composition.DesktopWindowContentBridge")) {
        return true;
    }
    return false;
}

inline bool IsProcessDeniedByPid(DWORD pid) {
    auto path = GetProcessImagePath(pid);
    if (path.empty()) return true; // unknown → safe deny for L0
    return IsProcessDenied(FileNameOf(path));
}

} // namespace FluentShell
