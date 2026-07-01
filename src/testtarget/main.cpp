// testtarget.exe — a deliberately "legacy" black-box app (no AnyFluent code).
// It pops the two #32770 variants we care about: a classic resource dialog and
// a comctl32 TaskDialog. Inject anyfluenthook.dll, then trigger a dialog.
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include "../demo/resource.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' "                       \
                        "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
                        "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
                        "language='*'\"")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

#define ID_BTN_CLASSIC 201
#define ID_BTN_TASK    202

static INT_PTR CALLBACK SysProtProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_INITDIALOG: {
        SendDlgItemMessageW(h, IDC_WARNICON, STM_SETICON, (WPARAM)LoadIconW(nullptr, IDI_WARNING), 0);
        int px = -MulDiv(10, (int)GetDpiForWindow(h), 72);
        HFONT f = CreateFontW(px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, 0, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SendDlgItemMessageW(h, IDC_MAINTEXT, WM_SETFONT, (WPARAM)f, TRUE);
        return (INT_PTR)TRUE;
    }
    case WM_CTLCOLORSTATIC:
        if (GetDlgCtrlID((HWND)l) == IDC_MAINTEXT) {
            SetTextColor((HDC)w, RGB(0, 51, 153));
            SetBkMode((HDC)w, TRANSPARENT);
            return (INT_PTR)GetSysColorBrush(COLOR_3DFACE);
        }
        break;
    case WM_COMMAND:
        if (LOWORD(w) == IDC_CONTINUE || LOWORD(w) == IDCANCEL) { EndDialog(h, LOWORD(w)); return (INT_PTR)TRUE; }
        break;
    case WM_CLOSE: EndDialog(h, IDCANCEL); return (INT_PTR)TRUE;
    }
    return (INT_PTR)FALSE;
}

static void ShowTaskDialog(HWND parent) {
    TASKDIALOGCONFIG tc{ sizeof(tc) };
    tc.hwndParent = parent;
    tc.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    tc.pszWindowTitle = L"System Protection";
    tc.pszMainIcon = TD_WARNING_ICON;
    tc.pszMainInstruction = L"You will not be able to undo unwanted system changes on this drive. Are you sure you want to continue?";
    tc.pszContent = L"This will delete all restore points on this drive. This might include older system image backups.";
    static const TASKDIALOG_BUTTON btns[] = { { 1000, L"Continue" }, { IDCANCEL, L"Cancel" } };
    tc.pButtons = btns; tc.cButtons = 2;
    int pressed = 0;
    TaskDialogIndirect(&tc, &pressed, nullptr, nullptr);
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        HINSTANCE hi = ((LPCREATESTRUCT)l)->hInstance;
        CreateWindowW(L"BUTTON", L"Show System Protection dialog (classic #32770)",
                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20, 20, 330, 34, h, (HMENU)ID_BTN_CLASSIC, hi, nullptr);
        CreateWindowW(L"BUTTON", L"Show TaskDialog (#32770 + DirectUI)",
                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 20, 62, 330, 34, h, (HMENU)ID_BTN_TASK, hi, nullptr);
        CreateWindowW(L"STATIC", L"Legacy black-box target. Inject anyfluenthook.dll, then click a button.",
                      WS_CHILD | WS_VISIBLE, 20, 108, 330, 44, h, nullptr, hi, nullptr);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == ID_BTN_CLASSIC)
            DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SYSPROT), h, SysProtProc, 0);
        else if (LOWORD(w) == ID_BTN_TASK)
            ShowTaskDialog(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hi;
    wc.lpszClassName = L"AnyFluentTestTarget";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    RegisterClassW(&wc);

    wchar_t title[128];
    wsprintfW(title, L"Legacy Test Target   (PID %lu)", GetCurrentProcessId());
    HWND h = CreateWindowExW(0, wc.lpszClassName, title,
                             WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_THICKFRAME),
                             CW_USEDEFAULT, CW_USEDEFAULT, 390, 215, nullptr, nullptr, hi, nullptr);
    ShowWindow(h, SW_SHOW);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
