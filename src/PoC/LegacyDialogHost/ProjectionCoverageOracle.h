#pragma once

// Included inside the host's namespace; this surface uses only native controls.
constexpr wchar_t kCoverageOracleClass[] = L"FluentShell.ProjectionCoverageOracle";
constexpr UINT kCoverageNativeUpdate = WM_APP + 410;
constexpr UINT kCoverageQuery = WM_APP + 411;
constexpr UINT_PTR kCoverageTimer = 2;
constexpr int kCoverageRows = 10000;
constexpr int kCoverageHiddenChildren = 1024;

enum CoverageControlId : int {
    IdCoverageList = 2101,
    IdCoverageTabs = 2102,
    IdCoverageProgress = 2103,
    IdCoverageMarquee = 2104,
    IdCoverageTick = 2105,
    IdCoverageSelection = 2106,
    IdCoverageTabResult = 2107,
    IdCoverageNativeUpdate = 2108,
};

struct ProjectionCoverageState final {
    HWND list = nullptr;
    HWND tabs = nullptr;
    HWND progress = nullptr;
    HWND marquee = nullptr;
    HWND tickLabel = nullptr;
    HWND selectionLabel = nullptr;
    HWND tabLabel = nullptr;
    unsigned long long tick = 0;
    unsigned nativeUpdates = 0;
    int hiddenChildren = 0;
};

void UpdateCoverageResults(ProjectionCoverageState& state) {
    const int selected = ListView_GetNextItem(state.list, -1, LVNI_SELECTED);
    const int tab = TabCtrl_GetCurSel(state.tabs);
    SetWindowTextW(state.selectionLabel, (L"Selected row=" + std::to_wstring(selected)).c_str());
    SetWindowTextW(state.tabLabel, (L"Selected tab=" + std::to_wstring(tab)).c_str());
}

void ApplyCoverageNativeUpdate(ProjectionCoverageState& state) {
    ++state.nativeUpdates;
    const std::wstring updated = L"Native row update " + std::to_wstring(state.nativeUpdates);
    ListView_SetItemText(state.list, 5, 0, const_cast<LPWSTR>(updated.c_str()));
    ListView_SetItemState(state.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(state.list, 7, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(state.list, 7, FALSE);
    TabCtrl_SetCurSel(state.tabs, 1);
    UpdateCoverageResults(state);
    AppendLog(L"coverage native update=" + std::to_wstring(state.nativeUpdates));
}

bool CreateCoverageControls(HWND root, ProjectionCoverageState& state) {
    const auto create = [root](const wchar_t* className, const wchar_t* text, DWORD style,
            int x, int y, int width, int height, int id) {
        const HWND control = CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style,
            x, y, width, height, root, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            GetModuleHandleW(nullptr), nullptr);
        if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return control;
    };
    state.tabs = create(WC_TABCONTROLW, L"Fixed-width tabs", WS_TABSTOP | TCS_FIXEDWIDTH,
        20, 16, 640, 44, IdCoverageTabs);
    state.list = create(WC_LISTVIEWW, L"Stored report rows", WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL,
        20, 76, 640, 304, IdCoverageList);
    state.progress = create(PROGRESS_CLASSW, L"Vertical progress", PBS_VERTICAL,
        724, 100, 24, 240, IdCoverageProgress);
    state.marquee = create(PROGRESS_CLASSW, L"Vertical marquee", PBS_VERTICAL | PBS_MARQUEE,
        814, 100, 24, 240, IdCoverageMarquee);
    state.tickLabel = create(L"Static", L"Native tick=0", 0, 20, 394, 620, 22, IdCoverageTick);
    state.selectionLabel = create(L"Static", L"Selected row=0", 0, 20, 424, 260, 22, IdCoverageSelection);
    state.tabLabel = create(L"Static", L"Selected tab=0", 0, 302, 424, 260, 22, IdCoverageTabResult);
    const HWND progressLabel = create(L"Static", L"Progress", 0, 696, 68, 86, 22, 2120);
    const HWND marqueeLabel = create(L"Static", L"Marquee", 0, 792, 68, 86, 22, 2121);
    const HWND update = create(L"Button", L"Apply native update", WS_TABSTOP | BS_PUSHBUTTON,
        688, 394, 188, 36, IdCoverageNativeUpdate);
    if (!state.tabs || !state.list || !state.progress || !state.marquee || !state.tickLabel ||
        !state.selectionLabel || !state.tabLabel || !progressLabel || !marqueeLabel || !update) return false;
    for (const wchar_t* label : {L"Fixed tab A", L"Fixed tab B", L"Fixed tab C"}) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<LPWSTR>(label);
        const int index = TabCtrl_GetItemCount(state.tabs);
        if (TabCtrl_InsertItem(state.tabs, index, &item) != index) return false;
    }
    TabCtrl_SetItemSize(state.tabs, 160, 28);
    for (int index = 0; index != 2; ++index) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<LPWSTR>(index == 0 ? L"Name" : L"State");
        column.cx = index == 0 ? 410 : 190;
        if (ListView_InsertColumn(state.list, index, &column) != index) return false;
    }
    SendMessageW(state.list, WM_SETREDRAW, FALSE, 0);
    for (int index = 0; index != kCoverageRows; ++index) {
        std::wstring label = L"Row " + std::to_wstring(index);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = index;
        item.pszText = label.data();
        if (ListView_InsertItem(state.list, &item) != index) return false;
        ListView_SetItemText(state.list, index, 1, const_cast<LPWSTR>(L"Ready"));
    }
    ListView_SetItemState(state.list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    SendMessageW(state.list, WM_SETREDRAW, TRUE, 0);
    SendMessageW(state.progress, PBM_SETRANGE32, 10, 210);
    SendMessageW(state.progress, PBM_SETPOS, 110, 0);
    SendMessageW(state.marquee, PBM_SETMARQUEE, TRUE, 40);
    // Hidden property-page implementation HWNDs must not consume projected nodes.
    for (int index = 0; index != kCoverageHiddenChildren; ++index) {
        if (!CreateWindowExW(0, L"Static", L"Hidden page", WS_CHILD, 0, 0, 10, 10,
                root, nullptr, GetModuleHandleW(nullptr), nullptr)) return false;
        ++state.hiddenChildren;
    }
    UpdateCoverageResults(state);
    return SetTimer(root, kCoverageTimer, 1000, nullptr) != 0;
}

LRESULT CALLBACK CoverageOracleWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<ProjectionCoverageState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<ProjectionCoverageState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_CREATE:
        if (!CreateCoverageControls(window, *state)) return -1;
        AppendLog(L"coverage created: rows=10000 hidden=1024 tabs=fixedWidth progress=vertical");
        return 0;
    case WM_TIMER:
        if (wParam == kCoverageTimer) {
            ++state->tick;
            SetWindowTextW(state->tickLabel, (L"Native tick=" + std::to_wstring(state->tick)).c_str());
            SendMessageW(state->progress, PBM_SETPOS, 10 + static_cast<int>((100 + state->tick * 7) % 201), 0);
        }
        return 0;
    case WM_NOTIFY: {
        const auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header && ((header->hwndFrom == state->tabs && header->code == TCN_SELCHANGE) ||
                (header->hwndFrom == state->list && header->code == LVN_ITEMCHANGED)))
            UpdateCoverageResults(*state);
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IdCoverageNativeUpdate) { ApplyCoverageNativeUpdate(*state); return 0; }
        break;
    case kCoverageNativeUpdate:
        ApplyCoverageNativeUpdate(*state);
        return 0;
    case kCoverageQuery:
        // Acceptance reads execute on the window's own GUI thread. This test-only
        // API lets a script compare projected behavior to the canonical controls.
        switch (wParam) {
        case 0: return static_cast<LRESULT>(state->tick);
        case 1: return TabCtrl_GetCurSel(state->tabs);
        case 2: return ListView_GetNextItem(state->list, -1, LVNI_SELECTED);
        case 3: return ListView_GetTopIndex(state->list);
        case 4: return ListView_GetItemCount(state->list);
        case 5: return SendMessageW(state->progress, PBM_GETPOS, 0, 0);
        case 6: return state->hiddenChildren;
        }
        return -1;
    case WM_DESTROY:
        KillTimer(window, kCoverageTimer);
        AppendLog(L"coverage destroyed");
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int RunProjectionCoverageOracle(HINSTANCE instance, int show) {
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = CoverageOracleWndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kCoverageOracleClass;
    if (!RegisterClassExW(&windowClass)) return 1;
    ProjectionCoverageState state;
    const HWND window = CreateWindowExW(0, kCoverageOracleClass, L"FluentShell Projection Coverage Oracle",
        WS_OVERLAPPEDWINDOW, 40, 40, 916, 514, nullptr, nullptr, instance, &state);
    if (!window) return 1;
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG message{};
    int result = 0;
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return result < 0 ? 1 : static_cast<int>(message.wParam);
}
