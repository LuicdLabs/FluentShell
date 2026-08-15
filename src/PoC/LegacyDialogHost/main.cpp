#include <windows.h>
#include <commctrl.h>

#include <string>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr wchar_t kHostClass[] = L"FluentShell.LegacyDialogHost";
constexpr wchar_t kUnsupportedClass[] = L"FluentShell.UnsupportedCustomChild";
constexpr UINT_PTR kOracleTimer = 1;

enum ControlId : int {
    IdMessageBox = 1001,
    IdTaskDialog = 1002,
    IdSaveConfirm = 1003,
    IdEdit = 1101,
    IdCheck = 1102,
    IdCombo = 1103,
    IdList = 1104,
    IdOracle = 1105,
    IdResult = 1106,
    IdCloseVeto = 1107,
    IdCreateUnsupported = 1108,
    IdProgress = 1109,
    IdMenuResetProgress = 1201,
    IdMenuExit = 1202,
};

HWND gMain = nullptr;
HWND gOracle = nullptr;
HWND gResult = nullptr;
HWND gUnsupported = nullptr;
HWND gProgress = nullptr;
unsigned long long gOracleTick = 0;

void AppendLog(const std::wstring& message) {
    const std::wstring debug = L"[LegacyDialogHost] " + message + L"\n";
    OutputDebugStringW(debug.c_str());

    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(static_cast<DWORD>(std::size(temp)), temp)) return;
    std::wstring path = temp;
    path += L"FluentShell.LegacyDialogHost.log";
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t line[1024]{};
    swprintf_s(line, L"%02u:%02u:%02u.%03u %s\r\n",
        time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, message.c_str());
    char utf8[4096]{};
    const int bytes = WideCharToMultiByte(
        CP_UTF8, 0, line, -1, utf8, static_cast<int>(std::size(utf8)), nullptr, nullptr);
    if (bytes > 1) {
        DWORD written = 0;
        WriteFile(file, utf8, static_cast<DWORD>(bytes - 1), &written, nullptr);
    }
    CloseHandle(file);
}

void ReportResult(const wchar_t* source, int button, BOOL verification = FALSE) {
    wchar_t text[256]{};
    swprintf_s(text, L"%s result: button=%d, verification=%s",
        source, button, verification ? L"true" : L"false");
    SetWindowTextW(gResult, text);
    AppendLog(text);
}

void ShowClassicMessageBox() {
    const int result = MessageBoxW(gMain,
        L"This standard MessageBox is translated by the out-of-process WinUI renderer.\n\n"
        L"Its result must still return to this native Win32 process.",
        L"Legacy Dialog Host",
        MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
    ReportResult(L"MessageBox", result);
}

void ShowTaskDialog() {
    TASKDIALOGCONFIG config{sizeof(config)};
    config.hwndParent = gMain;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
    config.pszWindowTitle = L"System Protection";
    config.pszMainIcon = TD_WARNING_ICON;
    config.pszMainInstruction = L"Do you want to turn on system protection for this drive?";
    config.pszContent = L"System protection keeps copies of system files and settings so you can restore your PC if something goes wrong.";
    config.pszFooter = L"The selected button and verification state are logged by the native host.";
    config.pszVerificationText = L"Don't show this again";
    config.nDefaultButton = IDYES;
    int button = 0;
    BOOL verification = FALSE;
    const HRESULT hr = TaskDialogIndirect(&config, &button, nullptr, &verification);
    if (FAILED(hr)) button = static_cast<int>(hr);
    ReportResult(L"TaskDialog", button, verification);
}

void ShowSaveConfirm() {
    TASKDIALOG_BUTTON buttons[] = {
        {100, L"&Save"},
        {101, L"Do&n't Save"},
        {IDCANCEL, L"Cancel"},
    };
    TASKDIALOGCONFIG config{sizeof(config)};
    config.hwndParent = gMain;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.pButtons = buttons;
    config.cButtons = static_cast<UINT>(std::size(buttons));
    config.nDefaultButton = 100;
    config.pszWindowTitle = L"Microsoft Management Console";
    config.pszMainIcon = TD_WARNING_ICON;
    config.pszMainInstruction = L"Do you want to save console settings to Console1?";
    config.pszContent = L"If you do not save the settings, your changes will be lost.";
    int button = 0;
    const HRESULT hr = TaskDialogIndirect(&config, &button, nullptr, nullptr);
    if (FAILED(hr)) button = static_cast<int>(hr);
    ReportResult(L"SaveConfirm", button);
}

LRESULT CALLBACK UnsupportedWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(dc, &rect, reinterpret_cast<HBRUSH>(COLOR_INFOBK + 1));
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, L"Unsupported custom HWND\nTranslation must restore native UI",
            -1, &rect, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

HWND AddControl(
    DWORD extendedStyle,
    const wchar_t* className,
    const wchar_t* text,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    HWND parent,
    int id) {
    return CreateWindowExW(extendedStyle, className, text, WS_CHILD | WS_VISIBLE | style,
        x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
}

void CreateDemoControls(HWND window) {
    AddControl(0, L"BUTTON", L"&MessageBox", BS_PUSHBUTTON | WS_TABSTOP,
        20, 20, 180, 34, window, IdMessageBox);
    AddControl(0, L"BUTTON", L"&TaskDialog", BS_PUSHBUTTON | WS_TABSTOP,
        212, 20, 180, 34, window, IdTaskDialog);
    AddControl(0, L"BUTTON", L"MMC &Save Confirm", BS_PUSHBUTTON | WS_TABSTOP,
        404, 20, 180, 34, window, IdSaveConfirm);

    AddControl(0, L"STATIC", L"Editable text", SS_LEFT,
        20, 72, 130, 22, window, 0);
    AddControl(WS_EX_CLIENTEDGE, L"EDIT", L"Native edit value",
        ES_AUTOHSCROLL | WS_TABSTOP, 152, 68, 260, 28, window, IdEdit);
    AddControl(0, L"BUTTON", L"Native &check state", BS_AUTOCHECKBOX | WS_TABSTOP,
        430, 68, 190, 28, window, IdCheck);

    AddControl(0, L"STATIC", L"ComboBox", SS_LEFT,
        20, 116, 100, 22, window, 0);
    HWND combo = AddControl(0, L"COMBOBOX", L"",
        CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
        152, 110, 260, 160, window, IdCombo);
    for (const wchar_t* item : {L"Alpha", L"Beta", L"Gamma"}) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
    SendMessageW(combo, CB_SETCURSEL, 1, 0);

    AddControl(0, L"STATIC", L"ListBox", SS_LEFT,
        20, 158, 100, 22, window, 0);
    HWND list = AddControl(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        LBS_NOTIFY | LBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP,
        152, 154, 260, 96, window, IdList);
    for (const wchar_t* item : {L"Mercury", L"Venus", L"Earth", L"Mars"}) {
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
    SendMessageW(list, LB_SETCURSEL, 2, 0);

    AddControl(0, L"BUTTON", L"&Veto window close", BS_AUTOCHECKBOX | WS_TABSTOP,
        430, 112, 190, 28, window, IdCloseVeto);
    AddControl(0, L"BUTTON", L"Create &unsupported child", BS_PUSHBUTTON | WS_TABSTOP,
        430, 154, 220, 34, window, IdCreateUnsupported);

    AddControl(0, L"BUTTON", L"Live native status", BS_GROUPBOX,
        10, 264, 680, 92, window, 0);
    gOracle = AddControl(0, L"STATIC", L"Native oracle tick: 0", SS_LEFT,
        24, 286, 300, 24, window, IdOracle);
    gProgress = AddControl(0, PROGRESS_CLASSW, L"", PBS_SMOOTH,
        350, 282, 320, 24, window, IdProgress);
    SendMessageW(gProgress, PBM_SETRANGE32, 0, 100);
    SendMessageW(gProgress, PBM_SETPOS, 0, 0);
    gResult = AddControl(WS_EX_CLIENTEDGE, L"STATIC", L"Dialog result: not run",
        SS_LEFT | SS_CENTERIMAGE, 24, 316, 646, 28, window, IdResult);
    AddControl(0, L"STATIC",
        L"The timer text is updated by the native app every second. Creating the custom child "
        L"must trigger whole-window fallback, never a hybrid surface.",
        SS_LEFT, 20, 360, 650, 52, window, 0);
}

LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        CreateDemoControls(window);
        SetTimer(window, kOracleTimer, 1000, nullptr);
        AppendLog(L"host created");
        return 0;
    case WM_TIMER:
        if (wParam == kOracleTimer) {
            wchar_t text[128]{};
            swprintf_s(text, L"Native oracle tick: %llu", ++gOracleTick);
            SetWindowTextW(gOracle, text);
            SendMessageW(gProgress, PBM_SETPOS,
                static_cast<WPARAM>((gOracleTick * 10) % 101), 0);
            if ((gOracleTick % 10) == 0) AppendLog(text);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IdMessageBox:
            if (HIWORD(wParam) == BN_CLICKED) ShowClassicMessageBox();
            break;
        case IdTaskDialog:
            if (HIWORD(wParam) == BN_CLICKED) ShowTaskDialog();
            break;
        case IdSaveConfirm:
            if (HIWORD(wParam) == BN_CLICKED) ShowSaveConfirm();
            break;
        case IdCreateUnsupported:
            if (HIWORD(wParam) == BN_CLICKED && !IsWindow(gUnsupported)) {
                gUnsupported = CreateWindowExW(WS_EX_CLIENTEDGE, kUnsupportedClass, L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    430, 202, 240, 58, window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(1200)),
                    GetModuleHandleW(nullptr), nullptr);
                AppendLog(L"dynamic unsupported child created");
                SetWindowTextW(gResult, L"Unsupported child created: expect native fallback");
            }
            break;
        case IdMenuResetProgress:
            gOracleTick = 0;
            SetWindowTextW(gOracle, L"Native oracle tick: 0");
            SendMessageW(gProgress, PBM_SETPOS, 0, 0);
            SetWindowTextW(gResult, L"Menu command: progress reset");
            AppendLog(L"menu command reset progress");
            break;
        case IdMenuExit:
            PostMessageW(window, WM_CLOSE, 0, 0);
            break;
        }
        return 0;
    case WM_CLOSE:
        if (IsDlgButtonChecked(window, IdCloseVeto) == BST_CHECKED) {
            SetWindowTextW(gResult, L"Close vetoed by native WM_CLOSE handler");
            AppendLog(L"WM_CLOSE vetoed");
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kOracleTimer);
        AppendLog(L"host destroyed");
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    WNDCLASSEXW customClass{sizeof(customClass)};
    customClass.lpfnWndProc = UnsupportedWndProc;
    customClass.hInstance = instance;
    customClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    customClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_INFOBK + 1);
    customClass.lpszClassName = kUnsupportedClass;
    RegisterClassExW(&customClass);

    WNDCLASSEXW hostClass{sizeof(hostClass)};
    hostClass.lpfnWndProc = WndProc;
    hostClass.hInstance = instance;
    hostClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    hostClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    hostClass.lpszClassName = kHostClass;
    RegisterClassExW(&hostClass);

    HMENU menuBar = CreateMenu();
    HMENU fileMenu = CreatePopupMenu();
    HMENU toolsMenu = CreatePopupMenu();
    AppendMenuW(fileMenu, MF_STRING, IdMessageBox, L"&MessageBox\tCtrl+M");
    AppendMenuW(fileMenu, MF_STRING, IdTaskDialog, L"&TaskDialog\tCtrl+T");
    AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(toolsMenu, MF_STRING, IdMenuResetProgress, L"&Reset progress");
    AppendMenuW(fileMenu, MF_POPUP,
        reinterpret_cast<UINT_PTR>(toolsMenu), L"&Tools");
    AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fileMenu, MF_STRING, IdMenuExit, L"E&xit");
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), L"&File");

    gMain = CreateWindowExW(0, hostClass.lpszClassName, L"AnyFluent Win32 Translation Oracle",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 720, 500,
        nullptr, menuBar, instance, nullptr);
    ShowWindow(gMain, show);
    UpdateWindow(gMain);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
