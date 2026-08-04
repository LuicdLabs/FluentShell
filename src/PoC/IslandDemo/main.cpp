#include "../../IslandHost/IslandHost.h"
#include "../../Renderer.Dwm/DwmRenderer.h"

#include <windows.h>
#include <string>

#include <Microsoft.UI.Dispatching.Interop.h>

// Standalone smoke test on its own STA with ContentPreTranslateMessage pump.

static FluentShell::Island::XamlIslandHost* g_host = nullptr;
static HWND g_hwnd = nullptr;

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        if (g_host) {
            g_host->ResizeToParent();
        }
        return 0;
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hWnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_host) {
            g_host->ResizeToParent();
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR cmd, int show) {
    // Optional: --smoke uses process XamlRuntime self-test then exits.
    if (cmd && wcsstr(cmd, L"--smoke")) {
        const bool ok = FluentShell::Island::RunIslandSmokeTest(2500);
        FluentShell::Island::XamlRuntime::Instance().Shutdown();
        return ok ? 0 : 1;
    }

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"FluentShell.IslandDemo";
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"FluentShell IslandDemo",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        CW_USEDEFAULT, CW_USEDEFAULT, 720, 520,
        nullptr, nullptr, hi, nullptr);

    FluentShell::Dwm::ApplyToWindow(g_hwnd, FluentShell::DwmStyle{
        true, FluentShell::Backdrop::Mica, true, false });

    FluentShell::Island::XamlIslandHost host;
    g_host = &host;

    if (!host.InitializeOnCurrentThread()) {
        MessageBoxW(g_hwnd, L"Failed to initialize WinAppSDK / XAML.", L"IslandDemo", MB_ICONERROR);
        return 1;
    }
    if (!host.AttachToWindow(g_hwnd)) {
        MessageBoxW(g_hwnd, L"Failed to attach island.", L"IslandDemo", MB_ICONERROR);
        return 2;
    }

    std::wstring label = L"FluentShell · stable render path (";
    label += host.BackendName();
    label += L")";
    if (!host.SetSmokeTestContent(label)) {
        MessageBoxW(g_hwnd, L"Failed to set XAML content.", L"IslandDemo", MB_ICONERROR);
        return 3;
    }
    host.ResizeToParent();

    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Required for island input / focus / composition scheduling.
        if (ContentPreTranslateMessage(&msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_host = nullptr;
    host.Shutdown();
    return 0;
}
