#include <windows.h>
#include <commctrl.h>

#include <string>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr wchar_t kHostClass[] = L"FluentShell.LegacyDialogHost";
constexpr wchar_t kUnsupportedClass[] = L"FluentShell.UnsupportedCustomChild";
constexpr wchar_t kMdiFrameClass[] = L"FluentShell.LegacyMdiFrame";
constexpr wchar_t kMdiChildClass[] = L"FluentShell.LegacyMdiChild";
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
    IdTree = 1110,
    IdTrackbar = 1111,
    IdTrackValue = 1112,
    IdItemList = 1113,
    IdMenuResetProgress = 1201,
    IdMenuExit = 1202,
    IdMdiNewWindow = 1301,
    IdMdiTile = 1302,
    IdMdiChildPing = 1401,
    IdMdiChildEdit = 1402,
    IdMdiChildStatus = 1403,
};

HWND gMain = nullptr;
HWND gOracle = nullptr;
HWND gResult = nullptr;
HWND gUnsupported = nullptr;
HWND gProgress = nullptr;
HWND gTree = nullptr;
HWND gTrackbar = nullptr;
HWND gTrackValue = nullptr;
HWND gListView = nullptr;
HIMAGELIST gItemIcons = nullptr;
HWND gMdiClient = nullptr;
int gMdiChildSerial = 0;
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

HTREEITEM AddTreeItem(HWND tree, HTREEITEM parent, const wchar_t* text) {
    TVINSERTSTRUCTW insert{};
    insert.hParent = parent;
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    insert.item.pszText = const_cast<LPWSTR>(text);
    insert.item.iImage = 0;
    insert.item.iSelectedImage = 1;
    const HTREEITEM item = reinterpret_cast<HTREEITEM>(
        SendMessageW(tree, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&insert)));
    // The common control keeps the selected-state icon only when it is assigned to
    // an existing item, so the open/closed pair is set here rather than at insert.
    if (item) {
        TVITEMW selected{};
        selected.mask = TVIF_HANDLE | TVIF_SELECTEDIMAGE;
        selected.hItem = item;
        selected.iSelectedImage = 1;
        SendMessageW(tree, TVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&selected));
    }
    return item;
}

// Three system icons are enough to prove that per-item imagery survives the
// boundary: a closed and an open state for the tree, and a third for the list.
HIMAGELIST CreateItemIconList() {
    HIMAGELIST images = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 3, 0);
    if (!images) return nullptr;
    for (const wchar_t* icon : { IDI_APPLICATION, IDI_INFORMATION, IDI_WARNING }) {
        if (HICON handle = LoadIconW(nullptr, icon)) ImageList_AddIcon(images, handle);
    }
    return images;
}

void CreateItemListView(HWND window) {
    gListView = AddControl(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_EDITLABELS | WS_TABSTOP,
        20, 600, 660, 170, window, IdItemList);
    if (!gListView) return;
    ListView_SetImageList(gListView, gItemIcons, LVSIL_SMALL);
    const wchar_t* columns[] = { L"Name", L"State" };
    const int widths[] = { 280, 340 };
    for (int index = 0; index < 2; ++index) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<LPWSTR>(columns[index]);
        column.cx = widths[index];
        SendMessageW(gListView, LVM_INSERTCOLUMNW, index, reinterpret_cast<LPARAM>(&column));
    }
    const wchar_t* rows[][2] = {
        { L"Local Disk (C:)", L"Ready" },
        { L"Recovery", L"Healthy" },
        { L"Removable", L"No media" },
    };
    for (int row = 0; row < 3; ++row) {
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = row;
        item.iImage = row % 3;
        item.pszText = const_cast<LPWSTR>(rows[row][0]);
        SendMessageW(gListView, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
        LVITEMW state{};
        state.mask = LVIF_TEXT;
        state.iItem = row;
        state.iSubItem = 1;
        state.pszText = const_cast<LPWSTR>(rows[row][1]);
        SendMessageW(gListView, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&state));
    }
    ListView_SetItemState(gListView, 0, LVIS_SELECTED | LVIS_FOCUSED,
        LVIS_SELECTED | LVIS_FOCUSED);
}

void ReportTrackbarValue() {
    wchar_t text[128]{};
    swprintf_s(text, L"Native trackbar value: %d",
        static_cast<int>(SendMessageW(gTrackbar, TBM_GETPOS, 0, 0)));
    SetWindowTextW(gTrackValue, text);
}

// A textual, iconless tree and a plain trackbar: the two controls whose bounded
// adapters have to prove that a projected gesture reaches the native control and
// that the native control's own notification comes back to this process.
void CreateHierarchyControls(HWND window) {
    gItemIcons = CreateItemIconList();
    AddControl(0, L"STATIC", L"TreeView (native hierarchy, icons, F2 renames)", SS_LEFT,
        20, 418, 320, 20, window, 0);
    gTree = AddControl(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
        TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS |
        TVS_EDITLABELS | WS_TABSTOP,
        20, 440, 320, 150, window, IdTree);
    SendMessageW(gTree, TVM_SETIMAGELIST, TVSIL_NORMAL,
        reinterpret_cast<LPARAM>(gItemIcons));
    const HTREEITEM root = AddTreeItem(gTree, TVI_ROOT, L"Console Root");
    const HTREEITEM services = AddTreeItem(gTree, root, L"Services and Applications");
    AddTreeItem(gTree, services, L"Local Services");
    AddTreeItem(gTree, services, L"WMI Control");
    const HTREEITEM viewer = AddTreeItem(gTree, root, L"Event Viewer");
    AddTreeItem(gTree, viewer, L"Application");
    AddTreeItem(gTree, viewer, L"System");
    SendMessageW(gTree, TVM_EXPAND, TVE_EXPAND, reinterpret_cast<LPARAM>(root));
    SendMessageW(gTree, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(services));

    AddControl(0, L"STATIC", L"Trackbar (native range 0-20)", SS_LEFT,
        360, 418, 320, 20, window, 0);
    gTrackbar = AddControl(0, TRACKBAR_CLASSW, L"",
        TBS_AUTOTICKS | WS_TABSTOP, 360, 440, 320, 40, window, IdTrackbar);
    SendMessageW(gTrackbar, TBM_SETRANGE, TRUE, MAKELPARAM(0, 20));
    SendMessageW(gTrackbar, TBM_SETLINESIZE, 0, 1);
    SendMessageW(gTrackbar, TBM_SETPAGESIZE, 0, 5);
    SendMessageW(gTrackbar, TBM_SETTICFREQ, 5, 0);
    SendMessageW(gTrackbar, TBM_SETPOS, TRUE, 7);
    gTrackValue = AddControl(0, L"STATIC", L"Native trackbar value: 7",
        SS_LEFT | SS_CENTERIMAGE, 360, 486, 320, 24, window, IdTrackValue);
    AddControl(0, L"STATIC", L"Report ListView (icons, F2 renames, ! is refused)", SS_LEFT,
        20, 578, 400, 20, window, 0);
    CreateItemListView(window);
}

LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        CreateDemoControls(window);
        CreateHierarchyControls(window);
        SetTimer(window, kOracleTimer, 1000, nullptr);
        AppendLog(L"host created");
        return 0;
    // A projected trackbar move arrives here as the control's own scroll
    // notification, so the native host is the one that reports the new value.
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lParam) == gTrackbar) {
            ReportTrackbarValue();
            if (LOWORD(wParam) == SB_THUMBPOSITION || LOWORD(wParam) == SB_ENDSCROLL) {
                wchar_t text[128]{};
                swprintf_s(text, L"Trackbar moved to %d by %s",
                    static_cast<int>(SendMessageW(gTrackbar, TBM_GETPOS, 0, 0)),
                    LOWORD(wParam) == SB_ENDSCROLL ? L"release" : L"thumb position");
                AppendLog(text);
            }
        }
        return 0;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (!header) break;
        // In-place renaming is the application's decision, twice: once when the
        // session may start and once when the new text may be kept.  A label that
        // starts with '!' is refused here, which is what a projected rename has to
        // report back instead of pretending it succeeded.
        if (header->code == TVN_BEGINLABELEDITW || header->code == LVN_BEGINLABELEDITW) {
            AppendLog(L"label edit session started");
            return FALSE;
        }
        if (header->code == TVN_ENDLABELEDITW) {
            const auto* info = reinterpret_cast<const NMTVDISPINFOW*>(lParam);
            if (!info || !info->item.pszText) return FALSE;
            if (info->item.pszText[0] == L'!') {
                AppendLog(std::wstring(L"tree rename refused: ") + info->item.pszText);
                return FALSE;
            }
            AppendLog(std::wstring(L"tree renamed to: ") + info->item.pszText);
            return TRUE;
        }
        if (header->code == LVN_ENDLABELEDITW) {
            const auto* info = reinterpret_cast<const NMLVDISPINFOW*>(lParam);
            if (!info || !info->item.pszText) return FALSE;
            if (info->item.pszText[0] == L'!') {
                AppendLog(std::wstring(L"list rename refused: ") + info->item.pszText);
                return FALSE;
            }
            AppendLog(std::wstring(L"list renamed to: ") + info->item.pszText);
            return TRUE;
        }
        if (header->hwndFrom == gTree &&
            (header->code == TVN_SELCHANGEDW || header->code == TVN_ITEMEXPANDEDW)) {
            wchar_t label[128]{};
            TVITEMW item{};
            item.mask = TVIF_HANDLE | TVIF_TEXT;
            item.hItem = reinterpret_cast<HTREEITEM>(
                SendMessageW(gTree, TVM_GETNEXTITEM, TVGN_CARET, 0));
            item.pszText = label;
            item.cchTextMax = static_cast<int>(std::size(label));
            if (item.hItem &&
                SendMessageW(gTree, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item))) {
                wchar_t text[256]{};
                swprintf_s(text, L"Tree %s: %s",
                    header->code == TVN_SELCHANGEDW ? L"selection" : L"expansion", label);
                SetWindowTextW(gResult, text);
                AppendLog(text);
            }
        }
        return 0;
    }
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

// --- MDI oracle -------------------------------------------------------------
//
// A real MDI frame: a frame window owning an MDIClient, which owns child frames
// created through WM_MDICREATE.  Each child hosts ordinary controls and reports
// what it observed, so a projected child proves a gesture reached the native
// child window rather than the frame.

LRESULT CALLBACK MdiChildWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        AddControl(0, L"BUTTON", L"&Ping this child", BS_PUSHBUTTON | WS_TABSTOP,
            12, 12, 150, 30, window, IdMdiChildPing);
        AddControl(WS_EX_CLIENTEDGE, L"EDIT", L"child edit",
            ES_AUTOHSCROLL | WS_TABSTOP, 12, 52, 220, 26, window, IdMdiChildEdit);
        AddControl(0, L"STATIC", L"No child command yet", SS_LEFT | SS_CENTERIMAGE,
            12, 88, 260, 24, window, IdMdiChildStatus);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == IdMdiChildPing && HIWORD(wParam) == BN_CLICKED) {
            wchar_t caption[128]{};
            GetWindowTextW(window, caption, static_cast<int>(std::size(caption)));
            wchar_t text[256]{};
            swprintf_s(text, L"%s answered its own button", caption);
            SetWindowTextW(GetDlgItem(window, IdMdiChildStatus), text);
            AppendLog(text);
            return 0;
        }
        break;
    case WM_MDIACTIVATE: {
        wchar_t caption[128]{};
        GetWindowTextW(window, caption, static_cast<int>(std::size(caption)));
        wchar_t text[256]{};
        swprintf_s(text, L"MDI %s: %s",
            reinterpret_cast<HWND>(lParam) == window ? L"activated" : L"deactivated", caption);
        AppendLog(text);
        break;
    }
    default:
        break;
    }
    return DefMDIChildProc(window, message, wParam, lParam);
}

HWND CreateMdiChildWindow(const wchar_t* title, int offset) {
    MDICREATESTRUCTW create{};
    create.szClass = kMdiChildClass;
    create.szTitle = title;
    create.hOwner = GetModuleHandleW(nullptr);
    create.x = 20 + offset;
    create.y = 20 + offset;
    create.cx = 340;
    create.cy = 200;
    create.style = 0;
    return reinterpret_cast<HWND>(SendMessageW(
        gMdiClient, WM_MDICREATE, 0, reinterpret_cast<LPARAM>(&create)));
}

LRESULT CALLBACK MdiFrameWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        CLIENTCREATESTRUCT client{};
        client.hWindowMenu = nullptr;
        client.idFirstChild = 50000;
        gMdiClient = CreateWindowExW(0, L"MDIClient", nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(0xCAC),
            GetModuleHandleW(nullptr), &client);
        if (!gMdiClient) return -1;
        CreateMdiChildWindow(L"Document 1", 0);
        CreateMdiChildWindow(L"Document 2", 40);
        AppendLog(L"MDI frame created");
        return 0;
    }
    case WM_SIZE: {
        RECT client{};
        GetClientRect(window, &client);
        MoveWindow(gMdiClient, 0, 0, client.right, client.bottom, TRUE);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IdMdiNewWindow: {
            wchar_t title[64]{};
            swprintf_s(title, L"Document %d", 3 + gMdiChildSerial++);
            CreateMdiChildWindow(title, 80 + 20 * gMdiChildSerial);
            return 0;
        }
        case IdMdiTile:
            SendMessageW(gMdiClient, WM_MDITILE, MDITILE_HORIZONTAL, 0);
            return 0;
        case IdMenuExit:
            PostMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        default:
            break;
        }
        break;
    case WM_DESTROY:
        AppendLog(L"MDI frame destroyed");
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefFrameProcW(window, gMdiClient, message, wParam, lParam);
}

int RunMdiOracle(HINSTANCE instance, int show) {
    WNDCLASSEXW childClass{sizeof(childClass)};
    childClass.lpfnWndProc = MdiChildWndProc;
    childClass.hInstance = instance;
    childClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    childClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    childClass.lpszClassName = kMdiChildClass;
    RegisterClassExW(&childClass);

    WNDCLASSEXW frameClass{sizeof(frameClass)};
    frameClass.lpfnWndProc = MdiFrameWndProc;
    frameClass.hInstance = instance;
    frameClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    frameClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_APPWORKSPACE + 1);
    frameClass.lpszClassName = kMdiFrameClass;
    RegisterClassExW(&frameClass);

    HMENU menuBar = CreateMenu();
    HMENU fileMenu = CreatePopupMenu();
    AppendMenuW(fileMenu, MF_STRING, IdMdiNewWindow, L"&New window");
    AppendMenuW(fileMenu, MF_STRING, IdMdiTile, L"&Tile horizontally");
    AppendMenuW(fileMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fileMenu, MF_STRING, IdMenuExit, L"E&xit");
    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), L"&File");

    HWND frame = CreateWindowExW(0, kMdiFrameClass,
        L"FluentShell MDI Translation Oracle", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 560, nullptr, menuBar, instance, nullptr);
    if (!frame) return 1;
    ShowWindow(frame, show);
    UpdateWindow(frame);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0)) {
        if (!TranslateMDISysAccel(gMdiClient, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR commandLine, int show) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    // The MDI oracle is a separate acceptance surface: one frame, one MDI client,
    // and real MDI child frames with their own controls.
    if (commandLine && wcsstr(commandLine, L"--mdi") != nullptr) {
        return RunMdiOracle(instance, show);
    }

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

    gMain = CreateWindowExW(0, hostClass.lpszClassName, L"FluentShell Win32 Translation Oracle",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 720, 880,
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
