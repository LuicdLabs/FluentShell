// Tier 1 — self-contained before/after demo.
// Shows two identical "System Protection" #32770 dialogs side by side; the
// right one is run through fluent::ModernizeDialog to reproduce Untitled.png.
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include "resource.h"
#include "../common/FluentCore.h"

// Pull in the themed common-controls v6 activation context (needed for
// SetWindowSubclass and themed default rendering of the "before" dialog).
#pragma comment(linker, "\"/manifestdependency:type='win32' "                       \
                        "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
                        "language='*'\"")

static INT_PTR CALLBACK DemoProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)l);      // mode: 0=before, 1=after
        HICON ic = LoadIconW(nullptr, IDI_WARNING);
        SendDlgItemMessageW(h, IDC_WARNICON, STM_SETICON, (WPARAM)ic, 0);
        int px = -MulDiv(10, (int)GetDpiForWindow(h), 72);     // ~10pt main-instruction
        HFONT f = CreateFontW(px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, 0, 0, 0,
                              CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SendDlgItemMessageW(h, IDC_MAINTEXT, WM_SETFONT, (WPARAM)f, TRUE);
        return (INT_PTR)TRUE;
    }
    case WM_CTLCOLORSTATIC: {
        if (GetWindowLongPtrW(h, GWLP_USERDATA) == 0 &&
            GetDlgCtrlID((HWND)l) == IDC_MAINTEXT) {            // "before": classic blue instruction
            SetTextColor((HDC)w, RGB(0, 51, 153));
            SetBkMode((HDC)w, TRANSPARENT);
            return (INT_PTR)GetSysColorBrush(COLOR_3DFACE);
        }
        break;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDCANCEL || LOWORD(w) == IDC_CONTINUE)
            return (INT_PTR)TRUE;                              // keep windows up for comparison
        break;
    case WM_CLOSE:
        PostQuitMessage(0);
        return (INT_PTR)TRUE;
    }
    return (INT_PTR)FALSE;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    HWND hBefore = CreateDialogParamW(hInst, MAKEINTRESOURCEW(IDD_SYSPROT), nullptr, DemoProc, 0);
    HWND hAfter  = CreateDialogParamW(hInst, MAKEINTRESOURCEW(IDD_SYSPROT), nullptr, DemoProc, 1);
    if (!hBefore || !hAfter) return 1;

    SetWindowTextW(hBefore, L"System Protection   (Before)");
    SetWindowTextW(hAfter,  L"System Protection   (After — Fluent)");

    RECT rb; GetWindowRect(hBefore, &rb);
    int w = rb.right - rb.left, ht = rb.bottom - rb.top;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int gap = 48, x0 = (sw - (w * 2 + gap)) / 2, y = (sh - ht) / 2 - 40;
    SetWindowPos(hBefore, nullptr, x0,           y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    SetWindowPos(hAfter,  nullptr, x0 + w + gap, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    fluent::ModernizeDialog(hAfter, false);

    ShowWindow(hBefore, SW_SHOW);
    ShowWindow(hAfter,  SW_SHOW);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hBefore, &msg) && !IsDialogMessageW(hAfter, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
