#pragma once

// Include after main.cpp's Check helper and ListViewActivationTests.h, inside its test namespace.

// Explorer's "Open File - Security Warning" (shell32, shown in explorer.exe when a file carrying the
// mark of the web is opened), rebuilt with the class, id and style bits Dump-WindowTree.ps1 read from
// the live dialog. Global apply's Explorer exception exists for this dialog, so the whole of it must
// capture: the name and publisher fields are SysLinks with no link and LWS_NOPREFIX.
void TestSecurityWarningDialog() {
    ListActivationCommonControls controls(ICC_LINK_CLASS | ICC_STANDARD_CLASSES);
    Check(static_cast<bool>(controls), "security warning v6 activation failed");
    if (!controls) return;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } dialog{ CreateWindowExW(0x00010101, WC_DIALOG, L"Open File - Security Warning", 0x84C80AC4,
        60, 60, 480, 317, nullptr, nullptr, instance, nullptr) };
    // The styles below already carry WS_CHILD, and WS_VISIBLE wherever the live control was visible.
    const auto child = [&](const wchar_t* type, const wchar_t* text, DWORD style, RECT rect, int id,
                           DWORD exStyle = WS_EX_NOPARENTNOTIFY) {
        return dialog.window ? CreateWindowExW(exStyle, type, text, style,
            rect.left, rect.top, rect.right, rect.bottom, dialog.window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr) : nullptr;
    };
    const HWND handles[] = {
        child(L"Static", L"The publisher could not be verified. Are you sure you want to run this software?",
            0x50000080, { 26, 18, 429, 38 }, 4417),
        child(L"Static", L"", 0x50000003, { 26, 55, 32, 32 }, 4418),
        child(L"Static", L"Name:", 0x50000082, { 83, 55, 56, 23 }, 65535),
        child(WC_LINK, L"...ers\\User\\Downloads\\reg-add-winpedwm\\main.bat", 0x50000004, { 150, 55, 301, 23 }, 4419),
        child(L"Static", L"Publisher:", 0x50000082, { 80, 78, 60, 23 }, 4420),
        child(WC_LINK, L"Unknown Publisher", 0x50000004, { 150, 78, 301, 23 }, 4421),
        child(L"Static", L"Type:", 0x50000082, { 83, 100, 56, 23 }, 4422),
        child(L"Static", L"Windows Batch File", 0x5000008C, { 150, 100, 301, 23 }, 4423),
        child(L"Static", L"From:", 0x50004082, { 83, 123, 56, 23 }, 4424),
        child(L"Static", L"C:\\Users\\User\\Downloads\\reg-add-winpedwm\\main.bat", 0x5000008C,
            { 150, 123, 301, 23 }, 4425),
        child(L"Button", L"&Run", 0x50010000, { 260, 162, 91, 26 }, 4426),
        child(L"Button", L"&Save", 0x48010000, { 260, 162, 91, 26 }, 4427),
        child(L"Button", L"Cancel", 0x50010001, { 360, 162, 91, 26 }, IDCANCEL),
        child(L"Button", L"Al&ways ask before opening this file", 0x58012003, { 26, 207, 257, 19 }, 4428),
        child(L"Static", L"", 0x50001010, { 8, 216, 462, 2 }, 4429, WS_EX_NOPARENTNOTIFY | WS_EX_STATICEDGE),
        child(L"Static", L"", 0x50000003, { 26, 234, 32, 32 }, 4430),
        child(WC_LINK, L"This file does not have a valid digital signature that verifies its publisher. "
            L"You should only run software from publishers you trust. <A>How can I decide what software to run?</A>",
            0x50010000, { 78, 235, 375, 30 }, 4431),
    };
    const bool created = dialog.window && std::all_of(std::begin(handles), std::end(handles),
        [](HWND window) { return window != nullptr; });
    Check(created, "security warning fixture could not create its controls");
    if (!created) return;
    const HICON shield = LoadIconW(nullptr, IDI_SHIELD);
    SendDlgItemMessageW(dialog.window, 4418, STM_SETICON, reinterpret_cast<WPARAM>(LoadIconW(nullptr, IDI_APPLICATION)), 0);
    SendDlgItemMessageW(dialog.window, 4430, STM_SETICON, reinterpret_cast<WPARAM>(shield), 0);
    CheckDlgButton(dialog.window, 4428, BST_CHECKED);
    ShowWindow(dialog.window, SW_SHOWNOACTIVATE);

    Translation::CaptureContext context;
    context.surfaceId = L"77777777-7777-7777-7777-5ec0a1e7a121";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    const bool captured = Translation::CaptureWindow(dialog.window, context, snapshot, error);
    if (!captured) std::wcerr << L"security warning capture rejection: " << error << L'\n';
    Check(captured, "Explorer's security warning stayed native");
    if (!captured) return;
    const auto nodeFor = [&](int controlId) {
        const auto found = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
            [&](const Translation::ControlNode& node) { return node.controlId == controlId; });
        return found == snapshot.nodes.end() ? nullptr : &*found;
    };
    using Translation::ControlKind;
    const auto* name = nodeFor(4419);
    const auto* publisher = nodeFor(4421);
    Check(name && name->kind == ControlKind::StaticText &&
          name->text == L"...ers\\User\\Downloads\\reg-add-winpedwm\\main.bat" &&
          publisher && publisher->kind == ControlKind::StaticText && publisher->text == L"Unknown Publisher",
        "the name and publisher fields were not carried as their text");
    const auto* run = nodeFor(4426);
    const auto* cancel = nodeFor(IDCANCEL);
    Check(run && run->kind == ControlKind::Button && cancel && cancel->kind == ControlKind::Button,
        "Run and Cancel were not carried as buttons");
    const auto* ask = nodeFor(4428);
    Check(ask && ask->kind == ControlKind::CheckBox && ask->checked,
        "the always-ask check box was not carried with its state");
    const auto* help = nodeFor(4431);
    Check(help && help->kind == ControlKind::SysLink &&
          help->items == std::vector<std::wstring>{ L"How can I decide what software to run?" },
        "the help link was not carried as a link");
    Check(nodeFor(4418) && nodeFor(4418)->kind == ControlKind::StaticIcon &&
          nodeFor(4430) && nodeFor(4430)->kind == ControlKind::StaticIcon,
        "the file and shield icons were not carried");
    Check(nodeFor(4429) && nodeFor(4429)->kind == ControlKind::Separator, "the etched rule was not carried");
    Check(!nodeFor(4427) || !nodeFor(4427)->visible, "the hidden Save button was shown");
}
