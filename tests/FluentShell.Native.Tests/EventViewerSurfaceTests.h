#pragma once

// Event Viewer's WinForms result pane: a form-like panel whose own paint is nothing but
// the dialog face, a panel whose children are all hidden, a read-only RichEdit event
// description, and MMC's busy shield, which only mmcndmgr.dll can raise.

namespace EventViewerSurface {

// Fills the client area with the colour kept in GWLP_USERDATA, like a Windows Forms
// panel painting its BackColor.
inline LRESULT CALLBACK FillProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_ERASEBKGND) {
        RECT client{};
        GetClientRect(window, &client);
        const HBRUSH brush = CreateSolidBrush(static_cast<COLORREF>(GetWindowLongPtrW(window, GWLP_USERDATA)));
        if (brush) {
            FillRect(reinterpret_cast<HDC>(wParam), &client, brush);
            DeleteObject(brush);
        }
        return 1;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

inline constexpr auto kPanelClass = L"FluentShellTestFormPanel";

inline bool RegisterPanelClass() {
    WNDCLASSW type{};
    type.lpfnWndProc = FillProc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = kPanelClass;
    return RegisterClassW(&type) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

inline HWND CreatePanel(HWND parent, int x, int y, int width, int height, COLORREF fill, DWORD style = 0) {
    const HWND panel = CreateWindowExW(WS_EX_CONTROLPARENT, kPanelClass, L"",
        WS_CHILD | WS_VISIBLE | style, x, y, width, height, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (panel) SetWindowLongPtrW(panel, GWLP_USERDATA, static_cast<LONG_PTR>(fill));
    return panel;
}

inline void Repaint(HWND window) {
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

} // namespace EventViewerSurface

void TestSurfaceFilledContainers() {
    using namespace EventViewerSurface;
    Check(RegisterPanelClass(), "surface-filled panel class was not registered");
    const HWND root = CreateWindowExW(0, L"Static", L"surface-filled", WS_OVERLAPPEDWINDOW,
        40, 40, 460, 380, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const HWND form = root ? CreatePanel(root, 0, 0, 400, 200, GetSysColor(COLOR_3DFACE)) : nullptr;
    const HWND empty = root ? CreatePanel(root, 0, 220, 180, 80, GetSysColor(COLOR_3DFACE)) : nullptr;
    Check(root && form && empty, "surface-filled panel fixtures were not created");
    if (!root || !form || !empty) {
        if (root) DestroyWindow(root);
        return;
    }
    // Five label/field rows: the uncovered face between them decomposes into more
    // painted rectangles than the pane rule carries, exactly like Event Viewer's
    // General tab.
    std::vector<HWND> fields;
    for (int row = 0; row < 5; ++row) {
        CreateWindowExW(0, L"Static", L"Label:", WS_CHILD | WS_VISIBLE,
            10, 10 + row * 34, 90, 16, form, nullptr, GetModuleHandleW(nullptr), nullptr);
        fields.push_back(CreateWindowExW(0, L"Edit", L"value", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_READONLY,
            120, 8 + row * 34, 150, 22, form, nullptr, GetModuleHandleW(nullptr), nullptr));
    }
    const HWND hiddenChild = CreateWindowExW(0, L"Static", L"hidden", WS_CHILD,
        10, 10, 60, 16, empty, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(root, SW_SHOWNOACTIVATE);
    Repaint(root);

    const auto classify = [](HWND hwnd, Translation::ControlKind& kind, std::wstring& reason) {
        reason.clear();
        return Translation::ClassifyControl(hwnd, kind, reason);
    };
    const auto capture = [](HWND hwnd, Translation::ControlNode& node, std::wstring& reason) {
        node = {};
        node.kind = Translation::ControlKind::PaneContainer;
        node.hwnd = hwnd;
        node.style = static_cast<uint64_t>(GetWindowLongPtrW(hwnd, GWL_STYLE));
        reason.clear();
        return Translation::CaptureControlDetail(hwnd, node, reason);
    };
    Translation::ControlKind kind{};
    std::wstring reason;
    Translation::ControlNode node;
    Check(classify(form, kind, reason) && kind == Translation::ControlKind::PaneContainer,
        "a form panel painting only the dialog face was not admitted as a frame");
    if (kind != Translation::ControlKind::PaneContainer) std::wcerr << L"form panel: " << reason << L'\n';
    Check(capture(form, node, reason) && node.chromeRegions.empty() && node.splits.empty(),
        "a surface-filled form panel carried chrome or offered splits between its fields");

    // Any other fill is the application's own paint, which the pane rule cannot carry.
    SetWindowLongPtrW(form, GWLP_USERDATA, static_cast<LONG_PTR>(RGB(0x20, 0x40, 0x60)));
    Repaint(form);
    Check(!classify(form, kind, reason) && reason.find(L"paints more than") != std::wstring::npos,
        "a form panel with an application colour fill was admitted as a plain frame");
    Check(!capture(form, node, reason), "a colour-filled form panel captured as a plain frame");
    SetWindowLongPtrW(form, GWLP_USERDATA, static_cast<LONG_PTR>(GetSysColor(COLOR_3DFACE)));
    Repaint(form);

    // A themed tab body is #F9F9F9 under the Windows 11 theme: neither the dialog face nor the
    // window background, but the same kind of surface. Wide bands of it around a few controls
    // are not chrome the projection has to carry (nor a reason to refuse the frame).
    const HWND themed = CreatePanel(root, 200, 220, 220, 100, RGB(0xF9, 0xF9, 0xF9));
    if (themed) {
        CreateWindowExW(0, L"Static", L"Label:", WS_CHILD | WS_VISIBLE, 10, 10, 60, 16, themed, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"Edit", L"value", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_READONLY, 120, 60, 90, 22,
            themed, nullptr, GetModuleHandleW(nullptr), nullptr);
        Repaint(root);
    }
    Check(themed && classify(themed, kind, reason) && kind == Translation::ControlKind::PaneContainer,
        "a themed near-white page around a few controls was not admitted as a frame");
    if (themed && kind != Translation::ControlKind::PaneContainer) std::wcerr << L"themed page: " << reason << L'\n';
    Check(themed && capture(themed, node, reason) && node.chromeRegions.empty() && node.splits.empty(),
        "a themed near-white page carried its background as chrome or offered splits");
    if (themed) {
        // A near-white that is not neutral is the application's own paint, which is carried.
        SetWindowLongPtrW(themed, GWLP_USERDATA, static_cast<LONG_PTR>(RGB(0xF9, 0xE8, 0xE0)));
        Repaint(themed);
        Check(classify(themed, kind, reason) && capture(themed, node, reason) && !node.chromeRegions.empty(),
            "a tinted page lost the paint of its own that the projection cannot supply");
        DestroyWindow(themed);
    }

    // A panel whose children are all hidden shows nothing but its face.
    Check(hiddenChild && classify(empty, kind, reason) && kind == Translation::ControlKind::PaneContainer,
        "an empty face-filled panel was not admitted as an empty frame");
    Check(capture(empty, node, reason) && node.chromeRegions.empty() && node.splits.empty(),
        "an empty face-filled panel carried chrome");
    SetWindowLongPtrW(empty, GWLP_USERDATA, static_cast<LONG_PTR>(GetSysColor(COLOR_WINDOW)));
    Repaint(empty);
    Check(classify(empty, kind, reason) && kind == Translation::ControlKind::PaneContainer,
        "an empty panel filled with the window background was not admitted");
    // An empty tab stop is an interactive control that has merely not painted.
    SetWindowLongPtrW(empty, GWL_STYLE, GetWindowLongPtrW(empty, GWL_STYLE) | WS_TABSTOP);
    Check(!classify(empty, kind, reason), "an empty tab-stop surface was admitted as a frame");
    DestroyWindow(root);
}

void TestReadOnlyRichEditAdapter() {
    const HMODULE library = LoadLibraryW(L"msftedit.dll");
    Check(library != nullptr, "msftedit.dll is unavailable");
    if (!library) return;
    const HWND root = CreateWindowExW(0, L"Static", L"rich-edit", WS_OVERLAPPEDWINDOW,
        40, 40, 360, 260, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const HWND description = root ? CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
        10, 10, 300, 80, root, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    const HWND editable = root ? CreateWindowExW(WS_EX_CLIENTEDGE, MSFTEDIT_CLASS, L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE,
        10, 100, 300, 60, root, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(root && description && editable, "RichEdit fixtures were not created");
    if (!root || !description || !editable) {
        if (root) DestroyWindow(root);
        FreeLibrary(library);
        return;
    }
    SetWindowTextW(description, L"The application stopped responding.\r\nSee the details tab.");
    SetWindowTextW(editable, L"editable");
    ShowWindow(root, SW_SHOWNOACTIVATE);

    Translation::ControlKind kind{};
    std::wstring reason;
    Check(Translation::ClassifyControl(description, kind, reason) && kind == Translation::ControlKind::Edit,
        "a read-only plain RichEdit was not admitted as a read-only text box");
    if (kind != Translation::ControlKind::Edit) std::wcerr << L"RichEdit: " << reason << L'\n';
    Translation::ControlNode node;
    node.kind = Translation::ControlKind::Edit;
    node.hwnd = description;
    node.style = static_cast<uint64_t>(GetWindowLongPtrW(description, GWL_STYLE));
    reason.clear();
    Check(Translation::CaptureControlDetail(description, node, reason) && node.readOnly && node.multiline,
        "a read-only RichEdit lost its read-only multiline shape");

    // An editable RichEdit is an edit box while its text is plain; formatting a plain edit box
    // would lose keeps it native.
    reason.clear();
    Check(Translation::ClassifyControl(editable, kind, reason) && kind == Translation::ControlKind::Edit,
        "an editable RichEdit with plain text was not admitted as a text box");
    CHARFORMAT2W editableBold{};
    editableBold.cbSize = sizeof(editableBold);
    editableBold.dwMask = CFM_BOLD;
    editableBold.dwEffects = CFE_BOLD;
    SendMessageW(editable, EM_SETSEL, 0, 4);
    SendMessageW(editable, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&editableBold));
    reason.clear();
    Check(!Translation::ClassifyControl(editable, kind, reason) &&
        reason.find(L"editable RichEdit") != std::wstring::npos,
        "an editable RichEdit with bold text was admitted as a plain text box");

    // RichEdit binds a CJK run and a Latin run to different fonts by itself; that is
    // not formatting a plain text box loses.
    SendMessageW(description, EM_SETLANGOPTIONS, 0,
        SendMessageW(description, EM_GETLANGOPTIONS, 0, 0) | IMF_AUTOFONT);
    SetWindowTextW(description, L"順利排程 2026-10-08T02:20:58Z 重新啟動。");
    reason.clear();
    Check(Translation::ClassifyControl(description, kind, reason) && kind == Translation::ControlKind::Edit,
        "a RichEdit whose only variation is font binding was refused");
    if (kind != Translation::ControlKind::Edit) std::wcerr << L"RichEdit font binding: " << reason << L'\n';

    // A detected link draws and acts as a link.
    SendMessageW(description, EM_AUTOURLDETECT, AURL_ENABLEURL, 0);
    SetWindowTextW(description, L"See https://learn.microsoft.com for details.");
    reason.clear();
    Check(!Translation::ClassifyControl(description, kind, reason),
        "a RichEdit holding a detected link was admitted as plain text");
    SendMessageW(description, EM_AUTOURLDETECT, 0, 0);
    SetWindowTextW(description, L"The application stopped responding.\r\nSee the details tab.");

    // A bold run is formatting a plain text box cannot show: the control is carried as
    // formatted text instead (minor 35), read-only, as paragraphs of runs.
    CHARFORMAT2W bold{};
    bold.cbSize = sizeof(bold);
    bold.dwMask = CFM_BOLD;
    bold.dwEffects = CFE_BOLD;
    SendMessageW(description, EM_SETSEL, 4, 15);
    SendMessageW(description, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&bold));
    SendMessageW(description, EM_SETSEL, 0, 0);
    reason.clear();
    Check(Translation::ClassifyControl(description, kind, reason) && kind == Translation::ControlKind::RichText,
        "a RichEdit with a bold run was not carried as rich text");
    if (kind != Translation::ControlKind::RichText) std::wcerr << L"bold RichEdit: " << reason << L'\n';
    Translation::ControlNode rich;
    rich.kind = Translation::ControlKind::RichText;
    rich.hwnd = description;
    rich.style = static_cast<uint64_t>(GetWindowLongPtrW(description, GWL_STYLE));
    reason.clear();
    Check(Translation::CaptureControlDetail(description, rich, reason) && rich.readOnly && rich.multiline,
        "rich text lost its read-only multiline shape");
    const auto describe = [](const Translation::ControlNode& value) {
        std::wstring text;
        for (const auto& paragraph : value.richParagraphs) {
            for (const auto& run : paragraph.runs) text += (run.bold ? L"[b:" : L"[") + run.text + L"]";
            text += L"|";
        }
        return text;
    };
    Check(describe(rich) == L"[The ][b:application][ stopped responding.]|[See the details tab.]|",
        "rich text did not split into its paragraphs and formatted runs");
    if (describe(rich) != L"[The ][b:application][ stopped responding.]|[See the details tab.]|")
        std::wcerr << L"rich paragraphs: " << describe(rich) << L'\n';

    // Italic, underline, strike-through and a colour travel in the run that holds them.
    CHARFORMAT2W styled{};
    styled.cbSize = sizeof(styled);
    styled.dwMask = CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT | CFM_COLOR;
    styled.dwEffects = CFE_ITALIC | CFE_UNDERLINE | CFE_STRIKEOUT;
    styled.crTextColor = RGB(0x12, 0x34, 0x56);
    SendMessageW(description, EM_SETSEL, 40, 44);
    SendMessageW(description, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&styled));
    SendMessageW(description, EM_SETSEL, 0, 0);
    std::vector<Translation::RichParagraphSnapshot> paragraphs;
    reason.clear();
    Check(Translation::ReadRichText(description, paragraphs, reason) && paragraphs.size() == 2,
        "styled rich text was not read");
    bool foundStyled = false;
    for (const auto& paragraph : paragraphs) {
        for (const auto& run : paragraph.runs) {
            // COLORREF 0x00BBGGRR holds red 0x12, green 0x34, blue 0x56: 0x123456 as RRGGBB.
            if (run.italic && run.underline && run.strike && run.color == 0x123456 && !run.bold &&
                run.text == L"the ") foundStyled = true;
        }
    }
    Check(foundStyled, "italic, underline, strike-through and colour did not reach their run");

    // What the projection cannot draw refuses the whole control: hidden text.
    CHARFORMAT2W hidden{};
    hidden.cbSize = sizeof(hidden);
    hidden.dwMask = CFM_HIDDEN;
    hidden.dwEffects = CFE_HIDDEN;
    SendMessageW(description, EM_SETSEL, 0, 3);
    SendMessageW(description, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&hidden));
    SendMessageW(description, EM_SETSEL, 0, 0);
    reason.clear();
    Check(!Translation::ReadRichText(description, paragraphs, reason) &&
        reason.find(L"hidden") != std::wstring::npos,
        "rich text with hidden text was admitted");
    DestroyWindow(root);
    FreeLibrary(library);
}

// Paints a diagonal stroke: application content no adapter can reproduce.
inline LRESULT CALLBACK PaintedLeafProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        MoveToEx(dc, 0, 0, nullptr);
        LineTo(dc, client.right, client.bottom);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void TestObscuredHostIsNotCaptured() {
    // Print Management lays MMC's result ListView over the HTML host that positions it,
    // leaving two pixels of the host on screen.  Content the capture cannot project
    // inside such a host drops the host instead of refusing the window; a host whose
    // uncovered band could hold a control keeps the ordinary refusal.
    WNDCLASSW leafClass{};
    leafClass.lpfnWndProc = PaintedLeafProc;
    leafClass.hInstance = GetModuleHandleW(nullptr);
    leafClass.lpszClassName = L"FluentShellTestPaintedLeaf";
    leafClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    const bool registered = RegisterClassW(&leafClass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    const HWND root = CreateWindowExW(0, L"Static", L"obscured-host", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        40, 40, 420, 320, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const HWND host = root ? CreateWindowExW(0, L"Static", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        10, 10, 300, 200, root, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    // A tab stop makes the painted leaf interactive, which the capture cannot project (a passive
    // one would be projected as the picture it paints).
    const HWND inner = host && registered ? CreateWindowExW(0, L"FluentShellTestPaintedLeaf", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 20, 20, 100, 60, host, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    const HWND cover = root ? CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | LVS_REPORT,
        12, 12, 296, 192, root, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(root && host && inner && cover, "obscured host fixture was not created");
    if (!root || !host || !inner || !cover) {
        if (root) DestroyWindow(root);
        return;
    }
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.cx = 120;
    column.pszText = const_cast<LPWSTR>(L"Printer");
    ListView_InsertColumn(cover, 0, &column);
    SetWindowPos(cover, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ShowWindow(root, SW_SHOWNOACTIVATE);
    Check(Translation::ObscuredBySiblingAbove(host) && Translation::InsideObscuredHost(root, inner) &&
        !Translation::ObscuredBySiblingAbove(cover),
        "a host covered but for its edges by a sibling above was not recognized");
    Translation::CaptureContext context;
    context.surfaceId = L"71717171-2222-4444-8888-a1a1a1a1a1a1";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    const bool captured = Translation::CaptureWindow(root, context, snapshot, error);
    if (!captured) std::wcerr << L"obscured host capture: " << error << L'\n';
    const auto has = [&](HWND window) {
        return std::any_of(snapshot.nodes.begin(), snapshot.nodes.end(),
            [window](const auto& node) { return node.hwnd == window; });
    };
    Check(captured && has(cover) && !has(host) && !has(inner),
        "an obscured host's unsupported content refused the window instead of dropping the host");
    // A wide uncovered band keeps the host in the capture, and with it the refusal.
    SetWindowPos(cover, nullptr, 12, 12, 296, 150, SWP_NOZORDER | SWP_NOACTIVATE);
    Check(!Translation::ObscuredBySiblingAbove(host) && !Translation::InsideObscuredHost(root, inner),
        "a host with a wide visible band was treated as obscured");
    context.revision = 2;
    Check(!Translation::CaptureWindow(root, context, snapshot, error),
        "unsupported content in a visible host was dropped instead of refused");
    DestroyWindow(root);
}

void TestListViewHeaderPlacementFlag() {
    // Windows Forms sets HDF_BITMAP_ON_RIGHT on every column header; without HDF_BITMAP
    // or HDF_IMAGE it draws nothing, so the list stays admitted.  A real header image
    // keeps its refusal.
    const HWND root = CreateWindowExW(0, L"Static", L"header-placement", WS_OVERLAPPEDWINDOW,
        40, 40, 360, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const HWND list = root ? CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT,
        10, 10, 320, 180, root, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(root && list, "header placement fixture was not created");
    if (!root || !list) {
        if (root) DestroyWindow(root);
        return;
    }
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.cx = 120;
    column.pszText = const_cast<LPWSTR>(L"Level");
    ListView_InsertColumn(list, 0, &column);
    const HWND header = ListView_GetHeader(list);
    const auto setFormat = [&](int format) {
        HDITEMW item{};
        item.mask = HDI_FORMAT;
        item.fmt = format;
        return Header_SetItem(header, 0, &item) != FALSE;
    };
    const auto capture = [&]() {
        Translation::ControlNode node;
        node.kind = Translation::ControlKind::ListView;
        node.hwnd = list;
        std::wstring reason;
        return Translation::CaptureControlDetail(list, node, reason);
    };
    Check(setFormat(HDF_STRING | HDF_BITMAP_ON_RIGHT) && capture(),
        "a header placement flag with no picture refused the list");
    Check(setFormat(HDF_STRING | HDF_IMAGE | HDF_BITMAP_ON_RIGHT) && !capture(),
        "a header image was admitted although the projection does not draw it");
    DestroyWindow(root);
}

void TestVirtualListWindow() {
    // A large virtual report list (Event Viewer's logs) is carried as a window of rows
    // around its native top row, with absolute indices for selection and actions.
    FluentShell::Tests::OwnerDataListView::Fixture fixture(LVS_REPORT);
    Check(fixture.ready, "virtual list window fixture was not created");
    if (!fixture.ready) return;
    SetWindowLongPtrW(fixture.list, GWL_STYLE,
        GetWindowLongPtrW(fixture.list, GWL_STYLE) & ~static_cast<LONG_PTR>(LVS_EDITLABELS));
    fixture.rows.clear();
    for (int index = 0; index < 10000; ++index)
        fixture.rows.push_back({ L"Event " + std::to_wstring(index), index % 2 ? L"Error" : L"Information" });
    fixture.SetCount();
    ShowWindow(fixture.root, SW_SHOWNOACTIVATE);
    ListView_SetItemState(fixture.list, 9000, LVIS_SELECTED, LVIS_SELECTED);

    Translation::ControlNode node;
    std::wstring error;
    const int readsBefore = fixture.textReads;
    const bool captured = fixture.Capture(node, error);
    if (!captured) std::wcerr << L"virtual list window: " << error << L'\n';
    Check(captured && node.itemCount == 10000 && node.itemWindowStart == 0 &&
        node.items.size() == node.rows.size() && node.items.size() > 0 && node.items.size() < 1000 &&
        node.items.front() == L"Event 0" && Translation::ListViewItemCount(node) == 10000,
        "a large virtual list was not carried as a window at its top row");
    Check(fixture.textReads - readsBefore < 2000,
        "a windowed capture read rows outside its window");
    Check(node.selectedIndices == std::vector<int>{ 9000 },
        "a selection outside the carried rows lost its absolute index");

    Check(Translation::ScrollListViewToTop(fixture.list, 5000) &&
        ListView_GetTopIndex(fixture.list) == 5000, "the native list did not scroll its top row to the request");
    Translation::ControlNode scrolled;
    Check(fixture.Capture(scrolled, error) && scrolled.itemWindowStart < 5000 &&
        scrolled.itemWindowStart + scrolled.items.size() > 5000 &&
        scrolled.items[5000 - scrolled.itemWindowStart] == L"Event 5000" &&
        *Translation::ListViewItemLabel(scrolled, 5000) == L"Event 5000" &&
        !Translation::ListViewItemLabel(scrolled, 0),
        "the window did not follow the native top row");

    // Actions keep absolute indices and witness only rows the window carries.
    Translation::WindowSnapshot snapshot;
    scrolled.nodeId = 71;
    scrolled.visible = scrolled.enabled = true;
    snapshot.nodes.push_back(scrolled);
    Translation::ActionRequest action;
    action.nodeId = 71;
    action.action = L"contextMenu";
    action.itemIndex = 5000;
    action.text = L"Event 5000";
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "a context menu on a carried row of a windowed list was refused");
    action.itemIndex = 0;
    action.text = L"Event 0";
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "a context menu on a row outside the window was admitted without a witness");
    action.action = L"setTopIndex";
    action.text.clear();
    action.itemIndex = 9999;
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "setTopIndex was refused for a row of the windowed list");
    action.itemIndex = 10000;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "setTopIndex past the end of the list was admitted");
    action.action = L"setFocusedIndex";
    action.itemIndex = 9999;
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "a focus request on a row of the windowed list was refused");

    snapshot.surfaceId = L"5a5a5a5a-1111-2222-3333-444444444444";
    snapshot.generation = 1;
    snapshot.revision = 3;
    const auto payload = Translation::SerializeWindowOpen(L"00112233445566778899aabbccddeeff", snapshot);
    const auto wire = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(payload))
        .GetNamedObject(L"window").GetNamedArray(L"nodes").GetObjectAt(0);
    Check(wire.GetNamedNumber(L"itemCount") == 10000 &&
        wire.GetNamedNumber(L"itemWindowStart") == static_cast<double>(scrolled.itemWindowStart),
        "a windowed list did not carry its count and window start on the wire");

    // A short virtual list is still carried whole.
    fixture.rows.resize(40);
    fixture.SetCount();
    Translation::ControlNode shortList;
    Check(fixture.Capture(shortList, error) && shortList.itemCount == 0 && shortList.itemWindowStart == 0 &&
        shortList.items.size() == 40, "a short virtual list was windowed");
}

void TestEmptyReportListWithoutColumns() {
    // Event Viewer's Subscriptions list has no columns until the collector service
    // answers, and a report list with no columns paints nothing.  With no rows it is
    // carried as an empty list; rows without columns stay refused.
    const HWND root = CreateWindowExW(0, L"Static", L"no-columns", WS_OVERLAPPEDWINDOW,
        40, 40, 360, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const HWND list = root ? CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT,
        10, 10, 320, 180, root, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(root && list, "zero-column list fixture was not created");
    if (!root || !list) {
        if (root) DestroyWindow(root);
        return;
    }
    const auto capture = [&](Translation::ControlNode& node, std::wstring& reason) {
        node = {};
        node.kind = Translation::ControlKind::ListView;
        node.hwnd = list;
        reason.clear();
        return Translation::CaptureControlDetail(list, node, reason);
    };
    Translation::ControlNode node;
    std::wstring reason;
    Check(capture(node, reason) && node.listViewMode == L"list" && node.items.empty() &&
        node.columns.empty() && node.rows.empty() && node.itemRects.empty() && !node.columnHeadersVisible,
        "an empty report list with no columns was not carried as an empty list");
    if (node.listViewMode != L"list") std::wcerr << L"zero-column list: " << reason << L'\n';

    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.pszText = const_cast<LPWSTR>(L"row");
    ListView_InsertItem(list, &item);
    Check(!capture(node, reason) && reason.find(L"no native report columns") != std::wstring::npos,
        "a report list with rows but no columns was admitted");

    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.cx = 100;
    column.pszText = const_cast<LPWSTR>(L"Name");
    ListView_InsertColumn(list, 0, &column);
    Check(capture(node, reason) && node.listViewMode == L"report" && node.columns.size() == 1 &&
        node.rows.size() == 1, "a list that gained a column was not carried as a report");
    DestroyWindow(root);
}

void TestMmcBusyShieldIdentity() {
    // Only mmcndmgr.dll's class is MMC's busy shield; a lookalike an application
    // registers under the same name neither disables the projected frame nor
    // disappears from the node tree.
    WNDCLASSW type{};
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"MMCWaitCursorWindow";
    const bool registered = RegisterClassW(&type) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    const HWND root = CreateWindowExW(0, L"Static", L"busy-shield", WS_OVERLAPPEDWINDOW,
        40, 40, 300, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const HWND shield = root ? CreateWindowExW(WS_EX_TRANSPARENT, L"MMCWaitCursorWindow", L"",
        WS_CHILD | WS_VISIBLE, 0, 0, 280, 160, root, nullptr, GetModuleHandleW(nullptr), nullptr) : nullptr;
    Check(registered && root && shield, "busy shield lookalike was not created");
    if (root && shield) {
        Check(!Translation::IsMmcBusyShield(shield) && !Translation::HasVisibleMmcBusyShield(root) &&
            !Translation::IsCompositeImplementationChild(shield),
            "an application-registered MMCWaitCursorWindow was treated as MMC's busy shield");
    }
    if (root) DestroyWindow(root);
}
