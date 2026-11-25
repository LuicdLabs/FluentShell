#pragma once

// Include in main.cpp's test namespace after Check(). The shell's Properties sheet (Explorer's
// "<folder> - Properties"), rebuilt where it matters from what ShellPropertiesProbe.h read off a real
// SHObjectProperties sheet.

std::atomic<unsigned> g_tabItemDataRequests{0};

LRESULT CALLBACK TabItemDataWitness(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR, DWORD_PTR) {
    // A property sheet's tab control carries TCM_SETITEMEXTRA bytes per item, and TCM_GETITEM with
    // TCIF_PARAM copies all of them into the caller's TCITEM.lParam -- past the end of a TCITEMW.
    if ((message == TCM_GETITEMW || message == TCM_GETITEMA) && lParam &&
        (reinterpret_cast<const TCITEMW*>(lParam)->mask & TCIF_PARAM))
        g_tabItemDataRequests.fetch_add(1);
    return DefSubclassProc(window, message, wParam, lParam);
}

void TestPropertySheetTabItemDataIsNeverRequested() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } dialog{ CreateWindowExW(0x00050101, WC_DIALOG, L"dev - Properties", 0x94C800C4,
        40, 40, 420, 520, nullptr, nullptr, instance, nullptr) };
    const HWND tab = dialog.window ? CreateWindowExW(WS_EX_NOPARENTNOTIFY, WC_TABCONTROLW, L"", 0x54030240,
        7, 8, 405, 467, dialog.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(12320)), instance, nullptr) : nullptr;
    Check(dialog.window && tab, "property sheet tab fixture could not be created");
    if (!dialog.window || !tab) return;
    // comctl32's property sheet stores a whole record per tab this way.
    constexpr size_t kExtra = 64;
    Check(SendMessageW(tab, TCM_SETITEMEXTRA, kExtra, 0) != FALSE, "TCM_SETITEMEXTRA was refused");
    struct ItemWithExtra final {
        TCITEMHEADERW header;
        BYTE extra[kExtra];
    };
    const wchar_t* labels[] = { L"General", L"Sharing", L"Security", L"Previous Versions", L"Customize" };
    for (int index = 0; index < static_cast<int>(std::size(labels)); ++index) {
        ItemWithExtra item{};
        item.header.mask = TCIF_TEXT | TCIF_PARAM;
        item.header.pszText = const_cast<LPWSTR>(labels[index]);
        std::fill(std::begin(item.extra), std::end(item.extra), static_cast<BYTE>(0xA5));
        Check(SendMessageW(tab, TCM_INSERTITEMW, index, reinterpret_cast<LPARAM>(&item)) == index,
            "property sheet tab could not be inserted");
    }
    SendMessageW(tab, TCM_SETCURSEL, 0, 0);
    SetWindowSubclass(tab, TabItemDataWitness, 1, 0);
    ShowWindow(dialog.window, SW_SHOWNOACTIVATE);

    g_tabItemDataRequests.store(0);
    Translation::CaptureContext context;
    context.surfaceId = L"77777777-7777-7777-7777-5ec0a1e7a131";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    const bool captured = Translation::CaptureWindow(dialog.window, context, snapshot, error);
    if (!captured) std::wcerr << L"property sheet tab capture rejection: " << error << L'\n';
    Check(captured, "a property sheet's tab strip stayed native");
    Check(g_tabItemDataRequests.load() == 0,
        "capture asked a TCM_SETITEMEXTRA tab control for its item data (overruns the caller's TCITEMW)");
    const auto node = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
        [](const Translation::ControlNode& candidate) { return candidate.kind == Translation::ControlKind::TabControl; });
    Check(node != snapshot.nodes.end() && node->items.size() == std::size(labels) && node->items[3] == L"Previous Versions",
        "a property sheet's tab labels were not captured");
    RemoveWindowSubclass(tab, TabItemDataWitness, 1);
}

// The Sharing page's folder thumbnail: a 32-bit premultiplied bitmap with real alpha in an
// SS_BITMAP | SS_CENTERIMAGE | SS_REALSIZEIMAGE static.  comctl32 v6 alpha-blends it; the projection
// must carry its alpha (taken opaque, its transparent pixels showed black) and fill nothing around it.
void TestAlphaStaticBitmapKeepsItsTransparency() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } dialog{ CreateWindowExW(0, WC_DIALOG, L"Sharing", WS_POPUP, 40, 40, 300, 200, nullptr, nullptr, instance, nullptr) };
    const HWND picture = dialog.window ? CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"Static", L"", 0x50000A0E,
        12, 26, 38, 38, dialog.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1021)), instance, nullptr) : nullptr;
    Check(dialog.window && picture, "alpha bitmap fixture could not be created");
    if (!dialog.window || !picture) return;
    constexpr int size = 32;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = -size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    Check(bitmap && bits, "alpha bitmap could not be created");
    if (!bitmap || !bits) return;
    // A premultiplied yellow square inset by 8 px, fully transparent around it.
    auto* pixel = static_cast<uint8_t*>(bits);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x, pixel += 4) {
            const bool inside = x >= 8 && x < 24 && y >= 8 && y < 24;
            pixel[0] = 0; pixel[1] = inside ? 0xC0 : 0; pixel[2] = inside ? 0xF0 : 0; pixel[3] = inside ? 0xFF : 0;
        }
    }
    SendMessageW(picture, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(bitmap));
    ShowWindow(dialog.window, SW_SHOWNOACTIVATE);
    Translation::CaptureContext context;
    context.surfaceId = L"77777777-7777-7777-7777-5ec0a1e7a132";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    const bool captured = Translation::CaptureWindow(dialog.window, context, snapshot, error);
    if (!captured) std::wcerr << L"alpha bitmap capture rejection: " << error << L'\n';
    Check(captured, "a page with an alpha bitmap stayed native");
    const auto node = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
        [](const Translation::ControlNode& candidate) { return candidate.controlId == 1021; });
    Check(node != snapshot.nodes.end() && node->kind == Translation::ControlKind::StaticBitmap &&
          node->imageWidth == 38 && node->imageHeight == 38 && node->imageData.size() == 38u * 38u * 4u,
        "the alpha bitmap was not captured at the control's size");
    if (node != snapshot.nodes.end() && node->imageData.size() == 38u * 38u * 4u) {
        const auto alphaAt = [&](int x, int y) { return node->imageData[(static_cast<size_t>(y) * 38 + x) * 4 + 3]; };
        // Centered: the bitmap's (0,0) lands at (3,3); its square at (11..26, 11..26).
        Check(alphaAt(0, 0) == 0 && alphaAt(5, 5) == 0 && alphaAt(37, 37) == 0,
            "the alpha bitmap's transparent pixels or its surroundings were made opaque");
        Check(alphaAt(18, 18) == 0xFF, "the alpha bitmap's opaque pixels lost their alpha");
    }
    SendMessageW(picture, STM_SETIMAGE, IMAGE_BITMAP, 0);
    DeleteObject(bitmap);
}
