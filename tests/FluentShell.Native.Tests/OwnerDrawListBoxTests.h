#pragma once

// Include after main.cpp's Check helper, inside its test namespace.

// An owner-draw ListBox (the Mouse applet's pointer list) projects its rows as its owner paints
// them: every row's normal face, and the selected face of the selected rows only.
struct OwnerDrawListFixture final {
    HWND root = nullptr;
    HWND list = nullptr;
    bool paint = true;

    static LRESULT CALLBACK Parent(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR id, DWORD_PTR data) {
        auto& fixture = *reinterpret_cast<OwnerDrawListFixture*>(data);
        if (message == WM_MEASUREITEM && lParam) {
            reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight = 20;
            return TRUE;
        }
        if (message == WM_DRAWITEM && lParam && fixture.paint) {
            const auto& draw = *reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (draw.CtlType != ODT_LISTBOX) return DefSubclassProc(window, message, wParam, lParam);
            const bool selected = (draw.itemState & ODS_SELECTED) != 0;
            FillRect(draw.hDC, &draw.rcItem, GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW));
            wchar_t text[64]{};
            SendMessageW(draw.hwndItem, LB_GETTEXT, draw.itemID, reinterpret_cast<LPARAM>(text));
            SetTextColor(draw.hDC, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
            SetBkMode(draw.hDC, TRANSPARENT);
            RECT label = draw.rcItem;
            DrawTextW(draw.hDC, text, -1, &label, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
            return TRUE;
        }
        const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, Parent, id);
        return result;
    }

    explicit OwnerDrawListFixture(DWORD extraStyle = 0) {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        root = CreateWindowExW(0, WC_DIALOG, L"owner-draw-list", WS_POPUP | WS_CAPTION | WS_SYSMENU,
            40, 40, 320, 240, nullptr, nullptr, instance, nullptr);
        if (!root) return;
        SetWindowSubclass(root, Parent, 13, reinterpret_cast<DWORD_PTR>(this));
        list = CreateWindowExW(WS_EX_CLIENTEDGE, L"ListBox", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | extraStyle,
            10, 10, 220, 120, root, reinterpret_cast<HMENU>(1104), instance, nullptr);
        if (!list) return;
        for (const wchar_t* row : { L"Normal Select", L"Help Select", L"Working In Background" })
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row));
        ShowWindow(root, SW_SHOWNOACTIVATE);
    }
    ~OwnerDrawListFixture() { if (root) DestroyWindow(root); }
};

void TestOwnerDrawListBoxes() {
    OwnerDrawListFixture fixture;
    Check(fixture.root && fixture.list, "owner-draw list fixture could not create its control");
    if (!fixture.list) return;
    SendMessageW(fixture.list, LB_SETCURSEL, 1, 0);
    const auto capture = [&](Translation::WindowSnapshot& snapshot, std::wstring& error) {
        Translation::CaptureContext context;
        context.surfaceId = L"77777777-7777-7777-7777-cdcdcdcdcdcd";
        context.generation = 1;
        context.revision = 1;
        return Translation::CaptureWindow(fixture.root, context, snapshot, error);
    };
    const auto listNode = [](const Translation::WindowSnapshot& snapshot) {
        return std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
            [](const Translation::ControlNode& candidate) { return candidate.controlId == 1104; });
    };
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    const bool captured = capture(snapshot, error);
    if (!captured) std::wcerr << L"owner-draw list capture rejection: " << error << L'\n';
    const auto node = listNode(snapshot);
    Check(captured && node != snapshot.nodes.end() && node->kind == Translation::ControlKind::ListBox &&
          node->ownerDrawList && node->ownerDrawList->mode == L"fixed" &&
          node->items == std::vector<std::wstring>{ L"Normal Select", L"Help Select", L"Working In Background" } &&
          node->selectedIndex == 1,
        "an owner-draw list was not captured with its rows and selection");
    if (!captured || node == snapshot.nodes.end() || !node->ownerDrawList) return;
    const auto& rows = node->ownerDrawList->items;
    Check(rows.size() == 3 && std::all_of(rows.begin(), rows.end(), [](const auto& row) {
              return row.normal.imageWidth > 0 && row.normal.imageHeight == 20 &&
                  row.normal.imageData.size() == static_cast<size_t>(row.normal.imageWidth) * 20 * 4;
          }),
        "an owner-draw list row face does not match the row the owner paints");
    Check(rows[0].selected.imageData.empty() && !rows[1].selected.imageData.empty() &&
          rows[2].selected.imageData.empty(),
        "an owner-draw list painted selected faces for rows that are not selected");
    const auto json = Translation::SerializeWindowOpen(L"00112233445566778899aabbccddeeff", snapshot);
    Check(json.find("\"ownerDrawList\":{") != std::string::npos,
        "an owner-draw list's faces were not serialized");
    const auto fingerprint = Translation::SnapshotFingerprint(snapshot);
    SendMessageW(fixture.list, LB_SETCURSEL, 2, 0);
    Translation::WindowSnapshot moved;
    Check(capture(moved, error) && listNode(moved) != moved.nodes.end() && listNode(moved)->ownerDrawList &&
          listNode(moved)->ownerDrawList->items[1].selected.imageData.empty() &&
          !listNode(moved)->ownerDrawList->items[2].selected.imageData.empty() &&
          Translation::SnapshotFingerprint(moved) != fingerprint,
        "a selection move did not move the selected face");

    // An owner that paints nothing has nothing to project: the window stays native.
    fixture.paint = false;
    Translation::WindowSnapshot unpainted;
    Check(!capture(unpainted, error) && error.find(L"owner-draw ListBox") != std::wstring::npos,
        "an owner-draw list whose owner paints nothing was captured");
}
