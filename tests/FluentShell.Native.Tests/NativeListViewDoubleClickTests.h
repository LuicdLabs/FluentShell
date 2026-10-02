#pragma once

// Included after ListViewActivationTests.h inside main.cpp's test namespace.
// This observes stock comctl32 using the production activation point reader.
// No WM_GETOBJECT provider or application-owned WM_NOTIFY handler substitutes
// for the control's own processing of the second double-click message pair.
namespace NativeListViewDoubleClick {

struct Notification final {
    UINT code = 0;
    int item = -1;
    int subItem = -1;
    POINT point{};
    UINT sourceMessage = 0;
    bool sourceThread = false;
};

struct Fixture final {
    HWND root = nullptr;
    HWND list = nullptr;
    HIMAGELIST largeImages = nullptr;
    HIMAGELIST smallImages = nullptr;
    DWORD thread = GetCurrentThreadId();
    UINT currentMouseMessage = 0;
    bool suppressLabelRectangle = false;
    unsigned suppressedLabelRectangles = 0;
    std::vector<UINT> mouseMessages;
    std::vector<Notification> notifications;

    ~Fixture() {
        if (root) DestroyWindow(root);
        if (largeImages) ImageList_Destroy(largeImages);
        if (smallImages) ImageList_Destroy(smallImages);
    }
};

LRESULT CALLBACK ParentSubclass(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR subclassId, DWORD_PTR data) {
    auto& fixture = *reinterpret_cast<Fixture*>(data);
    if (message == WM_NOTIFY && lParam) {
        const auto& header = *reinterpret_cast<const NMHDR*>(lParam);
        if (header.hwndFrom == fixture.list &&
            (header.code == NM_DBLCLK || header.code == LVN_ITEMACTIVATE || header.code == NM_CLICK)) {
            const auto& item = *reinterpret_cast<const NMITEMACTIVATE*>(lParam);
            fixture.notifications.push_back({header.code, item.iItem, item.iSubItem,
                item.ptAction, fixture.currentMouseMessage, GetCurrentThreadId() == fixture.thread});
        }
    }
    const auto result = DefSubclassProc(window, message, wParam, lParam);
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, ParentSubclass, subclassId);
    return result;
}

LRESULT CALLBACK ListSubclass(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR subclassId, DWORD_PTR data) {
    auto& fixture = *reinterpret_cast<Fixture*>(data);
    // Hide only the label rectangle during point resolution to exercise the
    // production icon fallback. All hit-tests and mouse dispatch stay native.
    if (fixture.suppressLabelRectangle && message == LVM_GETITEMRECT &&
        wParam == 1 && lParam && reinterpret_cast<const RECT*>(lParam)->left == LVIR_LABEL) {
        ++fixture.suppressedLabelRectangles;
        return FALSE;
    }
    const bool mouse = message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK || message == WM_LBUTTONUP;
    const UINT previous = fixture.currentMouseMessage;
    if (mouse) {
        fixture.mouseMessages.push_back(message);
        fixture.currentMouseMessage = message;
    }
    const auto result = DefSubclassProc(window, message, wParam, lParam);
    if (mouse) fixture.currentMouseMessage = previous;
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, ListSubclass, subclassId);
    return result;
}

bool Create(Fixture& fixture, DWORD mode) {
    fixture.root = CreateWindowExW(0, L"Static", L"native-list-double-click-diagnostic",
        WS_OVERLAPPEDWINDOW, 40, 40, 540, 320, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!fixture.root || !SetWindowSubclass(fixture.root, ParentSubclass, 0xAC41,
            reinterpret_cast<DWORD_PTR>(&fixture))) return false;
    const BOOL cloak = TRUE;
    if (FAILED(DwmSetWindowAttribute(fixture.root, DWMWA_CLOAK, &cloak, sizeof(cloak)))) return false;
    fixture.list = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_SHAREIMAGELISTS | LVS_SINGLESEL | mode,
        0, 0, 500, 260, fixture.root, reinterpret_cast<HMENU>(1401), GetModuleHandleW(nullptr), nullptr);
    if (!fixture.list || !SetWindowSubclass(fixture.list, ListSubclass, 0xAC42,
            reinterpret_cast<DWORD_PTR>(&fixture))) return false;
    fixture.largeImages = ImageList_Create(32, 32, ILC_COLOR32 | ILC_MASK, 1, 1);
    fixture.smallImages = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 1, 1);
    const HICON icon = LoadIconW(nullptr, IDI_INFORMATION);
    if (!fixture.largeImages || !fixture.smallImages || !icon ||
        ImageList_AddIcon(fixture.largeImages, icon) != 0 ||
        ImageList_AddIcon(fixture.smallImages, icon) != 0) return false;
    SendMessageW(fixture.list, LVM_SETIMAGELIST, LVSIL_NORMAL, reinterpret_cast<LPARAM>(fixture.largeImages));
    SendMessageW(fixture.list, LVM_SETIMAGELIST, LVSIL_SMALL, reinterpret_cast<LPARAM>(fixture.smallImages));
    SendMessageW(fixture.list, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = const_cast<LPWSTR>(L"Name");
    column.cx = 220;
    if (SendMessageW(fixture.list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column)) != 0) return false;
    for (int index = 0; index < 2; ++index) {
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = index;
        item.pszText = const_cast<LPWSTR>(index == 0 ? L"Other component" : L"Activation target");
        item.iImage = 0;
        if (SendMessageW(fixture.list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)) != index) return false;
    }
    ListView_SetItemState(fixture.list, 1, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ShowWindow(fixture.root, SW_SHOWNOACTIVATE);
    UpdateWindow(fixture.root);
    return SendMessageW(fixture.list, LVM_GETVIEW, 0, 0) == mode &&
        static_cast<UINT>(SendMessageW(fixture.list, LVM_MAPINDEXTOID, 1, 0)) != UINT_MAX;
}

bool ReadActivationPoint(Fixture& fixture, int rectangleKind, POINT& point, UINT& flags) {
    fixture.suppressLabelRectangle = rectangleKind == LVIR_ICON;
    const bool found = Translation::ReadListViewNativeActivationPoint(fixture.list, 1, point);
    fixture.suppressLabelRectangle = false;
    if (!found) return false;
    // Recheck the returned point against the unmodified stock control. Report
    // labels include empty column space, which must never count as a text hit.
    LVHITTESTINFO hit{};
    hit.pt = point;
    const int index = static_cast<int>(SendMessageW(fixture.list, LVM_HITTEST,
        static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(&hit)));
    flags = hit.flags;
    return index == 1 && hit.iItem == 1 && hit.iSubItem == 0 &&
        (hit.flags & (rectangleKind == LVIR_LABEL ? LVHT_ONITEMLABEL : LVHT_ONITEMICON)) != 0 &&
        (hit.flags & LVHT_ONITEMSTATEICON) == 0;
}

void Observe(DWORD mode, const wchar_t* modeName, int rectangleKind) {
    // Each point gets a fresh control, so a previous double-click cannot prime
    // private state that the first-pair-free sequence under observation lacks.
    Fixture fixture;
    const bool created = Create(fixture, mode);
    Check(created, "native double-click diagnostic could not create a cloaked v6 ListView");
    if (!created) return;
    POINT point{};
    UINT flags = 0;
    const bool hit = ReadActivationPoint(fixture, rectangleKind, point, flags);
    Check(fixture.suppressedLabelRectangles == (rectangleKind == LVIR_ICON ? 1u : 0u),
        "native activation point test did not exercise the requested label or icon fallback path");
    Check(hit, "native double-click diagnostic found no exact first-column label/icon hit");
    if (!hit) return;
    const auto nativeId = SendMessageW(fixture.list, LVM_MAPINDEXTOID, 1, 0);
    const HWND foreground = GetForegroundWindow();
    const HWND captureBefore = GetCapture();
    fixture.mouseMessages.clear();
    fixture.notifications.clear();
    const LPARAM position = MAKELPARAM(static_cast<SHORT>(point.x), static_cast<SHORT>(point.y));
    // Selection and item focus are already canonical, exactly as they are when
    // the projected ListView's deferred activation reaches the source thread.
    SendMessageW(fixture.list, WM_LBUTTONDBLCLK, MK_LBUTTON, position);
    const HWND captureAfterDoubleClick = GetCapture();
    SendMessageW(fixture.list, WM_LBUTTONUP, 0, position);
    const auto count = [&](UINT code) {
        return std::count_if(fixture.notifications.begin(), fixture.notifications.end(),
            [&](const Notification& value) { return value.code == code; });
    };
    const bool exact = std::all_of(fixture.notifications.begin(), fixture.notifications.end(),
        [&](const Notification& value) {
            return value.item == 1 && value.subItem == 0 && value.point.x == point.x && value.point.y == point.y &&
                value.sourceThread && value.sourceMessage != 0;
        });
    Check(exact, "stock ListView activation notification did not name the exact native hit on its owning thread");
    const bool stable = SendMessageW(fixture.list, LVM_MAPINDEXTOID, 1, 0) == nativeId &&
        SendMessageW(fixture.list, LVM_GETSELECTEDCOUNT, 0, 0) == 1 &&
        SendMessageW(fixture.list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_FOCUSED) == 1 &&
        SendMessageW(fixture.list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED) == 1;
    Check(count(NM_DBLCLK) == 1 && count(LVN_ITEMACTIVATE) == 1 && count(NM_CLICK) == 0,
        "stock ListView did not produce exactly one double-click activation without a first click pair");
    Check(fixture.mouseMessages == std::vector<UINT>{WM_LBUTTONDBLCLK, WM_LBUTTONUP},
        "stock ListView double-click test sent or generated an unexpected mouse message sequence");
    Check(stable && captureAfterDoubleClick == captureBefore &&
        GetCapture() == captureBefore && GetForegroundWindow() == foreground,
        "stock ListView double-click changed canonical selection, identity, capture or foreground");
    std::wcout << L"Native ListView DBLCLK/UP diagnostic mode=" << modeName
        << L" part=" << (rectangleKind == LVIR_LABEL ? L"label" : L"icon")
        << L" point=" << point.x << L"," << point.y << L" hitFlags=" << flags
        << L" NM_DBLCLK=" << count(NM_DBLCLK) << L" LVN_ITEMACTIVATE=" << count(LVN_ITEMACTIVATE)
        << L" NM_CLICK=" << count(NM_CLICK) << L" mouseMessages=" << fixture.mouseMessages.size()
        << L" exactItem=" << exact
        << L" stable=" << stable << L" captureAfterDoubleClick=" << (captureAfterDoubleClick == captureBefore)
        << L" captureUnchanged=" << (GetCapture() == captureBefore)
        << L" foregroundUnchanged=" << (GetForegroundWindow() == foreground)
        << L" supported=" << (count(NM_DBLCLK) == 1 && exact && stable) << L'\n';
}

} // namespace NativeListViewDoubleClick

void TestNativeListViewDoubleClickDispatch() {
    std::thread gui([] {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        {
            const ListActivationCommonControls controls;
            Check(static_cast<bool>(controls), "native double-click diagnostic could not activate comctl32 v6");
            if (controls) {
                for (const auto& [mode, name] : std::array<std::pair<DWORD, const wchar_t*>, 4>{{
                        {LVS_ICON, L"largeIcon"}, {LVS_REPORT, L"report"},
                        {LVS_SMALLICON, L"smallIcon"}, {LVS_LIST, L"list"}}}) {
                    NativeListViewDoubleClick::Observe(mode, name, LVIR_LABEL);
                    NativeListViewDoubleClick::Observe(mode, name, LVIR_ICON);
                }
            }
        }
        winrt::uninit_apartment();
    });
    gui.join();
}
