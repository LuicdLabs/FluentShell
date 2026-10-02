#pragma once

#include "../../src/Bridge/Translation/ControlAdapters.h"
#include "../../src/Bridge/Translation/WindowCapture.h"
#include "ItemImageCaptureTests.h"

#include <array>
#include <limits>
#include <string>
#include <vector>

namespace FluentShell::Tests {
namespace ListViewModes {

using namespace Bridge::Translation;

struct Fixture final {
    HWND root = nullptr;
    HWND list = nullptr;
    HIMAGELIST large = nullptr;
    HIMAGELIST smallImages = nullptr;
    bool ready = false;

    explicit Fixture(DWORD mode) {
        root = CreateWindowExW(0, L"Static", L"list-view-mode-regression",
            WS_OVERLAPPEDWINDOW, 0, 0, 440, 260, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!root) return;
        list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE |
            LVS_SHAREIMAGELISTS | LVS_EDITLABELS | mode, 0, 0, 400, 210,
            root, reinterpret_cast<HMENU>(991), GetModuleHandleW(nullptr), nullptr);
        if (!list) return;
        large = ImageList_Create(32, 32, ILC_COLOR32 | ILC_MASK, 1, 1);
        smallImages = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 1, 1);
        if (!large || !smallImages) return;
        HICON largeIcon = ItemImageCapture::NativeIndexIcon(32, 31);
        HICON smallIcon = ItemImageCapture::NativeIndexIcon(16, 15);
        const int largeIndex = largeIcon ? ImageList_AddIcon(large, largeIcon) : -1;
        const int smallIndex = smallIcon ? ImageList_AddIcon(smallImages, smallIcon) : -1;
        if (largeIcon) DestroyIcon(largeIcon);
        if (smallIcon) DestroyIcon(smallIcon);
        if (largeIndex != 0 || smallIndex != 0) return;
        SendMessageW(list, LVM_SETIMAGELIST, LVSIL_NORMAL, reinterpret_cast<LPARAM>(large));
        SendMessageW(list, LVM_SETIMAGELIST, LVSIL_SMALL, reinterpret_cast<LPARAM>(smallImages));
        // Real native columns are inserted even in icon mode, so capture must
        // deliberately leave them out while that mode is not displaying them.
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<LPWSTR>(L"Native report column");
        column.cx = 180;
        if (SendMessageW(list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column)) != 0) return;
        for (int index = 0; index < 2; ++index) {
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_IMAGE;
            item.iItem = index;
            item.pszText = const_cast<LPWSTR>(index == 0 ? L"First" : L"Second");
            item.iImage = 0;
            if (SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)) != index) return;
        }
        ListView_SetExtendedListViewStyleEx(list, LVS_EX_CHECKBOXES, LVS_EX_CHECKBOXES);
        ListView_SetCheckState(list, 1, TRUE);
        ListView_SetItemState(list, 1, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ready = true;
    }

    ~Fixture() {
        if (root) DestroyWindow(root);
        if (large) ImageList_Destroy(large);
        if (smallImages) ImageList_Destroy(smallImages);
    }

    bool Capture(ControlNode& node, std::wstring& error) const {
        node.kind = ControlKind::ListView;
        node.nodeId = 71;
        node.generation = 1;
        node.hwnd = list;
        node.style = static_cast<uint64_t>(GetWindowLongPtrW(list, GWL_STYLE));
        return CaptureControlDetail(list, node, error);
    }

    void SetMode(DWORD mode) const {
        const auto style = GetWindowLongPtrW(list, GWL_STYLE);
        SetWindowLongPtrW(list, GWL_STYLE, (style & ~LVS_TYPEMASK) | mode);
        // Modern controls keep a current view independently of the legacy
        // style mask. Exercise the documented mode setter as an MMC view menu
        // does; legacy controls retain the style change when this is unsupported.
        SendMessageW(list, LVM_SETVIEW, mode, 0);
        SetWindowPos(list, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
};

inline void TestMode(DWORD mode, const wchar_t* name, void (*check)(bool, const char*)) {
    Fixture fixture(mode);
    check(fixture.ready, "non-report ListView fixture was not created");
    if (!fixture.ready) return;
    if (mode == LVS_ICON || mode == LVS_SMALLICON) {
        // Exact geometry admits manual placement, including a partially clipped
        // item, without inventing an AUTOARRANGE restriction.
        POINT first{ -18, 24 };
        POINT second{ 270, 170 };
        SendMessageW(fixture.list, LVM_SETITEMPOSITION32, 0, reinterpret_cast<LPARAM>(&first));
        SendMessageW(fixture.list, LVM_SETITEMPOSITION32, 1, reinterpret_cast<LPARAM>(&second));
    }
    ControlKind kind{};
    std::wstring error;
    check(ClassifyControl(fixture.list, kind, error) && kind == ControlKind::ListView,
        "a native non-report ListView mode was rejected by admission");
    ControlNode node;
    const bool captured = fixture.Capture(node, error);
    if (!captured) std::wcerr << L"ListView mode capture " << name << L": " << error << L'\n';
    check(captured, "non-report ListView state capture failed");
    if (!captured) return;
    check(node.listViewMode == name && node.columns.empty() && node.columnWidths.empty() &&
        node.columnOrder.empty() && node.rows.empty() && !node.columnHeadersVisible,
        "non-report ListView was represented using hidden or synthetic report columns");
    check(node.items == std::vector<std::wstring>{ L"First", L"Second" } &&
        ListViewItemCount(node) == 2 && node.selectedIndices == std::vector<int>{1} &&
        node.focusedIndex == 1 && node.checkedIndices == std::vector<int>{1} &&
        node.checkBoxes && node.editableLabels,
        "non-report ListView lost canonical items, selection, checks or editing capability");
    check(node.itemRects.size() == 2, "non-report ListView omitted native item geometry");
    for (size_t index = 0; index < node.itemRects.size(); ++index) {
        RECT actual{ LVIR_BOUNDS };
        check(SendMessageW(fixture.list, LVM_GETITEMRECT, index,
            reinterpret_cast<LPARAM>(&actual)) && EqualRect(&actual, &node.itemRects[index]),
            "non-report item geometry was reflowed instead of preserving its native rectangle");
    }
    const uint32_t dimension = mode == LVS_ICON ? 32 : 16;
    const uint8_t marker = mode == LVS_ICON ? 31 : 15;
    check(node.imageList.size() == 1 && node.itemImages == std::vector<int>{0, 0} &&
        node.imageList[0].imageWidth == dimension && node.imageList[0].imageHeight == dimension &&
        !node.imageList[0].imageData.empty() && node.imageList[0].imageData[0] == marker,
        "ListView mode selected the wrong native normal/small image list");
    for (size_t index = 0; index < node.itemNativeIds.size(); ++index)
        check(ResolveListViewItemByNativeId(fixture.list, node.itemNativeIds[index]) ==
            static_cast<int>(index), "captured ListView native ID did not resolve to its item");
    check(!node.itemActivationSupported || node.itemNativeIds.size() == node.items.size(),
        "ListView advertised activation without stable native IDs");

    WindowSnapshot snapshot;
    snapshot.nodes.push_back(node);
    const auto json = SerializeWindowOpen(L"00112233445566778899aabbccddeeff", snapshot);
    check(json.find("\"itemRects\":[") != std::string::npos &&
        json.find("\"listViewMode\":") != std::string::npos &&
        json.find("\"itemActivationSupported\":") != std::string::npos &&
        json.find("itemNativeIds") == std::string::npos,
        "ListView mode/geometry/capability wire contract omitted fields or leaked native IDs");
    ActionRequest action;
    action.nodeId = node.nodeId;
    action.action = L"setSelection";
    action.integerValues = { 1 };
    check(ValidateActionForSnapshot(action, snapshot, error),
        "non-report selection was bounded by empty report rows");
    action.action = L"setItemText";
    action.itemIndex = 1;
    action.text = L"Renamed";
    check(ValidateActionForSnapshot(action, snapshot, error),
        "non-report rename was bounded by empty report rows");
    action.action = L"setItemCheck";
    check(ValidateActionForSnapshot(action, snapshot, error),
        "non-report checkbox action was bounded by empty report rows");
    action.action = L"setColumnOrder";
    action.integerValues.clear();
    check(!ValidateActionForSnapshot(action, snapshot, error),
        "non-report ListView accepted a report-column reordering action");

    // Recapturing the same node across a native mode switch must clear the
    // previous layout payload in both directions.
    fixture.SetMode(LVS_REPORT);
    ListView_SetItemState(fixture.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetItemState(fixture.list, 0, LVIS_SELECTED | LVIS_FOCUSED,
        LVIS_SELECTED | LVIS_FOCUSED);
    const bool reportCaptured = fixture.Capture(node, error);
    if (!reportCaptured) std::wcerr << L"ListView report recapture: " << error << L'\n';
    check(reportCaptured && node.listViewMode == L"report" &&
        node.rows.size() == 2 && node.columns.size() == 1 && node.itemRects.empty(),
        "report recapture retained non-report geometry or duplicated items");
    check(reportCaptured && node.selectedIndices == std::vector<int>{0} &&
        node.selectedIndex == 0 && node.focusedIndex == 0,
        "report recapture appended stale selection instead of replacing it");
    fixture.SetMode(mode);
    ListView_SetItemState(fixture.list, -1, 0, LVIS_SELECTED);
    const bool modeCaptured = fixture.Capture(node, error);
    if (!modeCaptured) std::wcerr << L"ListView non-report recapture " << name << L": " << error << L'\n';
    check(modeCaptured && node.listViewMode == name &&
        node.items.size() == 2 && node.rows.empty() && node.columns.empty() && node.itemRects.size() == 2,
        "non-report recapture retained report columns or duplicated items");
    check(modeCaptured && node.selectedIndices.empty() && node.selectedIndex == -1,
        "non-report recapture retained selection after native deselection");
}

inline LRESULT CALLBACK TileViewEvidence(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR, DWORD_PTR) {
    if (message == LVM_GETVIEW) return LV_VIEW_TILE;
    return DefSubclassProc(window, message, wParam, lParam);
}

inline void TestAdmissionAndActivation(void (*check)(bool, const char*)) {
    Fixture fixture(LVS_ICON);
    check(fixture.ready, "ListView admission fixture was not created");
    if (!fixture.ready) return;
    ControlKind kind{};
    std::wstring error;
    for (const DWORD flag : { LVS_OWNERDATA, LVS_OWNERDRAWFIXED }) {
        const HWND unsupported = CreateWindowExW(0, WC_LISTVIEWW, L"",
            WS_CHILD | LVS_ICON | flag, 0, 0, 200, 100, fixture.root, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        check(unsupported && !ClassifyControl(unsupported, kind, error) &&
            error.find(flag == LVS_OWNERDATA ? L"LVS_OWNERDATA" : L"LVS_OWNERDRAWFIXED") !=
                std::wstring::npos,
            "ListView owner-data/owner-draw rejection lost its separate diagnostic");
        if (unsupported) DestroyWindow(unsupported);
    }
    SetWindowSubclass(fixture.list, TileViewEvidence, 700, 0);
    check(!ClassifyControl(fixture.list, kind, error) && error.find(L"tile") != std::wstring::npos,
        "tile view was admitted as a legacy icon view");
    RemoveWindowSubclass(fixture.list, TileViewEvidence, 700);

    const std::wstring nonce = L"00112233445566778899aabbccddeeff";
    const std::string prefix = "{\"messageType\":\"action.invoke\",\"sessionNonce\":\"00112233445566778899aabbccddeeff\","
        "\"surfaceId\":\"4f17d4bb-b2bf-42b8-a334-2f9ad8d54d42\",\"nodeId\":\"71\","
        "\"eventId\":\"1\",\"expectedRevision\":\"7\",\"action\":\"activateItem\",\"value\":";
    ActionRequest action;
    check(ParseActionInvoke(prefix + "1}", nonce, action, error) && action.itemIndex == 1,
        "canonical ListView activation action was rejected");
    for (const char* invalid : { "-1}", "1.5}", "4096}", "\"1\"}", "null}" })
        check(!ParseActionInvoke(prefix + invalid, nonce, action, error),
            "malformed or out-of-bounds ListView activation index was admitted");
    check(!IsRequestSemanticAction(L"activateItem"),
        "numeric ListView activation was incorrectly made revision-rebased");

    ControlNode node;
    check(fixture.Capture(node, error), "activation validation fixture capture failed");
    WindowSnapshot snapshot;
    node.itemNativeIds = { 101, 102 };
    node.itemActivationSupported = true;
    snapshot.nodes = { node };
    action.nodeId = node.nodeId;
    action.action = L"activateItem";
    action.itemIndex = 1;
    check(ValidateActionForSnapshot(action, snapshot, error),
        "ListView activation rejected a valid item and explicit native capability");
    action.itemIndex = 2;
    check(!ValidateActionForSnapshot(action, snapshot, error),
        "ListView activation accepted an index beyond its item labels");
    action.itemIndex = 1;
    snapshot.nodes[0].itemActivationSupported = false;
    check(!ValidateActionForSnapshot(action, snapshot, error),
        "ListView activation ignored the absence of a native default action");
    snapshot.nodes[0].itemActivationSupported = true;
    snapshot.nodes[0].enabled = false;
    check(!ValidateActionForSnapshot(action, snapshot, error),
        "ListView activation accepted a disabled native control");
    snapshot.nodes[0].enabled = true;
    const auto fingerprint = SnapshotFingerprint(snapshot);
    snapshot.nodes[0].itemNativeIds[1] = 999;
    check(SnapshotFingerprint(snapshot) != fingerprint,
        "same-label native ListView replacement did not invalidate snapshot identity");
}

} // namespace ListViewModes

inline void TestListViewModes(void (*check)(bool, const char*)) {
    ListViewModes::TestMode(LVS_ICON, L"largeIcon", check);
    ListViewModes::TestMode(LVS_SMALLICON, L"smallIcon", check);
    ListViewModes::TestMode(LVS_LIST, L"list", check);
    ListViewModes::TestAdmissionAndActivation(check);
}

} // namespace FluentShell::Tests
