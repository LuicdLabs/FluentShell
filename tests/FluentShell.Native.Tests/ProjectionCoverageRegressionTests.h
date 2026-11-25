#pragma once

// Included inside main.cpp's native test namespace.
LRESULT CALLBACK StoredWindowContractChange(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR, DWORD_PTR state) {
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    auto* pending = reinterpret_cast<DWORD*>(state);
    if (message == LVM_GETITEMW && *pending != 0) {
        const DWORD change = std::exchange(*pending, 0);
        if (change == LVS_EDITLABELS)
            SetWindowLongPtrW(window, GWL_STYLE, GetWindowLongPtrW(window, GWL_STYLE) | LVS_EDITLABELS);
        else ListView_SetExtendedListViewStyleEx(window, LVS_EX_CHECKBOXES, LVS_EX_CHECKBOXES);
    }
    return result;
}

void TestProjectedNodeBudgetIgnoresHiddenChildren() {
    HWND root = CreateWindowExW(WS_EX_CONTROLPARENT, L"Static", L"hidden-pages",
        WS_OVERLAPPEDWINDOW, 20, 20, 400, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(root != nullptr, "hidden-page budget root was not created");
    if (!root) return;
    for (size_t index = 0; index < Ipc::kMaxNodes * 2; ++index) {
        HWND hidden = CreateWindowExW(0, L"Static", L"hidden page content", WS_CHILD,
            0, 0, 100, 20, root, nullptr, GetModuleHandleW(nullptr), nullptr);
        Check(hidden != nullptr, "hidden-page child was not created");
        if (!hidden) { DestroyWindow(root); return; }
    }
    HWND button = CreateWindowExW(0, L"Button", L"Continue", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        10, 20, 100, 30, root, reinterpret_cast<HMENU>(900), GetModuleHandleW(nullptr), nullptr);
    ShowWindow(root, SW_SHOWNOACTIVATE);
    Translation::CaptureContext context;
    context.surfaceId = L"71717171-1111-2222-3333-444444444444";
    context.generation = context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    const bool captured = Translation::CaptureWindow(root, context, snapshot, error);
    if (!captured) std::wcerr << L"hidden-page capture: " << error << L'\n';
    Check(button && captured && snapshot.nodes.size() == 1 && snapshot.nodes[0].hwnd == button &&
        snapshot.nodes[0].tabStop && snapshot.nodes[0].tabIndex == 0,
        "hidden HWNDs exhausted the projected-node budget or changed native tab traversal");
    DestroyWindow(root);

    root = CreateWindowExW(0, L"Static", L"projected-node-cap", WS_OVERLAPPEDWINDOW,
        20, 20, 400, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(root != nullptr, "projected-node cap root was not created");
    if (!root) return;
    for (size_t index = 0; index < Ipc::kMaxNodes; ++index) {
        HWND label = CreateWindowExW(0, L"Static", L"Visible", WS_CHILD | WS_VISIBLE,
            10, 10, 100, 20, root, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!label) { Check(false, "projected-node cap child was not created"); DestroyWindow(root); return; }
    }
    ShowWindow(root, SW_SHOWNOACTIVATE);
    Check(Translation::CaptureWindow(root, context, snapshot, error) && snapshot.nodes.size() == Ipc::kMaxNodes,
        "the exact projected-node cap was rejected");
    HWND extra = CreateWindowExW(0, L"Static", L"Overflow", WS_CHILD | WS_VISIBLE,
        10, 10, 100, 20, root, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(extra && !Translation::CaptureWindow(root, context, snapshot, error) &&
        error.find(L"512 projected node") != std::wstring::npos,
        "a visible node beyond the wire cap was admitted");
    DestroyWindow(root);
}

void TestFixedWidthTabCapture() {
    for (const DWORD extraStyle : { DWORD{0}, DWORD{TCS_MULTILINE | TCS_RAGGEDRIGHT} }) {
        HWND root = CreateWindowExW(0, L"Static", L"fixed-width-tabs", WS_OVERLAPPEDWINDOW,
            20, 20, 440, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        HWND tab = root ? CreateWindowExW(0, WC_TABCONTROLW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_FIXEDWIDTH | extraStyle,
            10, 10, 280, 160, root, reinterpret_cast<HMENU>(901), GetModuleHandleW(nullptr), nullptr) : nullptr;
        Check(root && tab, "fixed-width TabControl fixture was not created");
        if (!root || !tab) { if (root) DestroyWindow(root); continue; }
        for (const wchar_t* label : { L"Short", L"Long tab label", L"Last" }) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<LPWSTR>(label);
            const int index = TabCtrl_GetItemCount(tab);
            Check(SendMessageW(tab, TCM_INSERTITEMW, index, reinterpret_cast<LPARAM>(&item)) == index,
                "fixed-width tab item insertion failed");
        }
        TabCtrl_SetItemSize(tab, 88, 28);
        TabCtrl_SetCurSel(tab, 1);
        ShowWindow(root, SW_SHOWNOACTIVATE);
        Translation::ControlKind kind{};
        std::wstring error;
        Check(Translation::ClassifyControl(tab, kind, error) && kind == Translation::ControlKind::TabControl,
            "fixed-width tabs were rejected by the adapter registry");
        Translation::ControlNode detail;
        detail.kind = Translation::ControlKind::TabControl;
        detail.style = static_cast<DWORD>(GetWindowLongPtrW(tab, GWL_STYLE));
        const bool captured = Translation::CaptureControlDetail(tab, detail, error);
        Check(captured && detail.items.size() == 3 && detail.itemRects.size() == 3 && detail.selectedIndex == 1,
            "fixed-width tab labels, geometry or selection were lost");
        if (captured && detail.itemRects.size() == 3) {
            const auto width = detail.itemRects[0].right - detail.itemRects[0].left;
            Check(width > 0 && std::all_of(detail.itemRects.begin(), detail.itemRects.end(), [width](const RECT& rect) {
                    return rect.right - rect.left == width;
                }), "fixed-width tabs did not retain their native equal-width layout");
        }
        Check(Translation::SelectTabControl(root, tab, 901, 2, 3) && TabCtrl_GetCurSel(tab) == 2,
            "fixed-width tab selection was not routed natively");
        DestroyWindow(root);
    }
}

void TestVerticalProgressCapture() {
    HWND root = CreateWindowExW(0, L"Static", L"vertical-progress", WS_OVERLAPPEDWINDOW,
        20, 20, 300, 280, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND progress = root ? CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | PBS_VERTICAL, 10, 10, 24, 160, root,
        reinterpret_cast<HMENU>(902), GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(root && progress, "vertical progress fixture was not created");
    if (!root || !progress) { if (root) DestroyWindow(root); return; }
    SendMessageW(progress, PBM_SETRANGE32, 10, 210);
    SendMessageW(progress, PBM_SETPOS, 110, 0);
    ShowWindow(root, SW_SHOWNOACTIVATE);
    Translation::CaptureContext context;
    context.surfaceId = L"72727272-1111-2222-3333-444444444444";
    context.generation = context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    Check(Translation::CaptureWindow(root, context, snapshot, error) && snapshot.nodes.size() == 1 &&
        snapshot.nodes[0].kind == Translation::ControlKind::ProgressBar &&
        (snapshot.nodes[0].style & PBS_VERTICAL) != 0 && snapshot.nodes[0].minimum == 10 &&
        snapshot.nodes[0].maximum == 210 && snapshot.nodes[0].position == 110 && !snapshot.nodes[0].indeterminate,
        "vertical progress did not preserve its native range and orientation");
    SetWindowLongPtrW(progress, GWL_STYLE, GetWindowLongPtrW(progress, GWL_STYLE) | PBS_MARQUEE);
    Check(Translation::CaptureWindow(root, context, snapshot, error) && snapshot.nodes.size() == 1 &&
        snapshot.nodes[0].indeterminate && (snapshot.nodes[0].style & PBS_VERTICAL) != 0,
        "vertical marquee progress was rejected or lost its state");
    DestroyWindow(root);
}

void TestLargeStoredReportListWindow() {
    ListActivationCommonControls controls(ICC_LISTVIEW_CLASSES);
    Check(static_cast<bool>(controls), "stored row identity common-controls v6 context failed");
    if (!controls) return;
    HWND root = CreateWindowExW(0, L"Static", L"large-stored-report", WS_OVERLAPPEDWINDOW,
        20, 20, 480, 320, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    HWND list = root ? CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT,
        10, 10, 400, 220, root, reinterpret_cast<HMENU>(903), GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(root && list, "large stored ListView fixture was not created");
    if (!root || !list) { if (root) DestroyWindow(root); return; }
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = const_cast<LPWSTR>(L"Name");
    column.cx = 220;
    Check(ListView_InsertColumn(list, 0, &column) == 0, "large stored ListView column insertion failed");
    for (int index = 0; index < 10000; ++index) {
        std::wstring text = L"Row " + std::to_wstring(index);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = index;
        item.pszText = text.data();
        if (ListView_InsertItem(list, &item) != index) {
            Check(false, "large stored ListView item insertion failed"); DestroyWindow(root); return;
        }
    }
    ListView_SetItemState(list, 9000, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ShowWindow(root, SW_SHOWNOACTIVATE);
    Translation::ControlNode node;
    node.kind = Translation::ControlKind::ListView;
    std::wstring error;
    const bool captured = Translation::CaptureControlDetail(list, node, error);
    if (!captured) std::wcerr << L"large stored list: " << error << L'\n';
    Check(captured && node.itemCount == 10000 && node.itemWindowStart == 0 && node.rows.size() >= 48 && node.rows.size() <= 192 &&
        node.items.size() == node.rows.size() && node.selectedIndices == std::vector<int>{9000} &&
        node.focusedIndex == 9000 && node.itemNativeIds.empty() && !node.itemActivationSupported,
        "large stored report was not captured with bounded rows and absolute selection");
    Check(!captured || node.itemWindowNativeIds.size() == node.rows.size(),
        "stored row identities were not retained for revision checks");
    if (captured && !node.itemWindowNativeIds.empty()) {
        Translation::WindowSnapshot before;
        before.nodes.push_back(node);
        const auto fingerprint = Translation::SnapshotFingerprint(before);
        Check(ListView_DeleteItem(list, 0), "same-label stored row removal failed");
        LVITEMW replacement{};
        replacement.mask = LVIF_TEXT;
        replacement.iItem = 0;
        replacement.pszText = const_cast<LPWSTR>(L"Row 0");
        Check(ListView_InsertItem(list, &replacement) == 0, "same-label stored row replacement failed");
        Translation::ControlNode replaced;
        replaced.kind = Translation::ControlKind::ListView;
        Check(Translation::CaptureControlDetail(list, replaced, error) && replaced.rows == node.rows &&
            replaced.itemWindowNativeIds != node.itemWindowNativeIds,
            "same-label stored replacement did not preserve text while changing identity");
        before.nodes[0] = replaced;
        Check(Translation::SnapshotFingerprint(before) != fingerprint,
            "same-label stored row replacement was omitted from the action witness");
        const auto wire = Translation::SerializeWindowOpen(L"00112233445566778899aabbccddeeff", before);
        Check(wire.find("itemWindowNativeIds") == std::string::npos,
            "private stored row identities leaked into the protocol");
    }
    Check(Translation::ScrollListViewToTop(list, 5000), "large stored report native scroll failed");
    const bool scrolledCapture = Translation::CaptureControlDetail(list, node, error);
    if (!scrolledCapture || node.itemWindowStart >= 5000 || node.itemWindowStart + node.items.size() <= 5000)
        std::wcerr << L"stored scroll: top=" << ListView_GetTopIndex(list) << L" start=" << node.itemWindowStart
            << L" rows=" << node.items.size() << L" error=" << error << L'\n';
    Check(scrolledCapture && node.itemWindowStart < 5000 &&
        node.itemWindowStart + node.items.size() > 5000 &&
        Translation::ListViewItemLabel(node, 5000) && *Translation::ListViewItemLabel(node, 5000) == L"Row 5000",
        "large stored report row window did not follow the native scroll");
    const auto distantId = static_cast<uint32_t>(SendMessageW(list, LVM_MAPINDEXTOID, 5000, 0));
    Check(Translation::ResolveListViewItemByNativeId(list, distantId) == 5000,
        "stored row identity resolution confused absolute indices with the carried-row cap");
    ListView_SetExtendedListViewStyleEx(list, LVS_EX_CHECKBOXES, LVS_EX_CHECKBOXES);
    Check(!Translation::CaptureControlDetail(list, node, error),
        "large report checkboxes were windowed without a complete state contract");
    ListView_SetExtendedListViewStyleEx(list, LVS_EX_CHECKBOXES, 0);
    SetWindowLongPtrW(list, GWL_STYLE, GetWindowLongPtrW(list, GWL_STYLE) | LVS_EDITLABELS);
    Check(!Translation::CaptureControlDetail(list, node, error),
        "large report label editing was windowed without its full item contract");
    SetWindowLongPtrW(list, GWL_STYLE, GetWindowLongPtrW(list, GWL_STYLE) & ~static_cast<LONG_PTR>(LVS_EDITLABELS));
    for (const DWORD change : { DWORD{LVS_EDITLABELS}, DWORD{LVS_EX_CHECKBOXES} }) {
        DWORD pending = change;
        SetWindowSubclass(list, StoredWindowContractChange, 903, reinterpret_cast<DWORD_PTR>(&pending));
        Check(!Translation::CaptureControlDetail(list, node, error) && pending == 0 &&
            error.find(L"state contract changed") != std::wstring::npos,
            "stored report callbacks changed the windowed state contract without rejection");
        RemoveWindowSubclass(list, StoredWindowContractChange, 903);
        ListView_SetExtendedListViewStyleEx(list, LVS_EX_CHECKBOXES, 0);
        SetWindowLongPtrW(list, GWL_STYLE, GetWindowLongPtrW(list, GWL_STYLE) & ~static_cast<LONG_PTR>(LVS_EDITLABELS));
    }
    DestroyWindow(root);
}
