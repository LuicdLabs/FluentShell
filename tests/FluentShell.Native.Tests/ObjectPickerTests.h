#pragma once

// Include after main.cpp's Check helper and ListViewActivationTests.h, inside its test namespace.

// The object picker (regedit's Connect Network Registry, "Select Computer") is three things the
// capture refused: a link whose surrounding text names the next box's access key, an editable
// RichEdit that underlines the names it resolved, and the size grip of a resizable dialog.
void TestObjectPickerControls() {
    ListActivationCommonControls controls(ICC_LINK_CLASS | ICC_STANDARD_CLASSES);
    Check(static_cast<bool>(controls), "object picker v6 activation failed");
    if (!controls) return;
    Check(LoadLibraryW(L"Msftedit.dll") != nullptr, "the RichEdit library could not be loaded");
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } dialog{ CreateWindowExW(0, WC_DIALOG, L"object-picker", WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
        60, 60, 420, 220, nullptr, nullptr, instance, nullptr) };
    const auto child = [&](const wchar_t* type, const wchar_t* text, DWORD style, RECT rect, int id) {
        return dialog.window ? CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
            rect.left, rect.top, rect.right, rect.bottom, dialog.window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr) : nullptr;
    };
    const HWND link = child(WC_LINK, L"Enter the object name (<A>examples</A>)(&E):", WS_TABSTOP,
        { 10, 10, 360, 20 }, 126);
    const HWND names = child(MSFTEDIT_CLASS, L"", WS_TABSTOP | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL,
        { 10, 36, 360, 60 }, 214);
    const HWND grip = child(L"ScrollBar", L"", SBS_SIZEGRIP | SBS_SIZEBOXBOTTOMRIGHTALIGN,
        { 380, 150, 17, 17 }, 207);
    Check(dialog.window && link && names && grip, "object picker fixture could not create its controls");
    if (!dialog.window || !link || !names || !grip) return;
    ShowWindow(dialog.window, SW_SHOWNOACTIVATE);

    const auto capture = [&](Translation::WindowSnapshot& snapshot, std::wstring& error) {
        Translation::CaptureContext context;
        context.surfaceId = L"77777777-7777-7777-7777-acacacacacac";
        context.generation = 1;
        context.revision = 1;
        return Translation::CaptureWindow(dialog.window, context, snapshot, error);
    };
    const auto nodeFor = [](const Translation::WindowSnapshot& snapshot, int controlId) {
        const auto found = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
            [&](const Translation::ControlNode& node) { return node.controlId == controlId; });
        return found == snapshot.nodes.end() ? nullptr : &*found;
    };
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    const bool captured = capture(snapshot, error);
    if (!captured) std::wcerr << L"object picker capture rejection: " << error << L'\n';
    Check(captured, "the object picker's controls kept the dialog native");
    if (!captured) return;
    const auto* linkNode = nodeFor(snapshot, 126);
    Check(linkNode && linkNode->kind == Translation::ControlKind::SysLink &&
          linkNode->text == L"Enter the object name (examples)(E):" &&
          linkNode->items == std::vector<std::wstring>{ L"examples" },
        "the link's surrounding mnemonic text was not carried as the text it shows");
    const auto* namesNode = nodeFor(snapshot, 214);
    Check(namesNode && namesNode->kind == Translation::ControlKind::Edit && !namesNode->readOnly &&
          namesNode->multiline,
        "the editable RichEdit was not projected as an edit box");
    Check(!nodeFor(snapshot, 207), "the dialog's size grip was captured as a control");

    // A resolved name is underlined: still an edit box.
    SetWindowTextW(names, L"DESKTOP-01; server");
    CHARFORMAT2W format{};
    format.cbSize = sizeof(format);
    format.dwMask = CFM_UNDERLINE;
    format.dwEffects = CFE_UNDERLINE;
    SendMessageW(names, EM_SETSEL, 0, 10);
    SendMessageW(names, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
    Check(capture(snapshot, error) && nodeFor(snapshot, 214) &&
          nodeFor(snapshot, 214)->kind == Translation::ControlKind::Edit &&
          nodeFor(snapshot, 214)->text == L"DESKTOP-01; server",
        "an editable RichEdit with an underlined name stopped being an edit box");
    // Bold would be lost through a plain edit box: the window stays native.
    format.dwMask = CFM_BOLD;
    format.dwEffects = CFE_BOLD;
    SendMessageW(names, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
    Check(!capture(snapshot, error) && error.find(L"editable RichEdit") != std::wstring::npos,
        "an editable RichEdit with bold text was projected as plain text");
}
