// Toolchain smoke test: confirms MSVC + Windows SDK headers/libs for
// DWM, Direct2D, DirectWrite, and the undocumented uxtheme dark-mode ordinal.
#include <windows.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <uxtheme.h>
#include <cstdio>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "uxtheme.lib")

int main() {
    ID2D1Factory* pFactory = nullptr;
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &pFactory);
    printf("D2D1CreateFactory        hr=0x%08lX ptr=%p\n", hr, (void*)pFactory);

    IDWriteFactory* pDW = nullptr;
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(&pDW));
    printf("DWriteCreateFactory      hr=0x%08lX ptr=%p\n", hr, (void*)pDW);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AnyFluentSmoke";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Smoke", WS_OVERLAPPEDWINDOW,
                               0, 0, 300, 200, nullptr, nullptr, wc.hInstance, nullptr);

    BOOL dark = TRUE;
    HRESULT h1 = DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    int corner = 2;   // DWMWCP_ROUND
    HRESULT h2 = DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
    int backdrop = 2; // DWMSBT_MAINWINDOW (Mica)
    HRESULT h3 = DwmSetWindowAttribute(hwnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &backdrop, sizeof(backdrop));
    printf("DwmSetWindowAttribute    dark=0x%08lX corner=0x%08lX backdrop=0x%08lX\n", h1, h2, h3);

    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    void* p135 = ux ? reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(135))) : nullptr; // SetPreferredAppMode
    void* p133 = ux ? reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(133))) : nullptr; // AllowDarkModeForWindow
    printf("uxtheme ord135/133       %p / %p\n", p135, p133);

    if (pDW) pDW->Release();
    if (pFactory) pFactory->Release();
    if (hwnd) DestroyWindow(hwnd);
    printf("SMOKE OK\n");
    return 0;
}
