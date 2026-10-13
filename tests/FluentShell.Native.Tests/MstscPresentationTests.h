#pragma once

// Include after main.cpp's Check helper, ListViewActivationTests.h and ComboBoxExTests.h,
// inside its test namespace.

// mstsc's dialog draws two things the projection did not: the "Show Options" label beside its
// toolbar icon, and the grey hint in the empty computer box.

// A TBSTYLE_LIST toolbar draws a button's own label beside its icon; an icon-only list button's
// text is only its name.  TBSTYLE_EX_MIXEDBUTTONS limits the drawn labels to BTNS_SHOWTEXT.
void TestToolbarLabelBeside() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const HIMAGELIST images = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 1, 1);
    Check(images && ImageList_AddIcon(images, LoadIconW(nullptr, IDI_APPLICATION)) == 0,
        "toolbar label fixture could not build its image list");
    const auto capture = [&](DWORD style, DWORD extended, BYTE buttonStyle, Translation::ControlNode& node) {
        struct WindowOwner final {
            HWND window;
            ~WindowOwner() { if (window) DestroyWindow(window); }
        } root{ CreateWindowExW(0, L"Static", L"toolbar-label-beside", WS_OVERLAPPEDWINDOW,
            0, 0, 480, 120, nullptr, nullptr, instance, nullptr) };
        const HWND toolbar = root.window ? CreateWindowExW(0, TOOLBARCLASSNAMEW, L"",
            WS_CHILD | WS_VISIBLE | CCS_NOPARENTALIGN | CCS_NORESIZE | CCS_NODIVIDER | style,
            0, 0, 400, 40, root.window, reinterpret_cast<HMENU>(5017), instance, nullptr) : nullptr;
        if (!toolbar || !images) return false;
        SendMessageW(toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
        SendMessageW(toolbar, CCM_SETUNICODEFORMAT, TRUE, 0);
        SendMessageW(toolbar, TB_SETEXTENDEDSTYLE, 0, extended);
        SendMessageW(toolbar, TB_SETIMAGELIST, 0, reinterpret_cast<LPARAM>(images));
        TBBUTTON button{};
        button.iBitmap = 0;
        button.idCommand = 5014;
        button.fsStyle = static_cast<BYTE>(BTNS_BUTTON | BTNS_AUTOSIZE | buttonStyle);
        button.fsState = TBSTATE_ENABLED;
        button.iString = reinterpret_cast<INT_PTR>(L"Show &Options");
        if (!SendMessageW(toolbar, TB_ADDBUTTONSW, 1, reinterpret_cast<LPARAM>(&button))) return false;
        SendMessageW(toolbar, TB_AUTOSIZE, 0, 0);
        node = Translation::ControlNode{};
        node.kind = Translation::ControlKind::Toolbar;
        node.nodeId = 75;
        node.generation = 1;
        node.hwnd = toolbar;
        node.visible = true;
        node.enabled = true;
        node.style = static_cast<uint64_t>(GetWindowLongPtrW(toolbar, GWL_STYLE));
        std::wstring error;
        const bool captured = Translation::CaptureControlDetail(toolbar, node, error);
        if (!captured) std::wcerr << L"toolbar label capture rejection: " << error << L'\n';
        return captured && node.toolbarItems.size() == 1;
    };
    Translation::ControlNode node;
    Check(capture(TBSTYLE_LIST | TBSTYLE_FLAT, 0, 0, node) && node.toolbarItems[0].labelBeside &&
          node.toolbarItems[0].text == L"Show &Options" && !node.toolbarItems[0].imageData.empty(),
        "a list toolbar's labelled icon button did not carry its label beside the icon");
    Check(capture(TBSTYLE_FLAT, 0, 0, node) && !node.toolbarItems[0].labelBeside,
        "a toolbar that is not list-style claimed a label beside its icon");
    Check(capture(TBSTYLE_LIST | TBSTYLE_FLAT, TBSTYLE_EX_MIXEDBUTTONS, 0, node) &&
          !node.toolbarItems[0].labelBeside,
        "a mixed-buttons toolbar drew the label of a button without BTNS_SHOWTEXT");
    Check(capture(TBSTYLE_LIST | TBSTYLE_FLAT, TBSTYLE_EX_MIXEDBUTTONS, BTNS_SHOWTEXT, node) &&
          node.toolbarItems[0].labelBeside,
        "a mixed-buttons toolbar dropped the label of its BTNS_SHOWTEXT button");
    Translation::WindowSnapshot snapshot;
    snapshot.surfaceId = L"77777777-7777-7777-7777-eeeeeeeeeeee";
    snapshot.nodes.push_back(node);
    const auto json = Translation::SerializeWindowOpen(L"00112233445566778899aabbccddeeff", snapshot);
    Check(json.find("\"labelBeside\":true") != std::string::npos, "labelBeside was not serialized");
    if (images) ImageList_Destroy(images);
}

// The grey hint an empty box shows (common controls 6): an edit's, a ComboBoxEx's (in its own
// edit box), and an editable combo's.  A multiline edit never shows one.
void TestCueBanners() {
    ListActivationCommonControls controls(ICC_STANDARD_CLASSES | ICC_USEREX_CLASSES);
    Check(static_cast<bool>(controls), "cue banner v6 activation failed");
    if (!controls) return;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } host{ CreateWindowExW(0, L"Static", L"cue-banners", WS_OVERLAPPEDWINDOW,
        40, 40, 420, 300, nullptr, nullptr, instance, nullptr) };
    const auto child = [&](const wchar_t* type, DWORD style, int y, int id) {
        return host.window ? CreateWindowExW(0, type, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
            10, y, 260, style & CBS_DROPDOWN ? 200 : 24, host.window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr) : nullptr;
    };
    const HWND edit = child(L"Edit", WS_BORDER | ES_AUTOHSCROLL, 10, 601);
    const HWND multiline = child(L"Edit", WS_BORDER | ES_MULTILINE, 40, 602);
    const HWND comboEx = child(WC_COMBOBOXEXW, CBS_DROPDOWN, 80, 603);
    const HWND combo = child(L"ComboBox", CBS_DROPDOWN, 120, 604);
    Check(edit && multiline && comboEx && combo, "cue banner fixture could not create its controls");
    if (!edit || !multiline || !comboEx || !combo) return;
    SendMessageW(edit, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Example: computer.fabrikam.com"));
    SendMessageW(multiline, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"never shown"));
    SendMessageW(reinterpret_cast<HWND>(SendMessageW(comboEx, CBEM_GETEDITCONTROL, 0, 0)),
        EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Computer"));
    SendMessageW(combo, CB_SETCUEBANNER, 0, reinterpret_cast<LPARAM>(L"Pick or type"));
    ShowWindow(host.window, SW_SHOWNOACTIVATE);

    Translation::CaptureContext context;
    context.surfaceId = L"77777777-7777-7777-7777-ffffffffffff";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    const bool captured = Translation::CaptureWindow(host.window, context, snapshot, error);
    if (!captured) std::wcerr << L"cue banner capture rejection: " << error << L'\n';
    Check(captured, "the cue banner fixture kept its window native");
    if (!captured) return;
    const auto banner = [&](int controlId) {
        const auto found = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
            [&](const Translation::ControlNode& node) { return node.controlId == controlId; });
        return found == snapshot.nodes.end() ? std::wstring(L"<missing>") : found->cueBanner;
    };
    Check(banner(601) == L"Example: computer.fabrikam.com", "an edit box lost its cue banner");
    Check(banner(602).empty(), "a multiline edit carried a cue banner");
    Check(banner(603) == L"Computer", "a ComboBoxEx lost the cue banner of its edit box");
    Check(banner(604) == L"Pick or type", "an editable combo lost its cue banner");
    const auto json = Translation::SerializeWindowOpen(L"00112233445566778899aabbccddeeff", snapshot);
    Check(json.find("\"cueBanner\":\"Computer\"") != std::string::npos, "cueBanner was not serialized");
}

// A dialog that paints a banner above its controls (mstsc's logo and title), and nothing but its
// background below them.
LRESULT CALLBACK BannerDialogSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR) {
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};
        const HDC dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wParam);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(dc, &client, GetSysColorBrush(COLOR_3DFACE));
        RECT banner{ 0, 0, client.right, 60 };
        FillRect(dc, &banner, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        RECT logo{ 12, 12, 48, 48 };
        const HBRUSH blue = CreateSolidBrush(RGB(0, 90, 200));
        FillRect(dc, &logo, blue);
        DeleteObject(blue);
        if (message == WM_PAINT) EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    const LRESULT result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, BannerDialogSubclass, subclassId);
    return result;
}

void TestDialogPaintedBands() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const auto capture = [&](bool banner, Translation::WindowSnapshot& snapshot) {
        struct WindowOwner final {
            HWND window;
            ~WindowOwner() { if (window) DestroyWindow(window); }
        } dialog{ CreateWindowExW(0, WC_DIALOG, L"painted-bands", WS_POPUP | WS_CAPTION | WS_SYSMENU,
            60, 60, 360, 200, nullptr, nullptr, instance, nullptr) };
        if (!dialog.window) return false;
        const HWND button = CreateWindowExW(0, L"Button", L"Connect", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            10, 80, 100, 26, dialog.window, reinterpret_cast<HMENU>(1), instance, nullptr);
        if (!button) return false;
        if (banner) SetWindowSubclass(dialog.window, BannerDialogSubclass, 9, 0);
        ShowWindow(dialog.window, SW_SHOWNOACTIVATE);
        UpdateWindow(dialog.window);
        Translation::CaptureContext context;
        context.surfaceId = L"77777777-7777-7777-7777-abababababab";
        context.generation = 1;
        context.revision = 1;
        std::wstring error;
        const bool captured = Translation::CaptureWindow(dialog.window, context, snapshot, error);
        if (!captured) std::wcerr << L"painted band capture rejection: " << error << L'\n';
        return captured;
    };
    Translation::WindowSnapshot snapshot;
    Check(capture(true, snapshot) && snapshot.paintedBands.size() == 1 &&
          snapshot.paintedBands[0].rect.top == 0 && snapshot.paintedBands[0].rect.left == 0 &&
          snapshot.paintedBands[0].rect.bottom == 78 &&
          snapshot.paintedBands[0].imageWidth == static_cast<uint32_t>(snapshot.paintedBands[0].rect.right) &&
          snapshot.paintedBands[0].imageData.size() ==
              static_cast<size_t>(snapshot.paintedBands[0].imageWidth) * 78u * 4u,
        "a dialog's painted banner above its controls was not carried as one band");
    const auto json = Translation::SerializeWindowOpen(L"00112233445566778899aabbccddeeff", snapshot);
    Check(json.find("\"paintedBands\":[") != std::string::npos, "paintedBands was not serialized");
    Check(capture(false, snapshot) && snapshot.paintedBands.empty(),
        "a dialog that only fills its background carried a painted band");
}
