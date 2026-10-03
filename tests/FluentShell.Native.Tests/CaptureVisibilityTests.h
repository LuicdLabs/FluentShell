#pragma once

// A BS_ICON push button projects its icon in place of its text, which stays the
// accessible name; one with no icon set is refused rather than shown blank.
void TestIconButtonCapture() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } owner{ CreateWindowExW(0, L"#32770", L"icon button", WS_POPUP | WS_CAPTION,
        40, 40, 240, 120, nullptr, nullptr, instance, nullptr) };
    const HWND root = owner.window;
    const HWND button = root ? CreateWindowExW(0, L"Button", L"Browse", WS_CHILD | WS_VISIBLE |
        WS_TABSTOP | BS_PUSHBUTTON | BS_ICON, 10, 10, 40, 40, root, reinterpret_cast<HMENU>(41),
        instance, nullptr) : nullptr;
    HICON icon = LoadIconW(nullptr, IDI_INFORMATION);
    Check(root && button && icon, "icon button fixture could not create its windows");
    if (!root || !button || !icon) return;
    ShowWindow(root, SW_SHOWNOACTIVATE);
    Translation::CaptureContext context;
    context.surfaceId = L"a2a2a2a2-3b3b-4c4c-5d5d-6e6e6e6e6e6e";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot blank;
    std::wstring reason;
    Check(!Translation::CaptureWindow(root, context, blank, reason) &&
        reason.find(L"no current HICON") != std::wstring::npos,
        "an icon Button with no icon was projected blank");
    SendMessageW(button, BM_SETIMAGE, IMAGE_ICON, reinterpret_cast<LPARAM>(icon));
    Translation::WindowSnapshot snapshot;
    const bool captured = Translation::CaptureWindow(root, context, snapshot, reason);
    if (!captured) std::wcerr << L"icon button capture: " << reason << L"\n";
    const auto node = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
        [button](const auto& candidate) { return candidate.hwnd == button; });
    Check(captured && node != snapshot.nodes.end() && node->kind == Translation::ControlKind::Button &&
        node->imageWidth > 0 && !node->imageData.empty() && node->automationName == L"Browse",
        "an icon Button lost its picture or its accessible name");
    const std::string serialized = Translation::SerializeWindowOpen(L"00112233445566778899aabbccddeeff", snapshot);
    Check(serialized.find("\"imageData\":") != std::string::npos,
        "an icon Button's picture did not reach the wire");
}

// A real wizard-mode property sheet keeps a visible tab control whose items have no
// rectangles at all.  It draws nothing, so capture leaves it out instead of refusing
// the whole wizard for a "malformed" tab row.
INT_PTR CALLBACK EmptyWizardPageProc(HWND, UINT, WPARAM, LPARAM) { return FALSE; }

void TestWizardSheetTabStripIsNotCaptured() {
    // One empty dialog page, built in memory: DLGTEMPLATE, then empty menu, class
    // and title arrays.
    alignas(DWORD) std::array<WORD, 32> page{};
    auto* header = reinterpret_cast<DLGTEMPLATE*>(page.data());
    header->style = WS_CHILD | DS_CONTROL;
    header->cx = 200;
    header->cy = 100;
    PROPSHEETPAGEW sheetPage{};
    sheetPage.dwSize = sizeof(sheetPage);
    sheetPage.dwFlags = PSP_DLGINDIRECT;
    sheetPage.hInstance = GetModuleHandleW(nullptr);
    sheetPage.pResource = header;
    sheetPage.pfnDlgProc = EmptyWizardPageProc;
    HPROPSHEETPAGE pages[2] = { CreatePropertySheetPageW(&sheetPage), CreatePropertySheetPageW(&sheetPage) };
    PROPSHEETHEADERW sheet{};
    sheet.dwSize = sizeof(sheet);
    sheet.dwFlags = PSH_WIZARD | PSH_MODELESS;
    sheet.hInstance = GetModuleHandleW(nullptr);
    sheet.pszCaption = L"wizard";
    sheet.nPages = 2;
    sheet.phpage = pages;
    const HWND wizard = pages[0] && pages[1]
        ? reinterpret_cast<HWND>(PropertySheetW(&sheet)) : nullptr;
    Check(wizard && IsWindow(wizard), "wizard property sheet fixture could not be created");
    if (!wizard || !IsWindow(wizard)) return;
    ShowWindow(wizard, SW_SHOWNOACTIVATE);

    HWND tabs = nullptr;
    EnumChildWindows(wizard, [](HWND child, LPARAM found) -> BOOL {
        wchar_t name[32]{};
        GetClassNameW(child, name, 32);
        if (_wcsicmp(name, WC_TABCONTROLW) == 0 && IsWindowVisible(child)) {
            *reinterpret_cast<HWND*>(found) = child;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&tabs));
    Translation::CaptureContext context;
    context.surfaceId = L"f1f1f1f1-2a2a-3b3b-4c4c-5d5d5d5d5d5d";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring reason;
    const bool captured = Translation::CaptureWindow(wizard, context, snapshot, reason);
    if (!captured) std::wcerr << L"wizard sheet capture: " << reason << L"\n";
    Check(tabs != nullptr, "the wizard sheet fixture exposed no tab control");
    Check(captured && std::none_of(snapshot.nodes.begin(), snapshot.nodes.end(),
        [tabs](const auto& node) { return node.hwnd == tabs; }),
        "a wizard sheet's covered tab strip was captured or refused the wizard");
    DestroyWindow(wizard);
}

// An SS_BITMAP Static (wizard side art) is captured at the size it occupies, beyond
// the 96-pixel icon cap, and replacing its bitmap changes the fingerprint.
void TestStaticBitmapCapture() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } owner{ CreateWindowExW(0, L"#32770", L"wizard", WS_POPUP | WS_CAPTION | WS_SYSMENU,
        40, 40, 420, 260, nullptr, nullptr, instance, nullptr) };
    const HWND root = owner.window;
    const auto solidBitmap = [](COLORREF color) {
        HDC screen = GetDC(nullptr);
        HBITMAP bitmap = CreateCompatibleBitmap(screen, 120, 150);
        HDC memory = CreateCompatibleDC(screen);
        HGDIOBJ previous = SelectObject(memory, bitmap);
        HBRUSH brush = CreateSolidBrush(color);
        RECT all{ 0, 0, 120, 150 };
        FillRect(memory, &all, brush);
        DeleteObject(brush);
        SelectObject(memory, previous);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return bitmap;
    };
    HBITMAP first = solidBitmap(RGB(0x20, 0x60, 0xA0));
    const HWND picture = root ? CreateWindowExW(0, L"Static", L"", WS_CHILD | WS_VISIBLE | SS_BITMAP,
        8, 8, 0, 0, root, reinterpret_cast<HMENU>(12), instance, nullptr) : nullptr;
    Check(root && picture && first, "static bitmap fixture could not create its windows");
    if (!root || !picture || !first) {
        if (first) DeleteObject(first);
        return;
    }
    SendMessageW(picture, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(first));
    ShowWindow(root, SW_SHOWNOACTIVATE);
    UpdateWindow(root);

    Translation::CaptureContext context;
    context.surfaceId = L"d1d1d1d1-2e2e-3f3f-4040-515151515151";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring reason;
    const bool captured = Translation::CaptureWindow(root, context, snapshot, reason);
    if (!captured) std::wcerr << L"static bitmap capture: " << reason << L"\n";
    const auto node = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
        [picture](const auto& candidate) { return candidate.hwnd == picture; });
    Check(captured && node != snapshot.nodes.end() &&
        node->kind == Translation::ControlKind::StaticBitmap &&
        node->imageWidth == 120 && node->imageHeight == 150 &&
        node->imageFormat == L"bgra8-premultiplied" && node->imageData.size() == 120u * 150u * 4u,
        "an SS_BITMAP larger than the icon cap was not captured at its painted size");
    if (node != snapshot.nodes.end() && node->imageData.size() >= 4) {
        Check(node->imageData[0] == 0xA0 && node->imageData[1] == 0x60 &&
            node->imageData[2] == 0x20 && node->imageData[3] == 0xff,
            "the static bitmap's painted pixels were not carried as opaque BGRA");
    }
    const std::string serialized = Translation::SerializeWindowOpen(L"00112233445566778899aabbccddeeff", snapshot);
    Check(serialized.find("\"kind\":\"staticBitmap\"") != std::string::npos &&
        serialized.find("\"imageWidth\":120") != std::string::npos,
        "the static bitmap did not reach the wire with its pixels");

    HBITMAP second = solidBitmap(RGB(0xC0, 0x10, 0x10));
    HBITMAP replaced = reinterpret_cast<HBITMAP>(SendMessageW(
        picture, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(second)));
    if (replaced) DeleteObject(replaced);
    context.revision = 2;
    Translation::WindowSnapshot repainted;
    Check(Translation::CaptureWindow(root, context, repainted, reason) &&
        Translation::SnapshotFingerprint(repainted) != Translation::SnapshotFingerprint(snapshot),
        "a replaced static bitmap did not change the snapshot fingerprint");

    // SS_CENTERIMAGE centers the bitmap and fills the rest of the control with the
    // colour of the bitmap's top-left pixel.
    HBITMAP marked = solidBitmap(RGB(0x20, 0x60, 0xA0));
    if (marked) {
        HDC screen = GetDC(nullptr);
        HDC memory = CreateCompatibleDC(screen);
        HGDIOBJ previous = SelectObject(memory, marked);
        SetPixel(memory, 0, 0, RGB(0x00, 0xFF, 0x00));
        SelectObject(memory, previous);
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
    }
    const HWND centered = CreateWindowExW(0, L"Static", L"",
        WS_CHILD | WS_VISIBLE | SS_BITMAP | SS_CENTERIMAGE,
        200, 8, 160, 190, root, reinterpret_cast<HMENU>(13), instance, nullptr);
    if (centered && marked)
        SendMessageW(centered, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(marked));
    context.revision = 3;
    Translation::WindowSnapshot withCentered;
    const bool centeredCaptured = centered && marked &&
        Translation::CaptureWindow(root, context, withCentered, reason);
    const auto centeredNode = std::find_if(withCentered.nodes.begin(), withCentered.nodes.end(),
        [centered](const auto& candidate) { return candidate.hwnd == centered; });
    const auto pixelAt = [&](int x, int y) {
        const size_t offset = (static_cast<size_t>(y) * centeredNode->imageWidth + x) * 4;
        return std::array<uint8_t, 4>{ centeredNode->imageData[offset], centeredNode->imageData[offset + 1],
            centeredNode->imageData[offset + 2], centeredNode->imageData[offset + 3] };
    };
    Check(centeredCaptured && centeredNode != withCentered.nodes.end() &&
        centeredNode->imageWidth == 160 && centeredNode->imageHeight == 190 &&
        pixelAt(0, 0) == std::array<uint8_t, 4>{ 0x00, 0xFF, 0x00, 0xFF } &&
        pixelAt(20, 20) == std::array<uint8_t, 4>{ 0x00, 0xFF, 0x00, 0xFF } &&
        pixelAt(80, 95) == std::array<uint8_t, 4>{ 0xA0, 0x60, 0x20, 0xFF },
        "a centered SS_BITMAP was not centered over its top-left pixel fill");
    DestroyWindow(root);
    owner.window = nullptr;
    if (second) DeleteObject(second);
    if (marked) DeleteObject(marked);
}

// Include after main.cpp's Check helper, inside its test namespace.
//
// A WS_VISIBLE child whose ancestors clip it away entirely draws nothing natively
// (msinfo32 parks one outside its pane), so capture skips it like a hidden window;
// a partly visible child stays, and a minimized root keeps all of its children.
void TestFullyClippedChildIsNotCaptured() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } owner{ CreateWindowExW(0, L"#32770", L"clipping", WS_POPUP | WS_CAPTION | WS_SYSMENU,
        40, 40, 420, 240, nullptr, nullptr, instance, nullptr) };
    const HWND root = owner.window;
    const auto child = [&](HWND parent, int x, int y, int width, int height, int id) {
        return parent ? CreateWindowExW(0, L"Static", L"label", WS_CHILD | WS_VISIBLE | SS_LEFT,
            x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            instance, nullptr) : nullptr;
    };
    const HWND inside = child(root, 10, 10, 120, 20, 1);
    const HWND partial = child(root, 380, 10, 120, 20, 2);
    const HWND outside = child(root, 1200, 10, 120, 20, 3);
    const HWND empty = child(root, 10, 60, 0, 20, 4);
    Check(root && inside && partial && outside && empty, "clipping fixture could not create its windows");
    if (!root || !inside || !partial || !outside || !empty) return;
    ShowWindow(root, SW_SHOWNOACTIVATE);

    const auto capture = [&](Translation::WindowSnapshot& snapshot) {
        Translation::CaptureContext context;
        context.surfaceId = L"c1c1c1c1-2d2d-3e3e-4f4f-505050505050";
        context.generation = 1;
        context.revision = 1;
        std::wstring reason;
        const bool captured = Translation::CaptureWindow(root, context, snapshot, reason);
        if (!captured) std::wcerr << L"clipping capture: " << reason << L"\n";
        return captured;
    };
    const auto has = [](const Translation::WindowSnapshot& snapshot, HWND window) {
        return std::any_of(snapshot.nodes.begin(), snapshot.nodes.end(),
            [window](const auto& node) { return node.hwnd == window; });
    };
    Translation::WindowSnapshot snapshot;
    Check(capture(snapshot), "a window with a clipped child was refused");
    Check(has(snapshot, inside) && has(snapshot, partial),
        "a visible or partly visible child was dropped from the capture");
    Check(!has(snapshot, outside) && !has(snapshot, empty),
        "a child the native window clips away entirely was captured");

    ShowWindow(root, SW_SHOWMINNOACTIVE);
    Translation::WindowSnapshot minimized;
    if (IsIconic(root)) {
        Check(capture(minimized) && has(minimized, inside) && has(minimized, partial),
            "minimizing the root dropped its children from the capture");
    }
}
