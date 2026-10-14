#pragma once

// Include after main.cpp's Check helper and ListViewActivationTests.h, inside its test namespace.

// A tile-view ListView (the Sound applet's devices): each item is its large icon beside its
// label and the detail lines the tile shows (LVM_GETTILEINFO), at its native position.
void TestTileListViewCapture() {
    ListActivationCommonControls controls(ICC_LISTVIEW_CLASSES);
    Check(static_cast<bool>(controls), "tile ListView v6 activation failed");
    if (!controls) return;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    struct WindowOwner final {
        HWND window;
        ~WindowOwner() { if (window) DestroyWindow(window); }
    } host{ CreateWindowExW(0, L"Static", L"tile-list", WS_OVERLAPPEDWINDOW,
        40, 40, 520, 360, nullptr, nullptr, instance, nullptr) };
    const HWND list = host.window ? CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_ICON | LVS_SINGLESEL, 10, 10, 480, 300, host.window,
        reinterpret_cast<HMENU>(420), instance, nullptr) : nullptr;
    Check(list != nullptr, "tile ListView fixture could not create its control");
    if (!list) return;
    const HIMAGELIST icons = ImageList_Create(32, 32, ILC_COLOR32 | ILC_MASK, 1, 1);
    Check(icons && ImageList_AddIcon(icons, LoadIconW(nullptr, IDI_APPLICATION)) == 0,
        "tile ListView fixture could not build its icons");
    SendMessageW(list, LVM_SETIMAGELIST, LVSIL_NORMAL, reinterpret_cast<LPARAM>(icons));
    for (int column = 0; column < 3; ++column) {
        LVCOLUMNW header{};
        header.mask = LVCF_TEXT | LVCF_WIDTH;
        header.cx = 120;
        header.pszText = const_cast<LPWSTR>(column == 0 ? L"Name" : column == 1 ? L"Kind" : L"State");
        SendMessageW(list, LVM_INSERTCOLUMNW, column, reinterpret_cast<LPARAM>(&header));
    }
    struct Device final { const wchar_t* name; const wchar_t* kind; const wchar_t* state; };
    const Device devices[] = { { L"Speakers", L"Realtek Audio", L"Default Device" },
                               { L"Headphones", L"USB Audio", L"Ready" } };
    for (int row = 0; row < 2; ++row) {
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = row;
        item.iImage = 0;
        item.pszText = const_cast<LPWSTR>(devices[row].name);
        SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
        LVITEMW subitem{};
        subitem.iSubItem = 1;
        subitem.pszText = const_cast<LPWSTR>(devices[row].kind);
        SendMessageW(list, LVM_SETITEMTEXTW, row, reinterpret_cast<LPARAM>(&subitem));
        subitem.iSubItem = 2;
        subitem.pszText = const_cast<LPWSTR>(devices[row].state);
        SendMessageW(list, LVM_SETITEMTEXTW, row, reinterpret_cast<LPARAM>(&subitem));
        UINT shown[] = { 1, 2 };
        LVTILEINFO tile{};
        tile.cbSize = sizeof(tile);
        tile.iItem = row;
        tile.cColumns = 2;
        tile.puColumns = shown;
        SendMessageW(list, LVM_SETTILEINFO, 0, reinterpret_cast<LPARAM>(&tile));
    }
    Check(SendMessageW(list, LVM_SETVIEW, LV_VIEW_TILE, 0) == 1, "the fixture list did not enter tile view");
    ShowWindow(host.window, SW_SHOWNOACTIVATE);

    Translation::CaptureContext context;
    context.surfaceId = L"77777777-7777-7777-7777-aeaeaeaeaeae";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    const bool captured = Translation::CaptureWindow(host.window, context, snapshot, error);
    if (!captured) std::wcerr << L"tile ListView capture rejection: " << error << L'\n';
    const auto node = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
        [](const Translation::ControlNode& candidate) { return candidate.controlId == 420; });
    Check(captured && node != snapshot.nodes.end() && node->listViewMode == L"tile" &&
          node->items == std::vector<std::wstring>{ L"Speakers", L"Headphones" } &&
          node->itemDetails.size() == 2 &&
          node->itemDetails[0] == std::vector<std::wstring>{ L"Realtek Audio", L"Default Device" } &&
          node->itemDetails[1] == std::vector<std::wstring>{ L"USB Audio", L"Ready" } &&
          node->itemRects.size() == 2 && node->rows.empty() && !node->imageList.empty(),
        "a tile ListView did not carry its labels, detail lines, positions, and large icons");
    const auto json = Translation::SerializeWindowOpen(L"00112233445566778899aabbccddeeff", snapshot);
    Check(json.find("\"listViewMode\":\"tile\"") != std::string::npos &&
          json.find("\"itemDetails\":[[\"Realtek Audio\",\"Default Device\"]") != std::string::npos,
        "a tile ListView's detail lines were not serialized");

    // A badge the control composites on one item's icon (the default device's check) gives that
    // item an icon of its own.
    if (icons && ImageList_SetOverlayImage(icons, 0, 1)) {
        LVITEMW badge{};
        badge.stateMask = LVIS_OVERLAYMASK;
        badge.state = INDEXTOOVERLAYMASK(1);
        SendMessageW(list, LVM_SETITEMSTATE, 0, reinterpret_cast<LPARAM>(&badge));
        context.revision = 2;
        const bool badged = Translation::CaptureWindow(host.window, context, snapshot, error);
        const auto again = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
            [](const Translation::ControlNode& candidate) { return candidate.controlId == 420; });
        Check(badged && again != snapshot.nodes.end() && again->itemImages.size() == 2 &&
              again->itemImages[0] >= 0 && again->itemImages[1] >= 0 &&
              again->itemImages[0] != again->itemImages[1] && again->imageList.size() == 2,
            "a ListView item's overlay badge was not composited into its own icon");
    }
    if (icons) ImageList_Destroy(icons);
}
